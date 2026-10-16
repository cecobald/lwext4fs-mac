#!/bin/bash
# Threaded + handle-caching + timestamp test for lwext4fs-mac.
# ONLY touches throwaway images under /Users/ceco/lwext4-spike/.
set -u

BIN=/Users/ceco/lwext4-spike/lwext4fs-mac/lwext4fs-mac
IMG=/Users/ceco/lwext4-spike/hardening-test.ext4
MNT=/Users/ceco/lwext4-spike/mnt
E2=/usr/local/opt/e2fsprogs/sbin
LOG=/Users/ceco/lwext4-spike/hardening-mount.log

mkdir -p "$MNT"
rm -f "$LOG"

echo "### create fresh throwaway image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1048576 count=128 2>/dev/null
"$E2/mke2fs" -t ext4 -F -b 4096 -O ^orphan_file,^metadata_csum_seed -q "$IMG"
echo "mke2fs_rc=$?"
ls -l "$IMG"

echo
echo "### launch fuse (multithreaded; no -s)"
# read-only by default now -> ask for writes explicitly with -o rw
"$BIN" "$IMG" "$MNT" -f -o rw >"$LOG" 2>&1 &
FUSE_PID=$!
echo "fuse_pid=$FUSE_PID"

for i in $(seq 1 40); do
  if mount | grep -q " on $MNT "; then break; fi
  if ! kill -0 "$FUSE_PID" 2>/dev/null; then
    echo "!!! fuse process died early; log:"; cat "$LOG"; exit 1
  fi
  sleep 0.25
done
echo "### mount table:"
mount | grep "$MNT" || echo "(NOT MOUNTED)"

echo
echo "### 1) sequential write/read"
echo "hello lwext4 hardened" > "$MNT/hello.txt"
echo "write_rc=$?"
cat "$MNT/hello.txt"
echo "### stat hello.txt (timestamps):"
stat -f 'ino=%i mode=%Sp size=%z mtime=%m atime=%a ctime=%c' "$MNT/hello.txt"
MT=$("stat" -f '%m' "$MNT/hello.txt")
if [ "$MT" -gt 1000000000 ]; then echo "TIMESTAMP_OK mtime=$MT"; else echo "TIMESTAMP_EPOCH mtime=$MT"; fi

echo
echo "### 2) mkdir + nested write/read"
mkdir "$MNT/testdir"; echo "mkdir_rc=$?"
echo "nested content 12345" > "$MNT/testdir/inner.txt"
cat "$MNT/testdir/inner.txt"
mkdir -p "$MNT/a/b/c"; echo "mkdir_nested_rc=$?"
echo "deep" > "$MNT/a/b/c/deep.txt"
cat "$MNT/a/b/c/deep.txt"
ls -la "$MNT" "$MNT/testdir"

echo
echo "### 3) delete"
rm "$MNT/testdir/inner.txt"; echo "rm_rc=$?"
rmdir "$MNT/testdir"; echo "rmdir_rc=$?"
rm -rf "$MNT/a"; echo "rm_rf_rc=$?"
echo "### ls after delete:"; ls -la "$MNT"

echo
echo "### 4) file-handle reuse: write then read back same fd via python"
python3 - "$MNT/handle.txt" <<'PY'
import os, sys
p = sys.argv[1]
fd = os.open(p, os.O_RDWR | os.O_CREAT | os.O_TRUNC)
os.write(fd, b"AAAA" + b"B"*100)
os.lseek(fd, 0, 0)
data = os.read(fd, 200)
print("read-back:", data[:8], "len", len(data))
os.lseek(fd, 4, 0)
os.write(fd, b"ZZZZ")
os.lseek(fd, 0, 0)
print("after overwrite:", os.read(fd, 8))
os.close(fd)
print("size:", os.path.getsize(p))
PY
echo "handle_py_rc=$?"

echo
echo "### 5) CONCURRENCY: 8 parallel writers (dd 2 MiB each, distinct sources)"
for i in $(seq 1 8); do head -c 2097152 /dev/urandom > "/tmp/conc_src_$i"; done
pids=()
for i in $(seq 1 8); do
  dd if="/tmp/conc_src_$i" of="$MNT/f$i" bs=64k 2>/dev/null &
  pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
echo "parallel_write_wait_fail=$fail"

echo "### verify sizes + content integrity:"
ok=1
for i in $(seq 1 8); do
  sz=$(stat -f '%z' "$MNT/f$i")
  if cmp -s "/tmp/conc_src_$i" "$MNT/f$i" && [ "$sz" = "2097152" ]; then
    echo "f$i: OK size=$sz"
  else
    echo "f$i: MISMATCH size=$sz"; ok=0
  fi
done
echo "CONCURRENCY_INTEGRITY_OK=$ok"

echo
echo "### 6) unmount"
umount "$MNT" 2>&1 || diskutil unmount "$MNT" 2>&1
echo "umount_rc=$?"
sleep 1
kill "$FUSE_PID" 2>/dev/null
wait "$FUSE_PID" 2>/dev/null
echo "### fuse log:"; cat "$LOG"

echo
echo "### 7) e2fsck -fn"
"$E2/e2fsck" -fn "$IMG"
echo "E2FSCK_RC=$?"

echo
echo "### 8) debugfs listing of surviving files"
"$E2/debugfs" -R "ls -l /" "$IMG" 2>/dev/null
