/*
 * ops.c - struct fuse_operations implemented on top of lwext4.
 *
 * DESIGN
 * ------
 * lwext4 is a *path-based* filesystem. The FUSE high-level API hands us paths
 * for every operation, so mapping is trivial: we mount lwext4 at "/" and use
 * the incoming FUSE path verbatim as the lwext4 path.
 *
 * Thread safety
 * -------------
 * lwext4 is NOT re-entrant by itself. Its public entry points guard their
 * critical sections with a single per-mountpoint OS lock, but ONLY when that
 * lock has been registered with ext4_mount_setup_locks(). We register a
 * *recursive* pthread mutex (g_lw_mtx) for that purpose, and - belt and
 * braces - also take the same recursive mutex around every FUSE operation so
 * that composite operations (e.g. create = open + mode + owner + times) are
 * atomic. Recursive is required because an op-level critical section calls
 * lwext4 functions which take the mp lock again.
 *
 * This makes the front-end safe for macFUSE's multithreaded mode (main no
 * longer passes -s). It does NOT make lwext4 truly parallel: a single global
 * mutex serialises content operations. That is the granularity lwext4 offers.
 *
 * File handles
 * ------------
 * open()/create() allocate a heap struct ext4_file and stash it in
 * fuse_file_info->fh. read/write/ftruncate/fsync/flush/release reuse it and
 * release() frees it. This removes the open+seek+op+close-per-I/O pattern and
 * keeps a coherent fsize/fpos for the lifetime of the handle. Operations that
 * arrive without a handle (kernel-internal reads, stat-only paths) transparently
 * fall back to a temporary handle.
 *
 * Safe mode (read-only by default)
 * --------------------------------
 * The filesystem is read-only unless the user passes "-o rw". main.c parses
 * that flag into the global g_read_write (declared in ops.h) and also asks
 * macFUSE for a read-only kernel mount. Independently of the kernel, every
 * mutating FUSE callback here returns -EROFS when g_read_write is 0, so the
 * policy holds even if the kernel-level "ro" were ever bypassed. lwext4 is
 * still mounted read-write internally (journal replay needs it); the read-only
 * decision lives entirely in this front-end.
 *
 * Inode numbers
 * -------------
 * A small path -> ino table assigns stable synthetic inos (root=1, then
 * sequential). They are mutex-guarded. LIMITATION: these inos are synthetic -
 * stable for the life of a mount but not equal to ext4 on-disk inode numbers.
 *
 * Symlinks / hardlinks
 * --------------------
 * lwext4 provides path-based helpers (see include/ext4.h):
 *   ext4_fsymlink(target, path)   - create a symlink  ("softlink")
 *   ext4_readlink(path, buf, n, &rcnt) - read a symlink payload
 *   ext4_flink(path, hardlink_path)    - create a hardlink
 * These map directly onto FUSE symlink()/readlink()/link(). Short targets are
 * stored inline by lwext4 (fast symlink); longer ones via a data block.
 *
 * Extended attributes
 * -------------------
 * lwext4 exposes path-based xattr helpers (ext4_setxattr/getxattr/listxattr/
 * removexattr) that take fully-qualified names (e.g. "user.foo"). We map ONLY
 * the "user." namespace onto the image: any other namespace (macOS sends
 * com.apple.*, plus security./trusted./system.) is refused so the ext4 image
 * stays clean. Policy:
 *   setxattr/removexattr on non-user.*  -> -ENOTSUP
 *   getxattr on non-user.*              -> -ENOATTR (we never stored it)
 * lwext4 signals "no such attribute" with ENODATA; macOS wants ENOATTR, so
 * that one errno is translated in lw_xattr_errno().
 *
 * Error mapping
 * -------------
 * Every lwext4 return code is turned into the negated host errno the FUSE
 * callbacks must return by the single helper lw_errno() (EOK -> 0, unknown ->
 * EIO). This build has CONFIG_HAVE_OWN_ERRNO == 0, so lwext4 returns the host
 * <errno.h> values and the mapping is the identity, but it is written out
 * explicitly so the contract is auditable. Two front-end compensations for
 * lwext4 behaviour live on top of it:
 *   - lw_write(): lwext4's ext4_fwrite() clobbers its own error code in its
 *     Finish path, so an out-of-space write returns EOK with a short count; a
 *     short write is therefore reported as -ENOSPC (otherwise the kernel
 *     surfaces a bare EIO).
 *   - lw_rmdir(): lwext4's ext4_dir_rm() is RECURSIVE, so we pre-check for
 *     emptiness and return -ENOTEMPTY on a non-empty directory (POSIX rmdir
 *     semantics - and protection against accidental recursive deletion).
 *
 * Timestamps
 * ----------
 * create/mkdir/mknod/symlink stamp atime/mtime/ctime with "now". lwext4's
 * write path does not maintain timestamps, so write/truncate/ftruncate call
 * lw_touch_write() to bump mtime+ctime afterwards - always under the LW_LOCK
 * and only when g_read_write is set (a read-only front-end must never modify
 * the image). atime is intentionally NOT updated on read: that would add an
 * inode write to every read() (and would violate the read-only contract for
 * pure reads); utime/utimens still allow explicit atime control.
 *
 * Not implemented: ioctl, locks, poll, ACLs, resource forks.
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>

#include <ext4.h>
#include <ext4_inode.h>
#include <ext4_super.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "ops.h"

/* ------------------------------------------------------------------ */
/* global mount info + per-mount serialisation lock                   */
/* ------------------------------------------------------------------ */

static struct ext4_sblock *g_sb;

/* Recursive: lwext4's own mp critical sections nest inside our op-level
 * critical sections (and, if a bug path unlocks early, a recursive mutex
 * degrades gracefully instead of corrupting a plain fast mutex). */
static pthread_mutex_t g_lw_mtx;
static pthread_once_t g_lw_once = PTHREAD_ONCE_INIT;

static void lw_mtx_init(void)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_lw_mtx, &a);
	pthread_mutexattr_destroy(&a);
}

/* lwext4 mountpoint lock callbacks (see struct ext4_lock). */
static void lw_mp_lock(void)
{
	pthread_once(&g_lw_once, lw_mtx_init);
	pthread_mutex_lock(&g_lw_mtx);
}

static void lw_mp_unlock(void)
{
	pthread_mutex_unlock(&g_lw_mtx);
}

static const struct ext4_lock g_lw_locks = {
	.lock = lw_mp_lock,
	.unlock = lw_mp_unlock,
};

/* op-level guard (same recursive mutex). */
#define LW_LOCK()                                                              \
	do {                                                                   \
		pthread_once(&g_lw_once, lw_mtx_init);                         \
		pthread_mutex_lock(&g_lw_mtx);                                 \
	} while (0)
#define LW_UNLOCK() pthread_mutex_unlock(&g_lw_mtx)

/*
 * Reject mutations in read-only mode (default). Placed at the top of every
 * mutating FUSE callback; returns -EROFS to the caller. g_read_write is set
 * once by main() before fuse_main(), so it is stable here.
 */
#define LW_RO_GUARD()                                                          \
	do {                                                                   \
		if (!g_read_write)                                             \
			return -EROFS;                                         \
	} while (0)

/* ------------------------------------------------------------------ */
/* lwext4 -> POSIX errno mapping                                      */
/* ------------------------------------------------------------------ */

/*
 * Map a lwext4 return code to the negated errno the FUSE callbacks must
 * return (EOK -> 0).
 *
 * This build of lwext4 has CONFIG_HAVE_OWN_ERRNO == 0, so ext4_errno.h
 * pulls in the host <errno.h> and every lwext4 entry point returns a plain
 * POSIX/host errno. The mapping is therefore the identity for the codes
 * lwext4 actually produces, but we spell it out explicitly so the contract
 * is auditable, and so a code we did not expect never leaks a bogus value to
 * the kernel (unknown codes degrade to EIO). The set below covers every
 * errno ext4_errno.h knows about plus the composite codes an ext4 operation
 * can surface (ENAMETOOLONG, EMLINK, EMFILE, ENFILE, ENOSYS, EXDEV, ...).
 */
static int lw_errno(int r)
{
	switch (r) {
	case EOK:
		return 0;
	case EPERM:
		return -EPERM;
	case ENOENT:
		return -ENOENT;
	case EIO:
		return -EIO;
	case ENXIO:
		return -ENXIO;
	case E2BIG:
		return -E2BIG;
	case ENOMEM:
		return -ENOMEM;
	case EACCES:
		return -EACCES;
	case EFAULT:
		return -EFAULT;
	case EEXIST:
		return -EEXIST;
	case ENODEV:
		return -ENODEV;
	case ENOTDIR:
		return -ENOTDIR;
	case EISDIR:
		return -EISDIR;
	case EINVAL:
		return -EINVAL;
	case EFBIG:
		return -EFBIG;
	case ENOSPC:
		return -ENOSPC;
	case EROFS:
		return -EROFS;
	case EMLINK:
		return -EMLINK;
	case ERANGE:
		return -ERANGE;
	case ENOTEMPTY:
		return -ENOTEMPTY;
	case ENAMETOOLONG:
		return -ENAMETOOLONG;
	case EMFILE:
		return -EMFILE;
	case ENFILE:
		return -ENFILE;
	case ENOSYS:
		return -ENOSYS;
	case EXDEV:
		return -EXDEV;
	case EOVERFLOW:
		return -EOVERFLOW;
	case ELOOP:
		return -ELOOP;
	case ENODATA:
		return -ENODATA;
	case ENOTSUP:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

int lwext4fs_setup_locks(const char *mount_point)
{
	pthread_once(&g_lw_once, lw_mtx_init);
	return ext4_mount_setup_locks(mount_point, &g_lw_locks);
}

void lwext4fs_ops_init(const char *mount_point)
{
	pthread_once(&g_lw_once, lw_mtx_init);
	ext4_get_sblock(mount_point, &g_sb);
}

/* ------------------------------------------------------------------ */
/* path <-> inode-number table                                        */
/* ------------------------------------------------------------------ */

struct ino_node {
	char *path;             /* absolute FUSE/lwext4 path, e.g. "/a/b" */
	ino_t ino;
	struct ino_node *next;
};

static struct ino_node *g_ino_head;
static ino_t g_next_ino = 2;       /* 1 is reserved for root */
static pthread_mutex_t g_ino_lock = PTHREAD_MUTEX_INITIALIZER;

/* Return a stable synthetic ino for the given path, allocating if needed. */
static ino_t lw_ino_for_path(const char *path)
{
	struct ino_node *n;
	ino_t result;

	if (!strcmp(path, "/"))
		return 1;

	pthread_mutex_lock(&g_ino_lock);
	for (n = g_ino_head; n; n = n->next) {
		if (!strcmp(n->path, path)) {
			result = n->ino;
			pthread_mutex_unlock(&g_ino_lock);
			return result;
		}
	}
	n = calloc(1, sizeof(*n));
	if (!n) {
		pthread_mutex_unlock(&g_ino_lock);
		return 0;
	}
	n->path = strdup(path);
	n->ino = g_next_ino++;
	if (!n->path) {
		free(n);
		pthread_mutex_unlock(&g_ino_lock);
		return 0;
	}
	n->next = g_ino_head;
	g_ino_head = n;
	result = n->ino;
	pthread_mutex_unlock(&g_ino_lock);
	return result;
}

/* Drop a synthetic ino mapping (path no longer exists). */
static void lw_ino_forget(const char *path)
{
	struct ino_node **pp, *n;

	if (!strcmp(path, "/"))
		return;

	pthread_mutex_lock(&g_ino_lock);
	for (pp = &g_ino_head; (n = *pp); pp = &n->next) {
		if (!strcmp(n->path, path)) {
			*pp = n->next;
			free(n->path);
			free(n);
			break;
		}
	}
	pthread_mutex_unlock(&g_ino_lock);
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int lw_flush(void)
{
	return ext4_cache_flush("/");
}

static struct ext4_file *lw_fh(struct fuse_file_info *fi)
{
	if (fi && fi->fh)
		return (struct ext4_file *)(uintptr_t)fi->fh;
	return NULL;
}

/*
 * Test whether a directory is empty (no entries besides "." and "..").
 *
 * lwext4's ext4_dir_rm() is RECURSIVE: handed a non-empty directory it removes
 * the whole subtree and returns EOK. That is not POSIX rmdir() semantics, so we
 * check first and fail the operation with ENOTEMPTY ourselves. Calling
 * ext4_dir_rm() only after this check also hardens against accidental data
 * loss. Must be called with LW_LOCK held; returns 0 and sets *empty, or a
 * negative errno.
 */
static int lw_dir_is_empty(const char *path, int *empty)
{
	ext4_dir d;
	const ext4_direntry *de;
	int r;

	*empty = 1;
	r = ext4_dir_open(&d, path);
	if (r != EOK)
		return lw_errno(r);

	while ((de = ext4_dir_entry_next(&d)) != NULL) {
		size_t n = de->name_length;

		if (n == 1 && de->name[0] == '.')
			continue;
		if (n == 2 && de->name[0] == '.' && de->name[1] == '.')
			continue;
		*empty = 0;
		break;
	}

	ext4_dir_close(&d);
	return 0;
}

static void lw_set_times_now(const char *path)
{
	uint32_t now = (uint32_t)time(NULL);

	ext4_atime_set(path, now);
	ext4_mtime_set(path, now);
	ext4_ctime_set(path, now);
}

/*
 * Bump mtime + ctime to "now" after a successful content change.
 *
 * lwext4's own write path does not maintain timestamps, so the front-end does
 * it explicitly once the data write has succeeded (write / truncate /
 * ftruncate). Must be called with LW_LOCK held.
 *
 * Gated on the read-write flag: even though lwext4 is always mounted read-write
 * internally (journal replay needs it), a read-only front-end must never touch
 * the image - so nothing is stamped unless the user opted in with "-o rw".
 * atime is deliberately NOT updated on read (see README): doing so would add an
 * inode write to every read() and would violate the read-only contract for
 * pure reads; callers can still set atime explicitly via utime/utimens.
 */
static void lw_touch_write(const char *path)
{
	uint32_t now;

	if (!g_read_write)
		return;
	now = (uint32_t)time(NULL);
	ext4_mtime_set(path, now);
	ext4_ctime_set(path, now);
}

/*
 * Stat a path via lwext4 and fill a struct stat. If an open handle is given,
 * its (live) fsize is preferred for st_size so that getattr stays consistent
 * with an open file after writes.
 * Returns 0 on success or a negative errno.
 */
static int lw_do_getattr_h(const char *path, struct stat *st,
			   struct ext4_file *f)
{
	uint32_t mode, uid, gid, t;
	uint32_t ino;
	struct ext4_inode raw;

	memset(st, 0, sizeof(*st));

	if (ext4_inode_exist(path, EXT4_DE_UNKNOWN) != EOK)
		return -ENOENT;

	if (ext4_mode_get(path, &mode) != EOK)
		return -ENOENT;

	if (ext4_raw_inode_fill(path, &ino, &raw) != EOK)
		return -ENOENT;

	st->st_mode = mode;
	if (f) {
		st->st_size = (off_t)ext4_fsize(f);
	} else if (g_sb) {
		st->st_size = ext4_inode_get_size(g_sb, &raw);
	} else {
		ext4_file tmp;
		if (ext4_fopen(&tmp, path, "rb") == EOK) {
			st->st_size = (off_t)ext4_fsize(&tmp);
			ext4_fclose(&tmp);
		}
	}
	st->st_nlink = ext4_inode_get_links_cnt(&raw);
	if (st->st_nlink == 0)
		st->st_nlink = 1;
	st->st_ino = lw_ino_for_path(path);

	if (ext4_owner_get(path, &uid, &gid) == EOK) {
		st->st_uid = uid;
		st->st_gid = gid;
	}
	if (ext4_atime_get(path, &t) == EOK)
		st->st_atime = (time_t)t;
	if (ext4_mtime_get(path, &t) == EOK)
		st->st_mtime = (time_t)t;
	if (ext4_ctime_get(path, &t) == EOK)
		st->st_ctime = (time_t)t;

	st->st_blksize = 4096;
	st->st_blocks = (st->st_size + 511) / 512;
	return 0;
}

static int lw_do_getattr(const char *path, struct stat *st)
{
	return lw_do_getattr_h(path, st, NULL);
}

/* ------------------------------------------------------------------ */
/* FUSE operations                                                    */
/* ------------------------------------------------------------------ */

static int lw_getattr(const char *path, struct stat *st)
{
	int r;

	LW_LOCK();
	r = lw_do_getattr(path, st);
	LW_UNLOCK();
	return r;
}

static int lw_fgetattr(const char *path, struct stat *st,
		       struct fuse_file_info *fi)
{
	int r;

	LW_LOCK();
	r = lw_do_getattr_h(path, st, lw_fh(fi));
	LW_UNLOCK();
	return r;
}

static int lw_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		      off_t offset, struct fuse_file_info *fi)
{
	ext4_dir d;
	const ext4_direntry *de;
	char child[4096];
	int r;

	(void)offset;
	(void)fi;

	LW_LOCK();

	/* The kernel wants "." and ".." supplied by us in FUSE 2. */
	if (filler(buf, ".", NULL, 0) || filler(buf, "..", NULL, 0)) {
		LW_UNLOCK();
		return 0;
	}

	r = ext4_dir_open(&d, path);
	if (r != EOK) {
		LW_UNLOCK();
		return lw_errno(r);
	}

	while ((de = ext4_dir_entry_next(&d)) != NULL) {
		char name[256];
		size_t nlen = de->name_length;

		if (nlen == 0 || nlen >= sizeof(name))
			continue;
		memcpy(name, de->name, nlen);
		name[nlen] = '\0';

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		if (strlen(path) + 1 + nlen + 1 > sizeof(child))
			continue;
		if (!strcmp(path, "/"))
			snprintf(child, sizeof(child), "/%s", name);
		else
			snprintf(child, sizeof(child), "%s/%s", path, name);

		{
			struct stat st;
			if (lw_do_getattr(child, &st) == 0) {
				if (filler(buf, name, &st, 0))
					break;
			} else {
				if (filler(buf, name, NULL, 0))
					break;
			}
		}
	}

	ext4_dir_close(&d);
	LW_UNLOCK();
	return 0;
}

/*
 * Open an existing file and cache its lwext4 handle in fi->fh.
 * A same-handle "r+b" is used for every access mode except O_RDONLY: the file
 * already exists so "r+b" is valid, and it lets a delayed write() succeed even
 * if the kernel opened the file O_RDWR.
 */
static int lw_do_open_cached(const char *path, struct fuse_file_info *fi)
{
	struct ext4_file *f;
	const char *mode;
	int r;

	f = calloc(1, sizeof(*f));
	if (!f)
		return -ENOMEM;

	mode = ((fi->flags & O_ACCMODE) == O_RDONLY) ? "rb" : "r+b";
	r = ext4_fopen(f, path, mode);
	if (r != EOK) {
		free(f);
		return lw_errno(r);
	}

	fi->fh = (uint64_t)(uintptr_t)f;
	return 0;
}

static int lw_open(const char *path, struct fuse_file_info *fi)
{
	int r;

	LW_LOCK();
	if (ext4_inode_exist(path, EXT4_DE_UNKNOWN) != EOK)
		r = -ENOENT;
	else if (ext4_inode_exist(path, EXT4_DE_DIR) == EOK)
		r = -EISDIR;
	else
		r = lw_do_open_cached(path, fi);
	LW_UNLOCK();
	return r;
}

static int lw_create(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	struct ext4_file *f;
	int r;

	LW_RO_GUARD();
	LW_LOCK();

	f = calloc(1, sizeof(*f));
	if (!f) {
		LW_UNLOCK();
		return -ENOMEM;
	}

	/* "w+b" == O_RDWR|O_CREAT|O_TRUNC */
	r = ext4_fopen(f, path, "w+b");
	if (r != EOK) {
		free(f);
		LW_UNLOCK();
		return lw_errno(r);
	}

	/* Reasonable default metadata: caller's uid/gid, requested mode. */
	ext4_mode_set(path, mode & 07777);
	ext4_owner_set(path, (uint32_t)getuid(), (uint32_t)getgid());
	lw_set_times_now(path);

	fi->fh = (uint64_t)(uintptr_t)f;

	lw_ino_for_path(path);
	lw_flush();
	LW_UNLOCK();
	return 0;
}

static int lw_read(const char *path, char *buf, size_t size, off_t offset,
		   struct fuse_file_info *fi)
{
	struct ext4_file *f = lw_fh(fi);
	struct ext4_file tmp;
	int borrowed = 0;
	size_t rd = 0;
	int r;

	LW_LOCK();

	if (!f) {
		/* No cached handle: open a temporary one (kernel-internal read). */
		r = ext4_fopen(&tmp, path, "rb");
		if (r != EOK) {
			LW_UNLOCK();
			return lw_errno(r);
		}
		f = &tmp;
		borrowed = 1;
	}

	r = ext4_fseek(f, (int64_t)offset, SEEK_SET);
	if (r == EOK)
		r = ext4_fread(f, buf, size, &rd);

	if (borrowed)
		ext4_fclose(f);

	LW_UNLOCK();
	if (r != EOK)
		return lw_errno(r);
	return (int)rd;
}

static int lw_write(const char *path, const char *buf, size_t size,
		    off_t offset, struct fuse_file_info *fi)
{
	struct ext4_file *f = lw_fh(fi);
	struct ext4_file tmp;
	int borrowed = 0;
	size_t wr = 0;
	int r;

	LW_RO_GUARD();
	LW_LOCK();

	if (!f) {
		r = ext4_fopen(&tmp, path, "r+b");
		if (r != EOK) {
			LW_UNLOCK();
			return lw_errno(r);
		}
		f = &tmp;
		borrowed = 1;
	}

	r = ext4_fseek(f, (int64_t)offset, SEEK_SET);
	if (r == EOK)
		r = ext4_fwrite(f, buf, size, &wr);

	if (borrowed)
		ext4_fclose(f);

	/*
	 * lwext4's ext4_fwrite() overwrites its own error code in its Finish
	 * path ("r = ext4_fs_put_inode_ref(...)"), so a write that stops early
	 * because the filesystem is full is reported as EOK with a short count.
	 * Against lwext4 a short write can only mean "out of space", so turn it
	 * into ENOSPC explicitly instead of leaking a short count (which makes
	 * the kernel surface a bare EIO).
	 */
	if (r == EOK && wr < size)
		r = ENOSPC;

	if (r == EOK) {
		lw_touch_write(path);
		lw_flush();
	}
	LW_UNLOCK();
	if (r != EOK)
		return lw_errno(r);
	return (int)wr;
}

static int lw_truncate(const char *path, off_t size)
{
	struct ext4_file f;
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_fopen(&f, path, "r+b");
	if (r != EOK) {
		LW_UNLOCK();
		return lw_errno(r);
	}
	r = ext4_ftruncate(&f, (uint64_t)size);
	ext4_fclose(&f);
	if (r == EOK) {
		lw_touch_write(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_ftruncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	struct ext4_file *f = lw_fh(fi);
	struct ext4_file tmp;
	int borrowed = 0;
	int r;

	LW_RO_GUARD();
	LW_LOCK();

	if (!f) {
		r = ext4_fopen(&tmp, path, "r+b");
		if (r != EOK) {
			LW_UNLOCK();
			return lw_errno(r);
		}
		f = &tmp;
		borrowed = 1;
	}

	r = ext4_ftruncate(f, (uint64_t)size);
	if (borrowed)
		ext4_fclose(f);
	if (r == EOK) {
		lw_touch_write(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_mkdir(const char *path, mode_t mode)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_dir_mk(path);
	if (r == EOK) {
		ext4_mode_set(path, mode & 07777);
		ext4_owner_set(path, (uint32_t)getuid(), (uint32_t)getgid());
		lw_set_times_now(path);
		lw_ino_for_path(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_unlink(const char *path)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_fremove(path);
	if (r == EOK) {
		lw_ino_forget(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_rmdir(const char *path)
{
	int r, empty;

	LW_RO_GUARD();
	LW_LOCK();

	/* POSIX rmdir() must refuse non-empty directories; lwext4's
	 * ext4_dir_rm() would otherwise delete the whole subtree. */
	r = lw_dir_is_empty(path, &empty);
	if (r != 0) {
		LW_UNLOCK();
		return r;
	}
	if (!empty) {
		LW_UNLOCK();
		return -ENOTEMPTY;
	}

	r = ext4_dir_rm(path);
	if (r == EOK) {
		lw_ino_forget(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_rename(const char *from, const char *to)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	if (ext4_inode_exist(from, EXT4_DE_DIR) == EOK)
		r = ext4_dir_mv(from, to);
	else
		r = ext4_frename(from, to);
	if (r == EOK) {
		lw_ino_forget(from);
		lw_ino_for_path(to);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_chmod(const char *path, mode_t mode)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_mode_set(path, mode & 07777);
	if (r == EOK)
		lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_chown(const char *path, uid_t uid, gid_t gid)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_owner_set(path, (uint32_t)uid, (uint32_t)gid);
	if (r == EOK)
		lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_utime_helper(const char *path, const struct timespec tv[2])
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	if (tv) {
		if (tv[0].tv_nsec != UTIME_OMIT)
			ext4_atime_set(path, (uint32_t)tv[0].tv_sec);
		if (tv[1].tv_nsec != UTIME_OMIT)
			ext4_mtime_set(path, (uint32_t)tv[1].tv_sec);
		if (tv[0].tv_nsec != UTIME_OMIT || tv[1].tv_nsec != UTIME_OMIT)
			ext4_ctime_set(path, (uint32_t)time(NULL));
	}
	r = lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_utime(const char *path, struct utimbuf *ub)
{
	struct timespec tv[2];

	tv[0].tv_sec = ub->actime;
	tv[0].tv_nsec = 0;
	tv[1].tv_sec = ub->modtime;
	tv[1].tv_nsec = 0;
	return lw_utime_helper(path, tv);
}

static int lw_utimens(const char *path, const struct timespec tv[2])
{
	return lw_utime_helper(path, tv);
}

static int lw_statfs(const char *path, struct statvfs *sv)
{
	struct ext4_mount_stats st;
	uint64_t reserved = 0;
	int r;

	(void)path;

	memset(sv, 0, sizeof(*sv));

	LW_LOCK();
	r = ext4_mount_point_stats("/", &st);
	if (r == EOK && g_sb) {
		/*
		 * The ext4 "reserved blocks" pool is counted inside
		 * free_blocks_count but is not available to ordinary users;
		 * native ext4 reports it via f_bavail < f_bfree. Subtract it
		 * (64-bit field pair when the 64BIT incompat feature is set)
		 * so df(1) shows the same Available figure as a Linux mount.
		 */
		reserved = ext4_get32(g_sb, reserved_blocks_count_lo);
		if (ext4_sb_feature_incom(g_sb, EXT4_FINCOM_64BIT))
			reserved |= ((uint64_t)ext4_get32(g_sb,
					reserved_blocks_count_hi)) << 32;
	}
	LW_UNLOCK();
	if (r != EOK)
		return lw_errno(r);

	sv->f_bsize = st.block_size;
	sv->f_frsize = st.block_size;
	sv->f_blocks = st.blocks_count;
	sv->f_bfree = st.free_blocks_count;
	sv->f_bavail = (st.free_blocks_count > reserved)
		? st.free_blocks_count - reserved : 0;
	sv->f_files = st.inodes_count;
	sv->f_ffree = st.free_inodes_count;
	sv->f_favail = st.free_inodes_count;
	sv->f_namemax = 255;
	return 0;
}

static int lw_fsync(const char *path, int datasync,
		    struct fuse_file_info *fi)
{
	int r;

	(void)path;
	(void)datasync;
	(void)fi;

	LW_LOCK();
	r = lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_flush_op(const char *path, struct fuse_file_info *fi)
{
	int r;

	(void)path;
	(void)fi;

	LW_LOCK();
	r = lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_release(const char *path, struct fuse_file_info *fi)
{
	struct ext4_file *f;

	(void)path;

	LW_LOCK();
	f = lw_fh(fi);
	if (f) {
		ext4_fclose(f);
		free(f);
		fi->fh = 0;      /* guard against a double release */
		lw_flush();
	}
	LW_UNLOCK();
	return 0;
}

static int lw_opendir(const char *path, struct fuse_file_info *fi)
{
	int r;

	(void)fi;

	LW_LOCK();
	r = (ext4_inode_exist(path, EXT4_DE_DIR) == EOK) ? 0 : -ENOTDIR;
	LW_UNLOCK();
	return r;
}

static int lw_releasedir(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static int lw_access(const char *path, int mask)
{
	int r;

	(void)mask;

	LW_LOCK();
	r = (ext4_inode_exist(path, EXT4_DE_UNKNOWN) == EOK) ? 0 : -ENOENT;
	LW_UNLOCK();
	return r;
}

static int lw_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct ext4_file f;
	int r;

	(void)rdev;

	if (!S_ISREG(mode))
		return -EPERM;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_fopen(&f, path, "w+b");
	if (r == EOK) {
		ext4_fclose(&f);
		ext4_mode_set(path, mode & 07777);
		ext4_owner_set(path, (uint32_t)getuid(), (uint32_t)getgid());
		lw_set_times_now(path);
		lw_ino_for_path(path);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

/* ------------------------------------------------------------------ */
/* symlinks / hardlinks                                               */
/* ------------------------------------------------------------------ */

/*
 * FUSE symlink(from, to): 'from' is the link target (contents), 'to' is the
 * new symlink path. lwext4's ext4_fsymlink(target, path) matches this order.
 */
static int lw_symlink(const char *target, const char *linkpath)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_fsymlink(target, linkpath);
	if (r == EOK) {
		ext4_owner_set(linkpath, (uint32_t)getuid(),
				(uint32_t)getgid());
		lw_set_times_now(linkpath);
		lw_ino_for_path(linkpath);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_readlink(const char *path, char *buf, size_t size)
{
	size_t rcnt = 0;
	int r;

	LW_LOCK();
	/*
	 * Reserve one byte for the NUL terminator the FUSE API requires, then
	 * copy the payload. Short ("fast") symlinks are read straight out of
	 * the inode by lwext4; longer ones come from a data block.
	 */
	r = ext4_readlink(path, buf, size ? size - 1 : 0, &rcnt);
	LW_UNLOCK();
	if (r != EOK)
		return lw_errno(r);
	if (size) {
		if (rcnt >= size)
			rcnt = size - 1;
		buf[rcnt] = '\0';
	}
	return 0;
}

/* FUSE link(from, to): 'from' is the existing file, 'to' the new hardlink. */
static int lw_link(const char *from, const char *to)
{
	int r;

	LW_RO_GUARD();
	LW_LOCK();
	r = ext4_flink(from, to);
	if (r == EOK) {
		lw_ino_for_path(to);
		lw_flush();
	}
	LW_UNLOCK();
	return lw_errno(r);
}

/* ------------------------------------------------------------------ */
/* extended attributes                                                */
/* ------------------------------------------------------------------ */

/*
 * Only the ext4 "user." namespace is persisted on the image. Everything else
 * (macOS sends com.apple.*, plus security./trusted./system.) is refused so the
 * backing ext4 filesystem is not polluted with host-specific clutter. This
 * also means the read-only/xattr namespace checks are independent of any
 * kernel-level handling.
 */
static int lw_xattr_is_user(const char *name)
{
	return name && !strncmp(name, "user.", 5) && name[5] != '\0';
}

/*
 * lwext4 reports a missing attribute as ENODATA (== 96 on macOS); the macOS
 * xattr API expects ENOATTR (== 93). Translate just that one code; all other
 * errno values already match the host.
 */
static int lw_xattr_errno(int r)
{
	if (r == EOK)
		return 0;
	if (r == ENODATA)
		return -ENOATTR;
	return lw_errno(r);
}

#ifdef __APPLE__
static int lw_getxattr(const char *path, const char *name, char *value,
		       size_t size, uint32_t position)
#else
static int lw_getxattr(const char *path, const char *name, char *value,
		       size_t size)
#endif
{
	size_t dlen = 0;
	int r;

#ifdef __APPLE__
	(void)position;   /* only meaningful for resource forks */
#endif

	if (!lw_xattr_is_user(name))
		return -ENOATTR;

	LW_LOCK();
	r = ext4_getxattr(path, name, strlen(name), value, size, &dlen);
	LW_UNLOCK();
	if (r != EOK)
		return lw_xattr_errno(r);
	if (size == 0)
		return (int)dlen;        /* size probe */
	if (dlen > size)
		return -ERANGE;
	return (int)dlen;
}

#ifdef __APPLE__
static int lw_setxattr(const char *path, const char *name, const char *value,
		       size_t size, int flags, uint32_t position)
#else
static int lw_setxattr(const char *path, const char *name, const char *value,
		       size_t size, int flags)
#endif
{
	int r;

#ifdef __APPLE__
	(void)position;
#endif
	(void)flags;      /* XATTR_CREATE/XATTR_REPLACE both map to set/replace */

	if (!g_read_write)
		return -EROFS;
	if (!lw_xattr_is_user(name))
		return -ENOTSUP;

	LW_LOCK();
	r = ext4_setxattr(path, name, strlen(name), value, size);
	if (r == EOK)
		lw_flush();
	LW_UNLOCK();
	return lw_errno(r);
}

static int lw_listxattr(const char *path, char *list, size_t size)
{
	size_t ret = 0;
	int r;

	LW_LOCK();
	r = ext4_listxattr(path, list, size, &ret);
	LW_UNLOCK();
	if (r != EOK)
		return lw_errno(r);
	return (int)ret;
}

static int lw_removexattr(const char *path, const char *name)
{
	int r;

	if (!g_read_write)
		return -EROFS;
	if (!lw_xattr_is_user(name))
		return -ENOTSUP;

	LW_LOCK();
	/*
	 * lwext4's ext4_removexattr() has a latent bug: if it cannot open the
	 * path it re-takes the mountpoint lock instead of releasing it before
	 * returning. Guard with an existence check so that error path is never
	 * reached and our recursive lock cannot be leaked.
	 */
	if (ext4_inode_exist(path, EXT4_DE_UNKNOWN) != EOK) {
		LW_UNLOCK();
		return -ENOENT;
	}
	r = ext4_removexattr(path, name, strlen(name));
	if (r == EOK)
		lw_flush();
	LW_UNLOCK();
	return r == EOK ? 0 : lw_xattr_errno(r);
}

struct fuse_operations lwext4fs_ops = {
	.getattr = lw_getattr,
	.fgetattr = lw_fgetattr,
	.symlink = lw_symlink,
	.readlink = lw_readlink,
	.link = lw_link,
	.readdir = lw_readdir,
	.open = lw_open,
	.create = lw_create,
	.read = lw_read,
	.write = lw_write,
	.truncate = lw_truncate,
	.ftruncate = lw_ftruncate,
	.mkdir = lw_mkdir,
	.unlink = lw_unlink,
	.rmdir = lw_rmdir,
	.rename = lw_rename,
	.chmod = lw_chmod,
	.chown = lw_chown,
	.utime = lw_utime,
	.utimens = lw_utimens,
	.statfs = lw_statfs,
	.fsync = lw_fsync,
	.flush = lw_flush_op,
	.release = lw_release,
	.opendir = lw_opendir,
	.releasedir = lw_releasedir,
	.access = lw_access,
	.mknod = lw_mknod,
	.setxattr = lw_setxattr,
	.getxattr = lw_getxattr,
	.listxattr = lw_listxattr,
	.removexattr = lw_removexattr,
};
