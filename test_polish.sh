#!/bin/bash
# Polish-round tests for lwext4fs-mac:
#   Feature 1 - timestamps updated on write (mtime/ctime bumped)
#   Feature 2 - precise lwext4 -> POSIX errno mapping
#   Feature 3 - statfs accuracy (df -h sane, shrinks after a big write)
#
# ONLY touches throwaway images under /Users/ceco/lwext4-spike/.
set -u

BIN=/Users/ceco/lwext4-spike/lwext4fs-mac/lwext4fs-mac
E2=/usr/local/opt/e2fsprogs/sbin
BASE=/Users/ceco/lwext4-spike
MKE2FS_OPTS="-t ext4 -F -b 4096 -O ^orphan_file"

PASS=0
FAIL=0
note() { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); note "  PASS: $*"; }
bad()  { FAIL=$((FAIL+1)); note "  FAIL: $*"; }

mount_image() {
  local img="$1" mnt="$2"; shift 2
  local log="$mnt.log"
  mkdir -p "$mnt"; rm -f "$log"
  "$BIN" "$img" "$mnt" -f "$@" >"$log" 2>&1 &
  FUSE_PID=$!
  local i
  for i in $(seq 1 40); do
    if mount | grep -q " on $mnt "; then return 0; fi
    if ! kill -0 "$FUSE_PID" 2>/dev/null; then
      note "!!! fuse died early; log:"; cat "$log"; return 1
    fi
    sleep 0.25
  done
  note "!!! mount timeout; log:"; cat "$log"; return 1
}

unmount_image() {
  local mnt="$1"
  umount "$mnt" 2>/dev/null || diskutil unmount "$mnt" >/dev/null 2>&1
  local rc=$? i
  for i in $(seq 1 20); do kill -0 "$FUSE_PID" 2>/dev/null || break; sleep 0.25; done
  kill "$FUSE_PID" 2>/dev/null; wait "$FUSE_PID" 2>/dev/null
  return $rc
}

fresh_img() { rm -f "$1"; "$E2/mke2fs" $MKE2FS_OPTS "$1" "$2" >/dev/null 2>&1; }

errno_of() { python3 -c "import errno;print(errno.$1)"; }

########################################################################
note "=============================================================="
note "Feature 1: timestamps updated on write"
note "=============================================================="
IMG=$BASE/p_mtime.ext4; MNT=$BASE/mnt_mtime
fresh_img "$IMG" 128M
if mount_image "$IMG" "$MNT" -o rw; then
  echo "aaa" > "$MNT/t.txt"
  MT0=$(stat -f '%m' "$MNT/t.txt"); CT0=$(stat -f '%c' "$MNT/t.txt")
  note "  after create:      mtime=$MT0 ctime=$CT0"
  sleep 1
  echo "bbb" >> "$MNT/t.txt"      # append -> write() at offset
  MT1=$(stat -f '%m' "$MNT/t.txt"); CT1=$(stat -f '%c' "$MNT/t.txt")
  note "  after append+1s:   mtime=$MT1 ctime=$CT1"
  NOW=$(date +%s)
  [ "$MT1" -gt 1000000000 ] && ok "mtime is non-epoch ($MT1)" || bad "mtime epoch ($MT1)"
  [ "$MT1" -gt "$MT0" ] && ok "mtime advanced after write ($MT0 -> $MT1)" || bad "mtime did NOT advance ($MT0 -> $MT1)"
  [ "$CT1" -ge "$MT1" ] && ok "ctime bumped ($CT1)" || bad "ctime not bumped ($CT1)"
  skew=$(( NOW - MT1 )); if [ "$skew" -lt 0 ]; then skew=$(( -skew )); fi
  [ "$skew" -le 120 ] && ok "mtime is current (skew ${skew}s)" || bad "mtime skew too large (${skew}s)"
  # truncate path also bumps mtime
  MT2=$MT1
  sleep 1
  : > "$MNT/t.txt"
  MT3=$(stat -f '%m' "$MNT/t.txt")
  note "  after truncate+1s: mtime=$MT3"
  [ "$MT3" -gt "$MT2" ] && ok "mtime advanced after truncate+write" || bad "mtime not bumped by truncate ($MT2 -> $MT3)"
  # read must NOT modify anything (read-only semantics for data reads)
  MT4=$(stat -f '%m' "$MNT/t.txt")
  cat "$MNT/t.txt" >/dev/null
  MT5=$(stat -f '%m' "$MNT/t.txt")
  [ "$MT4" = "$MT5" ] && ok "plain read does not touch mtime ($MT5)" || bad "read changed mtime ($MT4 -> $MT5)"
  unmount_image "$MNT"
  "$E2/e2fsck" -fn "$IMG" >/tmp/fsck_mtime.out 2>&1
  [ $? -eq 0 ] && ok "e2fsck rc=0 after mtime scenario" || { bad "e2fsck nonzero"; cat /tmp/fsck_mtime.out; }
fi

########################################################################
note "=============================================================="
note "Feature 2: error mapping (exact errnos)"
note "=============================================================="

# --- ENOENT / EISDIR / ENOTEMPTY / EEXIST on a normal image ---
IMG=$BASE/p_err.ext4; MNT=$BASE/mnt_err
fresh_img "$IMG" 128M
if mount_image "$IMG" "$MNT" -o rw; then
  mkdir "$MNT/dir"; echo "x" > "$MNT/dir/inner"; echo "y" > "$MNT/file.txt"
  RES=$(python3 - "$MNT" <<'PY'
import os, sys
m = sys.argv[1]
def t(desc, fn):
    try:
        fn(); print("%-22s rc=0 (no error!)" % desc)
    except OSError as e:
        print("%-22s errno=%d" % (desc, e.errno))
t("ENOENT open missing",  lambda: open(os.path.join(m, "nope.txt")))
t("EISDIR open dir w",    lambda: open(os.path.join(m, "dir"), "w"))
t("EEXIST mkdir existing",lambda: os.mkdir(os.path.join(m, "dir")))
t("EEXIST O_EXCL exist",  lambda: os.open(os.path.join(m, "file.txt"), os.O_CREAT|os.O_EXCL|os.O_WRONLY))
t("ENOTEMPTY rmdir",      lambda: os.rmdir(os.path.join(m, "dir")))
PY
)
  note "$RES"
  chk() { # chk <label> <expected> <pattern>
    local got; got=$(echo "$RES" | grep -F "$2" | sed -n 's/.*errno=\([0-9]*\).*/\1/p')
    if [ "$got" = "$3" ]; then ok "$1: errno=$got"; else bad "$1: expected $3 got '$got'"; fi
  }
  chk "ENOENT open missing"   "ENOENT open missing"   "$(errno_of ENOENT)"
  chk "EISDIR open dir w"     "EISDIR open dir w"     "$(errno_of EISDIR)"
  chk "EEXIST mkdir existing" "EEXIST mkdir existing" "$(errno_of EEXIST)"
  chk "EEXIST O_EXCL exist"   "EEXIST O_EXCL exist"   "$(errno_of EEXIST)"
  chk "ENOTEMPTY rmdir"       "ENOTEMPTY rmdir"       "$(errno_of ENOTEMPTY)"
  unmount_image "$MNT"
fi

# --- ENOSPC: fill a small image ---
IMG=$BASE/p_full.ext4; MNT=$BASE/mnt_full
fresh_img "$IMG" 16M
if mount_image "$IMG" "$MNT" -o rw; then
  note "  image is 16 MiB; writing 64 MiB blob to force ENOSPC"
  RES=$(python3 - "$MNT" <<'PY'
import os, sys
m = sys.argv[1]
p = os.path.join(m, "big.bin")
blk = b"\x5a" * (1024 * 1024)
try:
    with open(p, "wb") as f:
        for i in range(64):
            f.write(blk)
            f.flush()
    print("ENOSPC write big    rc=0 (unexpectedly fit!)")
except OSError as e:
    print("ENOSPC write big    errno=%d" % e.errno)
PY
)
  note "$RES"
  got=$(echo "$RES" | sed -n 's/.*errno=\([0-9]*\).*/\1/p')
  if [ "$got" = "$(errno_of ENOSPC)" ]; then ok "ENOSPC fill: errno=$got"; else bad "ENOSPC fill: expected $(errno_of ENOSPC) got '$got'"; fi
  note "  df after fill:"; df -h "$MNT" | sed 's/^/    /'
  unmount_image "$MNT"
  "$E2/e2fsck" -fn "$IMG" >/tmp/fsck_full.out 2>&1
  [ $? -eq 0 ] && ok "e2fsck rc=0 after ENOSPC scenario" || { bad "e2fsck nonzero"; cat /tmp/fsck_full.out; }
fi

########################################################################
note "=============================================================="
note "Feature 3: statfs accuracy (df -h)"
note "=============================================================="
IMG=$BASE/p_df.ext4; MNT=$BASE/mnt_df
fresh_img "$IMG" 256M
if mount_image "$IMG" "$MNT" -o rw; then
  note "  df -h before write:"; df -h "$MNT" | sed 's/^/    /'
  AV0=$(df -k "$MNT" | awk 'NR==2{print $4}')
  SZ0=$(df -k "$MNT" | awk 'NR==2{print $2}')
  US0=$(df -k "$MNT" | awk 'NR==2{print $3}')
  note "  parsed: size=${SZ0}K used=${US0}K avail=${AV0}K"
  [ "$SZ0" -gt 200000 ] && ok "reported size ~256M (${SZ0}K)" || bad "size looks wrong (${SZ0}K)"
  [ "$AV0" -lt "$SZ0" ] && ok "avail < size (reserved blocks accounted: ${AV0}K < ${SZ0}K)" || bad "avail >= size; reserved not subtracted (${AV0}K)"
  dd if=/dev/zero of="$MNT/blob" bs=1m count=100 2>/dev/null
  sync
  note "  df -h after 100 MiB write:"; df -h "$MNT" | sed 's/^/    /'
  AV1=$(df -k "$MNT" | awk 'NR==2{print $4}')
  note "  parsed: avail ${AV0}K -> ${AV1}K"
  [ "$AV1" -lt "$AV0" ] && ok "available shrank after big write (${AV0}K -> ${AV1}K)" || bad "available did not shrink (${AV0}K -> ${AV1}K)"
  unmount_image "$MNT"
  "$E2/e2fsck" -fn "$IMG" >/tmp/fsck_df.out 2>&1
  [ $? -eq 0 ] && ok "e2fsck rc=0 after statfs scenario" || { bad "e2fsck nonzero"; cat /tmp/fsck_df.out; }
fi

########################################################################
note "=============================================================="
note "SUMMARY: PASS=$PASS FAIL=$FAIL"
note "=============================================================="
[ "$FAIL" -eq 0 ]
