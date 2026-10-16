/*
 * main.c - macFUSE (FUSE 2 API) front-end for the lwext4 ext4 library.
 *
 * Usage:
 *     lwext4fs-mac <device_or_image> <mountpoint> [extra fuse options...]
 *
 *   e.g.  lwext4fs-mac /tmp/img.ext4 /tmp/mnt -f -o debug
 *
 * The backing file (or block device) is exposed to lwext4 through the
 * "file_dev" blockdev (blockdev/linux/file_dev.c), then mounted at "/"
 * inside the lwext4 namespace, so FUSE paths map 1:1 onto lwext4 paths.
 *
 * THIS IS A FEASIBILITY SCAFFOLD, NOT PRODUCTION CODE.
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>

#include <ext4.h>
#include <file_dev.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ops.h"

/* lwext4 device name + internal mount point. */
#define LWE_DEV_NAME "lwext4fs"
#define LWE_MOUNT_POINT "/"

static const char *g_image;

/*
 * Read-only unless the user explicitly asks for writes with "-o rw".
 * Shared with ops.c (declared in ops.h), which enforces it at the FUSE layer
 * by returning -EROFS from every mutating callback.
 */
int g_read_write = 0;

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s <device_or_image> <mountpoint> [fuse options...]\n"
		"\n"
		"Mounts read-only by default; pass '-o rw' to enable writes.\n",
		prog);
}

/*
 * Scan the user-supplied FUSE args (argv[3..]) for an "rw"/"ro" mount option
 * and return the effective read-write flag (0 = read-only). Handles both the
 * detached form ("-o rw", "-o rw,foo") and the attached form ("-orw").
 * When both appear the last one wins, matching mount(8) semantics; the default
 * (nothing given) is read-only.
 */
static int parse_rw_flag(int argc, char **argv)
{
	int rw = 0;
	int i;

	for (i = 3; i < argc; i++) {
		const char *opts = NULL;
		const char *p;

		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			opts = argv[++i];
		} else if (!strncmp(argv[i], "-o", 2) && argv[i][2] != '\0') {
			opts = argv[i] + 2;
		}
		if (!opts)
			continue;

		p = opts;
		while (*p) {
			const char *q = strchr(p, ',');
			size_t len = q ? (size_t)(q - p) : strlen(p);

			if (len == 2 && !strncmp(p, "rw", 2))
				rw = 1;
			else if (len == 2 && !strncmp(p, "ro", 2))
				rw = 0;

			if (!q)
				break;
			p = q + 1;
		}
	}
	return rw;
}

/*
 * Open the backing store, register the lwext4 file blockdev, and mount the
 * filesystem at LWE_MOUNT_POINT. Enables write-back caching so that a batch of
 * metdata ops is cheap; data is flushed on fsync/write/umount by ops.c.
 */
static int lwext4_setup(const char *image)
{
	struct ext4_blockdev *bd;
	int r;

	file_dev_name_set(image);
	bd = file_dev_get();
	if (!bd) {
		fprintf(stderr, "lwext4fs: file_dev_get() failed\n");
		return -EIO;
	}

	r = ext4_device_register(bd, LWE_DEV_NAME);
	if (r != EOK) {
		fprintf(stderr, "ext4_device_register: %d\n", r);
		return -r;
	}

	/* Mount read-write at the lwext4 root. */
	r = ext4_mount(LWE_DEV_NAME, LWE_MOUNT_POINT, false);
	if (r != EOK) {
		fprintf(stderr, "ext4_mount: %d\n", r);
		return -r;
	}

	/*
	 * Register our recursive pthread mutex as lwext4's mountpoint OS lock so
	 * its internal critical sections are actually serialised (without this,
	 * ext4_lock callbacks are NULL and lwext4 is not thread-safe). This must
	 * happen after ext4_mount() because ext4_mount_setup_locks() resolves the
	 * mountpoint by name - and before any concurrent activity (recover, the
	 * FUSE loop).
	 */
	r = lwext4fs_setup_locks(LWE_MOUNT_POINT);
	if (r != EOK) {
		fprintf(stderr, "lwext4fs: ext4_mount_setup_locks: %d\n", r);
		return -r;
	}

	/* Replay the journal if present (harmless on non-journaled fs). */
	r = ext4_recover(LWE_MOUNT_POINT);
	if (r != EOK && r != ENOTSUP)
		fprintf(stderr, "lwext4fs: ext4_recover: %d (continuing)\n", r);

	r = ext4_journal_start(LWE_MOUNT_POINT);
	if (r != EOK)
		fprintf(stderr, "lwext4fs: ext4_journal_start: %d (continuing)\n",
			r);

	/* Batch metadata writes; ops.c flushes explicitly. */
	ext4_cache_write_back(LWE_MOUNT_POINT, 1);

	lwext4fs_ops_init(LWE_MOUNT_POINT);
	return 0;
}

static void lwext4_teardown(void)
{
	/* Flush write-back cache and stop the journal before unmounting. */
	ext4_cache_write_back(LWE_MOUNT_POINT, 0);
	ext4_journal_stop(LWE_MOUNT_POINT);
	ext4_umount(LWE_MOUNT_POINT);
}

int main(int argc, char **argv)
{
	char *fargv[64];
	int fargc = 0;
	const char *mountpoint;
	int i, r;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	g_image = argv[1];
	mountpoint = argv[2];

	/* Decide read-only vs read-write before touching the image. */
	g_read_write = parse_rw_flag(argc, argv);

	r = lwext4_setup(g_image);
	if (r != 0) {
		fprintf(stderr, "lwext4fs: failed to mount '%s' (err %d)\n",
			g_image, r);
		return 1;
	}
	fprintf(stderr, "lwext4fs: mounted '%s' -> lwext4 '/'\n", g_image);
	if (g_read_write)
		fprintf(stderr,
			"lwext4fs: mounted READ-WRITE (-o rw)\n");
	else
		fprintf(stderr,
			"lwext4fs: mounted READ-ONLY "
			"(pass -o rw to enable writes)\n");

	/*
	 * Build the argv passed to fuse_main():
	 *   argv[0]                      program name
	 *   -o volname=...,fsname=...    macFUSE-friendly defaults
	 *   <user extra options argv[3..]>
	 *   <mountpoint>
	 *
	 * NOTE: -s (single-threaded) is deliberately NOT passed. lwext4 thread
	 * safety is provided by the recursive mountpoint lock registered above
	 * plus the op-level guard in ops.c.
	 */
	fargv[fargc++] = argv[0];
	fargv[fargc++] = "-o";
	/*
	 * Ask macFUSE for a read-only kernel mount by default (defence in depth:
	 * the kernel rejects writes before they ever reach our callbacks, and
	 * ops.c additionally returns -EROFS). "ro" is added only when the user
	 * did not request "-o rw"; their original options are still passed
	 * through unchanged below.
	 */
	fargv[fargc++] = g_read_write
		? "volname=lwext4fs,fsname=lwext4fs"
		: "volname=lwext4fs,fsname=lwext4fs,ro";
	for (i = 3; i < argc; i++)
		fargv[fargc++] = argv[i];
	fargv[fargc++] = (char *)mountpoint;
	fargv[fargc] = NULL;

	r = fuse_main(fargc, fargv, &lwext4fs_ops, NULL);

	lwext4_teardown();
	fprintf(stderr, "lwext4fs: unmounted (fuse_main rc=%d)\n", r);
	return r;
}
