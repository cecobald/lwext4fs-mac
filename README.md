# lwext4fs-mac — macFUSE (FUSE 2) front-end over lwext4

**Status: feasibility scaffold, NOT production.** Proves that a read-**write**
ext4 filesystem can be mounted on macOS by wrapping the patched `lwext4`
library with a thin macFUSE (FUSE 2 high-level API) layer.

Additional features (this iteration):

- **Safe mode** — the filesystem mounts **read-only by default**; writes only
  when the user passes **`-o rw`** explicitly.
- **Symlinks / hardlinks** — `symlink`, `readlink`, `link`.
- **Extended attributes** — `getxattr`/`setxattr`/`listxattr`/`removexattr`,
  limited to the ext4 `user.` namespace.
- **Timestamps updated on write** — `write`/`truncate`/`ftruncate` bump the
  file's **mtime + ctime** to "now" (read-write mounts only; lwext4 itself does
  not maintain them).
- **Precise error mapping** — every lwext4 return code is funnelled through one
  `lw_errno()` helper that yields the exact POSIX errno (`ENOSPC`, `EEXIST`,
  `ENOTEMPTY`, `EISDIR`, `ENOENT`, …) instead of a generic `-1`/`EIO`.
- **Accurate `statfs`** — real block/inode counts from the superblock, with the
  ext4 root-reserved blocks subtracted so `df -h` matches a native ext4 mount
  and shrinks as you write.

## Build

```sh
make            # -> ./lwext4fs-mac
```

Needs: macFUSE 4.x (provides libfuse 2.x), pkg-config `fuse`, and the
lwext4 build under `/Users/ceco/lwext4-spike/lwext4` (static libs already
built).

## Run

```sh
./lwext4fs-mac <device_or_image> <mountpoint> [fuse options...]
# read-only (the default):
./lwext4fs-mac /path/img.ext4 /path/mnt -f
# read-write (opt in):
./lwext4fs-mac /path/img.ext4 /path/mnt -f -o rw
# foreground + debug:
./lwext4fs-mac /path/img.ext4 /path/mnt -f -o rw -o debug
```

The mount mode is logged at startup:

```
lwext4fs: mounted READ-ONLY (pass -o rw to enable writes)
lwext4fs: mounted READ-WRITE (-o rw)
```

The backing file/device is exposed to lwext4 via the `file_dev` blockdev
(`blockdev/linux/file_dev.c`), then mounted at lwext4 root `"/"`, so FUSE
paths map 1:1 onto lwext4 paths.

## Files

- `src/main.c`   – arg parsing, blockdev registration, `ext4_mount`,
  `ext4_mount_setup_locks`, `fuse_main` (multithreaded; no `-s`).
- `src/ops.c/.h` – `struct fuse_operations` mapping to lwext4; file-handle
  cache, mountpoint lock, timestamp-on-create.
- `Makefile`     – links lwext4 + blockdev static libs + libfuse.
- `test.sh`, `test_hardening.sh`, `test_stress.sh` – end-to-end tests on
  throwaway images (see below). These request `-o rw` explicitly.
- `test_features.sh` – tests safe mode, symlink/hardlink and xattr.
- `test_polish.sh` – tests timestamps-on-write, exact errno mapping (ENOSPC,
  ENOENT, EISDIR, EEXIST, ENOTEMPTY) and statfs/`df -h` accuracy.

## IMPORTANT image-format constraint

lwext4 (this version) does **not** support the `orphan_file` incompat feature
nor the `metadata_csum_seed` read-only feature, both of which modern
`mke2fs` enables by default. Such an image either fails to mount (ENOTSUP)
or is silently forced read-only. Create test images with:

```sh
/usr/local/opt/e2fsprogs/sbin/mke2fs -t ext4 -F -b 4096 \
    -O ^orphan_file,^metadata_csum_seed img.ext4
```

## Design notes (scaffold)

- **Synthetic inode numbers.** A small path→ino table assigns stable inos
  (root=1, then sequential). They do not match ext4 on-disk inodes, are not
  persisted, and are not shared across hardlinks. `ext4_raw_inode_fill()` /
  `direntry->inode` would give real inode numbers if needed later.
- **Thread safety.** `main` no longer passes `-s`; macFUSE runs multithreaded.
  ops.c registers a **recursive pthread mutex** as lwext4's mountpoint OS lock
  via `ext4_mount_setup_locks()` (without it, `ext4_lock` callbacks are NULL
  and lwext4 is not thread-safe) and also takes the same recursive mutex around
  every FUSE operation, so composite ops are atomic. NOTE: a *single* global
  mutex serialises content operations — that is the granularity lwext4 offers,
  so this gives thread-safety, not true parallel I/O.
- **Open-file handle caching.** `open`/`create` allocate a heap
  `struct ext4_file` and stash it in `fi->fh`; `read`/`write`/`ftruncate`/
  `fsync`/`flush`/`release` reuse it (falling back to a temporary handle when
  the kernel issues an operation without one). `getattr`/`fgetattr` prefer the
  open handle's live size. `release` frees the handle and clears `fi->fh`.
- **Safe mode (read-only by default).** `main.c` scans the FUSE args for
  `rw`/`ro` (detached `-o rw` and attached `-orw` forms) into the global
  `g_read_write`; nothing given means read-only. In read-only mode macFUSE is
  asked for a read-only kernel mount (`-o ...,ro`) *and* every mutating FUSE
  callback in `ops.c` returns `-EROFS` (belt and braces — the ops-layer guard
  was verified to work even with a writable kernel mount). lwext4 itself is
  still mounted **read-write internally** so journal replay/recovery works; the
  read-only policy is enforced purely at the FUSE front-end.
- **Symlinks / hardlinks.** lwext4's path-based helpers are used directly:
  `ext4_fsymlink(target, path)`, `ext4_readlink(path, buf, n, &rcnt)` and
  `ext4_flink(path, hardlink_path)` → FUSE `symlink`/`readlink`/`link`. Short
  targets use lwext4's in-inode ("fast") representation, longer ones a data
  block; `readlink` NUL-terminates as the FUSE API requires.
- **Extended attributes.** FUSE `getxattr`/`setxattr`/`listxattr`/`removexattr`
  map onto `ext4_getxattr`/`ext4_setxattr`/`ext4_listxattr`/`ext4_removexattr`
  (which take fully-qualified names). **Policy: only the `user.` namespace is
  persisted.** `setxattr`/`removexattr` on any other namespace (`com.apple.*`,
  `security.*`, `trusted.*`, `system.*`) return `-ENOTSUP`, and `getxattr` on
  such names returns `-ENOATTR`, so the ext4 image is never polluted with
  host-specific clutter. (macOS's VFS silently swallows `-ENOTSUP` from
  `setxattr`, which is why such calls appear to "succeed" while nothing is
  stored.) lwext4's "no such attribute" code is `ENODATA`; the ops layer
  translates it to the macOS `ENOATTR`.
- **Timestamps.** `create`/`mkdir`/`mknod`/`symlink` set atime/mtime/ctime to
  "now" and set a default mode/uid/gid. lwext4's own write path does not
  maintain timestamps, so `write`/`truncate`/`ftruncate` also bump **mtime +
  ctime** to "now" via `lw_touch_write()` — but only on read-write mounts (a
  read-only front-end must never modify the image). atime is deliberately NOT
  updated on read: that would add an inode write to every `read()` and break the
  read-only contract for pure reads; `utime`/`utimens` still set it explicitly.
  Granularity is one second.
- **Error mapping.** `lw_errno()` maps lwext4 codes to the negated POSIX errno
  returned to FUSE. Because this lwext4 build has `CONFIG_HAVE_OWN_ERRNO == 0`
  it already returns host errno values, so the mapping is the identity, but it
  is written explicitly: `EPERM ENOENT EIO ENXIO E2BIG ENOMEM EACCES EFAULT
  EEXIST ENODEV ENOTDIR EISDIR EINVAL EFBIG ENOSPC EROFS EMLINK ERANGE
  ENOTEMPTY ENAMETOOLONG EMFILE ENFILE ENOSYS EXDEV EOVERFLOW ELOOP ENODATA
  ENOTSUP` (unknown -> `EIO`). Two behaviours are compensated at the front-end:
  (a) lwext4's `ext4_fwrite()` clobbers its error in its Finish path, so a
  full-disk write comes back as EOK with a short count — `lw_write()` reports
  that as `ENOSPC`; (b) lwext4's `ext4_dir_rm()` is **recursive**, so `lw_rmdir()`
  pre-checks emptiness and returns `ENOTEMPTY`, preserving POSIX semantics and
  avoiding accidental recursive deletion.
- **statfs.** `statfs` reads the real superblock counts via
  `ext4_mount_point_stats()`, and subtracts `reserved_blocks_count`
  (lo/hi, 64-bit aware) from `f_bavail` so `df` shows the same *Available*
  figure as a native ext4 mount.
- **Not implemented**: ioctl, locks, poll, ACLs, resource forks.

## Test result

`test.sh` (basic), `test_hardening.sh` (sequential + 8-way parallel writers +
timestamps + handle reuse) and `test_stress.sh` (6 workers × 150 mixed
create/write/read/unlink + dirs) all pass. `test_features.sh` (33 checks) covers
read-only-by-default (writes rejected with `EROFS`), `-o rw` writes, symlink
create/readlink/follow (short and long targets), hardlink create/content/nlink/
unlink semantics, and xattr set/get/list/size-probe/remove plus the `com.apple.*`
refusal. After unmount, `e2fsck -fn` reports **clean, exit code 0** on every
image. Parallel writers' contents verify with `cmp`. Freshly created files carry
non-epoch timestamps (uid/gid = the mounting user).

`test_polish.sh` (18 checks) additionally verifies: mtime+ctime advance after a
write/append/truncate (and reads leave mtime untouched); exact errnos —
`ENOENT`=2, `EISDIR`=21, `EEXIST`=17 (both `mkdir`-over and `O_EXCL`),
`ENOTEMPTY`=66, `ENOSPC`=28 on a filled image; and `df -h` reporting the true
size, reserved-block-accurate *Available*, and *Available* shrinking after a
100 MiB write.

## lwext4 patch applied for this iteration

`src/ext4_xattr.c` was fixed: `ext4_xattr_remove()` used the uninitialised
`block_finder.s` search context when removing an attribute stored in the inode
body (`ibody_finder.s` is correct, matching upstream). Without the fix,
`removexattr` was a silent no-op for in-inode attributes. Backup:
`src/ext4_xattr.c.bak-xattrfix-2026-10-16`; the static lib was rebuilt with the
existing CMake build tree (`cmake --build build_generic --target lwext4`, same
flags); the previous archive is saved as
`build_generic/src/liblwext4.a.bak-2026-10-16`.
