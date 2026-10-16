#!/bin/bash
# Mixed-workload stress test for lwext4fs-mac threading.
# ONLY touches throwaway images under /Users/ceco/lwext4-spike/.
set -u

BIN=/Users/ceco/lwext4-spike/lwext4fs-mac/lwext4fs-mac
IMG=/Users/ceco/lwext4-spike/stress-test.ext4
MNT=/Users/ceco/lwext4-spike/mnt
E2=/usr/local/opt/e2fsprogs/sbin
LOG=/Users/ceco/lwext4-spike/stress-mount.log
WORKERS=6
ITERS=150

mkdir -p "$MNT"; rm -f "$LOG"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1048576 count=256 2>/dev/null
"$E2/mke2fs" -t ext4 -F -b 4096 -O ^orphan_file,^metadata_csum_seed -q "$IMG"
echo "mke2fs_rc=$?"

# read-only by default now -> ask for writes explicitly with -o rw
"$BIN" "$IMG" "$MNT" -f -o rw >"$LOG" 2>&1 &
FUSE_PID=$!
echo "fuse_pid=$FUSE_PID"
for i in $(seq 1 40); do
  mount | grep -q " on $MNT " && break
  kill -0 "$FUSE_PID" 2>/dev/null || { echo "died early"; cat "$LOG"; exit 1; }
  sleep 0.25
done
mount | grep "$MNT" || { echo "(NOT MOUNTED)"; exit 1; }

echo "### launching $WORKERS workers x $ITERS iterations (create/write/read/unlink + dirs)"
pids=()
for w in $(seq 1 $WORKERS); do
  (
    set -e
    d="$MNT/dir$w"
    mkdir "$d"
    for i in $(seq 1 $ITERS); do
      f="$d/file$i"
      # 4 KiB deterministic-ish payload
      head -c 4096 /dev/urandom > "/tmp/stress_src_$w"
      dd if="/tmp/stress_src_$w" of="$f" bs=4096 2>/dev/null
      cmp -s "/tmp/stress_src_$w" "$f"
      if [ $((i % 10)) -eq 0 ]; then
        sub="$d/sub$i"; mkdir "$sub"; rmdir "$sub"
      fi
      rm "$f"
    done
    rmdir "$d"
  ) &
  pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || { echo "worker pid $p FAILED"; fail=1; }; done
echo "ALL_WORKERS_OK=$([ $fail -eq 0 ] && echo 1 || echo 0)"

echo "### residual listing:"
ls -la "$MNT"

echo "### unmount"
umount "$MNT" 2>&1 || diskutil unmount "$MNT" 2>&1
echo "umount_rc=$?"
sleep 1; kill "$FUSE_PID" 2>/dev/null; wait "$FUSE_PID" 2>/dev/null

echo "### e2fsck -fn"
"$E2/e2fsck" -fn "$IMG"; echo "E2FSCK_RC=$?"
