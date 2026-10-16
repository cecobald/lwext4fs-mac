# lwext4fs-mac v1.0.0

An **ext4 read/write** filesystem for macOS — a small **macFUSE** (FUSE 2)
front-end wrapping the **lwext4** library. Lets macOS mount and *write* Linux
ext4 volumes (and images).

## Highlights
- **Full read/write** ext4 (write/create/mkdir/rename/truncate/unlink/rmdir/chmod/chown/utimens).
- **Read-only by default** — writes only with `-o rw`.
- **symlink / readlink / hardlink** and **xattr** (`user.*`).
- `mtime`/`ctime` bumped on write; **precise POSIX errnos** (`ENOSPC/EEXIST/ENOTEMPTY/EISDIR/ENOENT`); accurate `statfs`.
- **Multithreaded** (recursive mutex + lwext4 locks); open-file-handle cache.
- Verified end-to-end: writes on a throwaway image, then `e2fsck -fn` → **clean (exit 0)**.

## Why
macOS cannot write ext4 natively; existing free tools (`ext4fuse`, `ExtendFS`)
are read-only. This is a working read/write alternative, built on macFUSE
instead of FSKit (which requires an Apple-restricted entitlement).

## Requirements
- macOS (Intel or Apple Silicon) with **macFUSE 4.x** installed
- Xcode Command Line Tools (`cc`), `make`, `cmake`, `pkg-config`, `git`
- optional: `e2fsprogs` for `mke2fs`/`e2fsck`

## Build & install
```sh
git clone <repo-url> lwext4fs-mac && cd lwext4fs-mac
./build.sh                 # fetches + patches + builds lwext4 and the front-end
sudo make install          # -> /usr/local/bin/lwext4fs-mac
```

## Usage
```sh
sudo lwext4fs-mac <device|image> <mountpoint> [options]
#  read-only (default)
sudo lwext4fs-mac /dev/disk0s3 /Volumes/Linux
#  read-write (opt in)
sudo lwext4fs-mac /dev/disk0s3 /Volumes/Linux -o rw
```

## Risk / scope
**Prototype — not production.** lwext4 is not as battle-tested as the Linux
kernel ext4. Only write when the owning OS is fully shut down (never
hibernated), keep backups, and prefer shared exFAT for cross-OS file exchange.
See `README.md` for full limitations.
