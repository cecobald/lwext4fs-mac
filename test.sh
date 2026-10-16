#!/bin/bash
# Feasibility test: mount lwext4fs-mac, do R/W, umount, fsck.
# ONLY touches throwaway image under /Users/ceco/lwext4-spike/.
set -u

BIN=/Users/ceco/lwext4-spike/lwext4fs-mac/lwext4fs-mac
IMG=/Users/ceco/lwext4-spike/fuse-test.ext4
MNT=/Users/ceco/lwext4-spike/mnt
E2=/usr/local/opt/e2fsprogs/sbin
LOG=/Users/ceco/lwext4-spike/fuse-mount.log

mkdir -p "$MNT"
rm -f "$LOG"

# read-only by default now -> ask for writes explicitly with -o rw
echo "### launching: $BIN $IMG $MNT -f -o rw"
"$BIN" "$IMG" "$MNT" -f -o rw >"$LOG" 2>&1 &
FUSE_PID=$!
echo "fuse_pid=$FUSE_PID"

# wait for the mount to appear (up to ~10s)
for i in $(seq 1 20); do
  if mount | grep -q " on $MNT "; then break; fi
  if ! kill -0 "$FUSE_PID" 2>/dev/null; then
    echo "!!! fuse process died early; log:"; cat "$LOG"; exit 1
  fi
  sleep 0.5
done

echo "### mount table:"
mount | grep "$MNT" || echo "(not mounted)"

echo "### ls mountpoint:"; ls -la "$MNT"

echo "### write file"
echo "hello lwext4 scaffold" > "$MNT/hello.txt"
echo "write_rc=$?"

echo "### read back:"; cat "$MNT/hello.txt"

echo "### mkdir"
mkdir "$MNT/testdir"; echo "mkdir_rc=$?"

echo "### create nested file"
echo "nested content 12345" > "$MNT/testdir/inner.txt"
cat "$MNT/testdir/inner.txt"

echo "### ls -la mountpoint:"; ls -la "$MNT"
echo "### ls -la testdir:"; ls -la "$MNT/testdir"
echo "### stat hello.txt:"; stat "$MNT/hello.txt"

echo "### delete nested file + rmdir"
rm "$MNT/testdir/inner.txt"; echo "rm_rc=$?"
rmdir "$MNT/testdir"; echo "rmdir_rc=$?"

echo "### final ls:"; ls -la "$MNT"

echo "### umount"
umount "$MNT" 2>&1 || diskutil unmount "$MNT" 2>&1
echo "umount_rc=$?"

# give the daemon a moment to flush+teardown
sleep 1
kill "$FUSE_PID" 2>/dev/null
wait "$FUSE_PID" 2>/dev/null
echo "### fuse log:"
cat "$LOG"

echo "### e2fsck -fn"
"$E2/e2fsck" -fn "$IMG"
echo "E2FSCK_RC=$?"
