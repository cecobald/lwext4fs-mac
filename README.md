# lwext4fs-mac

**An ext4 read/write filesystem for macOS.** A small [macFUSE](https://macfuse.github.io/)
(FUSE 2 high-level API) front-end over the [lwext4](https://github.com/gkostka/lwext4)
library, so macOS can mount **and write** Linux ext4 volumes and disk images.

> **Status: v1.0.0 — working prototype.** Verified read/write on ext4 images with
> a clean `e2fsck` afterwards. It is **not** a production, kernel-grade driver —
> see [Risks](#risks) before pointing it at a disk you care about.

---

## Why

macOS cannot write ext4 natively. The common free tools are read-only
(`ext4fuse`, [ExtendFS](https://github.com/kthchew/ExtendFS)), and the commercial
one (Paragon extFS) is paid. This project wraps **lwext4** — a full ext4 stack
(extents, journaling, metadata checksums, directory indexing) — behind macFUSE to
provide real read/write access. It uses **macFUSE** rather than Apple's FSKit
because FSKit file-system modules require an Apple-restricted entitlement
(`com.apple.developer.fskit.fsmodule`).

## Features

- **Full read/write ext4**: `write`, `create`, `mkdir`, `rename`, `truncate`,
  `unlink`, `rmdir`, `chmod`, `chown`, `utimens`, …
- **Read-only by default** — the volume mounts read-only unless you pass **`-o rw`**
  (belt-and-braces: a read-only kernel mount *and* an op-layer guard returning `-EROFS`).
- **Symlinks / hardlinks** — `symlink`, `readlink`, `link`.
- **Extended attributes** — `getxattr`/`setxattr`/`listxattr`/`removexattr`,
  limited to the ext4 `user.` namespace.
- **Correct metadata** — `mtime`/`ctime` bumped on write; **precise POSIX errnos**
  (`ENOSPC`, `EEXIST`, `ENOTEMPTY`, `EISDIR`, `ENOENT`, …); accurate `statfs`.
- **Multithreaded** (recursive mutex registered with lwext4) with an open-file
  handle cache.

## Requirements

- **macOS** (Intel or Apple Silicon).
- **[macFUSE](https://macfuse.github.io/) 4.x** — install it first, approve the
  system extension when macOS asks, and reboot. Homebrew: `brew install --cask macfuse`.
- **Build tools**: Xcode Command Line Tools (`xcode-select --install`), `make`,
  `cmake`, `pkg-config`, `git`.
- *(optional)* **e2fsprogs** for `mke2fs`/`e2fsck` (e.g. `brew install e2fsprogs`,
  keg-only → `/usr/local/opt/e2fsprogs/sbin`).

## Build from source

```sh
git clone https://github.com/cecobald/lwext4fs-mac.git
cd lwext4fs-mac
./build.sh          # fetches lwext4 @ a pinned commit, applies patches, builds everything
```

`build.sh` produces **`./lwext4fs-mac`**. It will:

1. clone `gkostka/lwext4` into `vendor/lwext4` and check out a pinned commit,
2. apply the fixes under `patches/`,
3. build lwext4's static libraries (CMake),
4. build this front-end.

**Manual build** (if you already have an lwext4 tree with `build_generic/`):

```sh
make LWEXT4=/path/to/lwext4
```

## Install

```sh
sudo make install          # installs /usr/local/bin/lwext4fs-mac
# uninstall:
sudo make uninstall
```

## Usage

```sh
lwext4fs-mac <device|image> <mountpoint> [fuse options...]

# read-only (default):
sudo lwext4fs-mac /dev/disk0s3 /Volumes/Linux
# read-write (opt in explicitly):
sudo lwext4fs-mac /dev/disk0s3 /Volumes/Linux -o rw
# a disk image, foreground, debug:
sudo lwext4fs-mac ./image.ext4 ./mnt -f -o rw -o debug
```

The mode is logged at startup:

```
lwext4fs: mounted READ-ONLY (pass -o rw to enable writes)
lwext4fs: mounted READ-WRITE (-o rw)
```

Unmount with **`sudo lwext4fs-umount <mountpoint>`** (installed by `make install`; retries with a forced unmount because Spotlight/`mds` frequently holds real-device macFUSE mounts busy), or plainly `umount <mountpoint>`.

## Image / volume format note

This lwext4 build does **not** support the `orphan_file` incompat feature.
If you create test images with a modern `mke2fs`, disable it:

```sh
/usr/local/opt/e2fsprogs/sbin/mke2fs -t ext4 -F -b 4096 -O ^orphan_file img.ext4
```

(`metadata_csum_seed`, which modern `mke2fs` also enables, **is** supported —
lwext4 derives the same CRC seed from the UUID.)

## Limitations

- **Serialized I/O** — a single global recursive mutex (thread-safe, but not
  truly parallel; that is the granularity lwext4 offers).
- `atime` is **not** updated on read (would add an inode write to every read and
  break the read-only contract); `utime`/`utimens` still set it.
- **Synthetic inode numbers** (a path→ino table); hardlinks do not share an ino.
  `st_nlink` is correct.
- No `flock`/`ioctl`/`poll`/`fallocate`; no ACLs or resource forks.
- Symlink targets larger than one block are rejected (lwext4 limit).
- Filenames are not NFD-normalized.

## Risks

**This is a prototype. Do not treat it like the Linux kernel's ext4.**

- lwext4 is not as battle-tested as the in-kernel ext4 driver; a bug in its
  write path could corrupt the filesystem.
- **Never write while the owning OS is running — or hibernated.** A hibernated
  Linux still owns the partition; writing from macOS can corrupt it.
- There is no crash-atomicity guarantee of the kind the Linux journal provides.
- Keep backups; prefer a shared **exFAT** partition for routine cross-OS file
  exchange, and run `e2fsck -fn` before/after if you must write.

## Design notes

- **Front-end**: `src/main.c` registers the backing file/device as an lwext4
  `file_dev` blockdev, mounts it at lwext4 root `"/"`, registers a recursive
  mutex via `ext4_mount_setup_locks()`, and calls `fuse_main` (multithreaded).
  `src/ops.c(.h)` implements `struct fuse_operations` mapped onto lwext4, with an
  ino⇄path table and an open-file-handle cache.
- **Error mapping**: a single `lw_errno()` maps lwext4 codes to POSIX errnos.
  Two upstream quirks are compensated at the front-end: `ext4_fwrite()` clobbers
  its error on a full disk (reported as `ENOSPC`), and `ext4_dir_rm()` is
  recursive (guarded → `ENOTEMPTY`).
- See `REPORT-bg.md` for a full write-up (in Bulgarian) and `RELEASE-v1.0.0.md`
  for the release summary.

## Tests

`test.sh`, `test_features.sh`, `test_hardening.sh`, `test_stress.sh` and
`test_polish.sh` exercise the filesystem on **throwaway images** and check
`e2fsck -fn` is clean afterwards. They never touch a real disk.

## License

**GPL-2.0.** This program links the [lwext4](https://github.com/gkostka/lwext4)
library, which is licensed GPLv2, so the combined work is distributed under the
GNU General Public License v2 (see [`LICENSE`](LICENSE)).
