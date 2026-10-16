#!/bin/bash
# Feature tests for lwext4fs-mac:
#   Feature 1 - read-only by default (writes need "-o rw")
#   Feature 2 - symlink / readlink / hardlink
#   Feature 3 - xattr (user.* namespace only)
#
# ONLY touches throwaway images under /Users/ceco/lwext4-spike/.
set -u

BIN=/Users/ceco/lwext4-spike/lwext4fs-mac/lwext4fs-mac
E2=/usr/local/opt/e2fsprogs/sbin
BASE=/Users/ceco/lwext4-spike
MKE2FS_OPTS="-t ext4 -F -b 4096 -O ^orphan_file,^metadata_csum_seed"
IMG_SIZE=128M

PASS=0
FAIL=0
declare -a SUMMARY

note()  { printf '%s\n' "$*"; }
ok()    { PASS=$((PASS+1)); note "  PASS: $*"; }
bad()   { FAIL=$((FAIL+1)); note "  FAIL: $*"; }

check() { # check <desc> <expected_rc> <actual_rc>
  if [ "$2" = "$3" ]; then ok "$1 (rc=$3)"; else bad "$1 (expected rc=$2 got rc=$3)"; fi
}

# mount_image <img> <mnt> <extra fuse args...>
# Starts the fs in the background, waits for the mount to appear.
mount_image() {
  local img="$1" mnt="$2"; shift 2
  local log="$mnt.log"
  mkdir -p "$mnt"
  rm -f "$log"
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

# unmount_image <mnt>
unmount_image() {
  local mnt="$1"
  umount "$mnt" 2>/dev/null || diskutil unmount "$mnt" >/dev/null 2>&1
  local rc=$?
  local i
  for i in $(seq 1 20); do
    kill -0 "$FUSE_PID" 2>/dev/null || break
    sleep 0.25
  done
  kill "$FUSE_PID" 2>/dev/null
  wait "$FUSE_PID" 2>/dev/null
  return $rc
}

# fsck <img> <label>
fsck() {
  "$E2/e2fsck" -fn "$1" >/tmp/fsck_$2.out 2>&1
  local rc=$?
  echo "  --- e2fsck -fn $1 (rc=$rc) ---"
  cat /tmp/fsck_$2.out
  return $rc
}

fresh_img() { # <path>
  rm -f "$1"
  "$E2/mke2fs" $MKE2FS_OPTS "$1" $IMG_SIZE >/dev/null 2>&1
}

########################################################################
note "=============================================================="
note "Feature 1a: READ-ONLY BY DEFAULT (no -o rw)"
note "=============================================================="
IMG=$BASE/t_ro.ext4; MNT=$BASE/mnt_ro
fresh_img "$IMG"
if mount_image "$IMG" "$MNT"; then
  note "mount flags:"; mount | grep " on $MNT "
  # ---- reads must work: create a file first via an rw mount ----
  unmount_image "$MNT"
  mount_image "$IMG" "$MNT" -o rw
  echo "seed" > "$MNT/seed.txt"
  check "seed write on rw mount" 0 $?
  unmount_image "$MNT"
  # ---- now read-only mount ----
  mount_image "$IMG" "$MNT"
  note "mount flags (expect read-only):"; mount | grep " on $MNT "
  if mount | grep " on $MNT " | grep -q "read-only"; then
    ok "mount is read-only"
  else
    bad "mount is NOT read-only"
  fi
  echo "hi" > "$MNT/newfile.txt" 2>/dev/null
  rc=$?
  if [ $rc -ne 0 ]; then ok "touch/write rejected (rc=$rc)"; else bad "write unexpectedly succeeded"; fi
  # errno check via python
  RO_ERR=$(python3 - "$MNT" <<'PY'
import sys, os, errno
m = sys.argv[1]
try:
    open(os.path.join(m, "x.txt"), "w").write("x")
    print("NOERROR")
except OSError as e:
    print(e.errno)
PY
)
  note "  errno from python write attempt: $RO_ERR (EROFS=%s)" "$(python3 -c 'import errno;print(errno.EROFS)')"
  [ "$RO_ERR" = "$(python3 -c 'import errno;print(errno.EROFS)')" ] && ok "errno is EROFS" || bad "errno is not EROFS: $RO_ERR"
  rm -f "$MNT/seed.txt" 2>/dev/null
  check "rm on ro mount fails" 1 $?
  mkdir "$MNT/rodir" 2>/dev/null
  check "mkdir on ro mount fails" 1 $?
  note "  read of seed.txt: $(cat "$MNT/seed.txt" 2>&1)"
  unmount_image "$MNT"
  fsck "$IMG" ro && ok "e2fsck rc=0 after ro scenario" || bad "e2fsck nonzero after ro scenario"
fi

########################################################################
note "=============================================================="
note "Feature 1b: READ-WRITE with -o rw"
note "=============================================================="
IMG=$BASE/t_rw.ext4; MNT=$BASE/mnt_rw
fresh_img "$IMG"
if mount_image "$IMG" "$MNT" -o rw; then
  note "mount flags:"; mount | grep " on $MNT "
  if mount | grep " on $MNT " | grep -q "read-only"; then
    bad "rw mount is marked read-only"
  else
    ok "rw mount is writable"
  fi
  echo "hello rw" > "$MNT/a.txt"; check "write a.txt" 0 $?
  note "  read back: $(cat "$MNT/a.txt")"
  cat "$MNT/a.txt" | grep -q "hello rw" && ok "content matches" || bad "content mismatch"
  mkdir "$MNT/dirw"; check "mkdir dirw" 0 $?
  unmount_image "$MNT"
  fsck "$IMG" rw && ok "e2fsck rc=0 after rw scenario" || bad "e2fsck nonzero after rw scenario"
fi

########################################################################
note "=============================================================="
note "Feature 2: symlink / readlink / hardlink"
note "=============================================================="
IMG=$BASE/t_link.ext4; MNT=$BASE/mnt_link
fresh_img "$IMG"
if mount_image "$IMG" "$MNT" -o rw; then
  printf 'target-payload-42\n' > "$MNT/real.txt"
  # --- symlink: short (fast) target ---
  ln -s real.txt "$MNT/soft"
  check "create short symlink" 0 $?
  tg=$(readlink "$MNT/soft"); rc=$?
  check "readlink short" 0 $rc
  [ "$tg" = "real.txt" ] && ok "short symlink target = '$tg'" || bad "short target wrong: '$tg'"
  out=$(cat "$MNT/soft" 2>&1)
  [ "$out" = "target-payload-42" ] && ok "following short symlink reads target" || bad "follow short failed: '$out'"
  note "  ls -l soft: $(ls -l "$MNT/soft")"
  # --- symlink: long target (> 60 bytes -> data block path) ---
  LONGTGT="this/is/a/very/very/very/very/very/long/target/path/that/exceeds/sixty/bytes"
  ln -s "$LONGTGT" "$MNT/softlong"
  check "create long symlink" 0 $?
  tg2=$(readlink "$MNT/softlong")
  [ "$tg2" = "$LONGTGT" ] && ok "long symlink target round-trips" || bad "long target wrong: '$tg2'"
  # --- hardlink ---
  ln "$MNT/real.txt" "$MNT/hard.txt"
  check "create hardlink" 0 $?
  nl=$(stat -f '%l' "$MNT/real.txt")
  [ "$nl" = "2" ] && ok "real.txt nlink=$nl" || bad "real.txt nlink=$nl (expected 2)"
  c1=$(cat "$MNT/real.txt"); c2=$(cat "$MNT/hard.txt")
  [ "$c1" = "$c2" ] && ok "hardlink content identical" || bad "hardlink content differs"
  # modify through one name, visible through the other
  printf 'CHANGED\n' > "$MNT/hard.txt"
  [ "$(cat "$MNT/real.txt")" = "CHANGED" ] && ok "write via hardlink visible on original" || bad "hardlink not shared"
  # remove original; hardlink must survive
  rm "$MNT/real.txt"; check "unlink original" 0 $?
  [ -f "$MNT/hard.txt" ] && ok "hardlink survives unlink of original" || bad "hardlink vanished"
  nl2=$(stat -f '%l' "$MNT/hard.txt")
  [ "$nl2" = "1" ] && ok "nlink dropped to 1" || bad "nlink after unlink = $nl2"
  ls -la "$MNT"
  unmount_image "$MNT"
  fsck "$IMG" link && ok "e2fsck rc=0 after link scenario" || bad "e2fsck nonzero after link scenario"
fi

########################################################################
note "=============================================================="
note "Feature 3: xattr (user.* namespace)"
note "=============================================================="
IMG=$BASE/t_xattr.ext4; MNT=$BASE/mnt_xattr
fresh_img "$IMG"
if mount_image "$IMG" "$MNT" -o rw; then
  echo "xattr file" > "$MNT/f.txt"
  # --- direct syscalls (macOS python's os module has no xattr support) ---
  python3 - "$MNT" <<'PY'
import ctypes, ctypes.util, os, sys
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
p = os.path.join(sys.argv[1], "f.txt").encode()
libc.setxattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int]; libc.setxattr.restype=ctypes.c_int
libc.getxattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int]; libc.getxattr.restype=ctypes.c_ssize_t
libc.listxattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_int]; libc.listxattr.restype=ctypes.c_ssize_t
libc.removexattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int]; libc.removexattr.restype=ctypes.c_int
fails=0
def sx(n,v):
    ctypes.set_errno(0); r=libc.setxattr(p,n,v,len(v),0,0); print("  setxattr",n.decode(),"->",r,"(errno",ctypes.get_errno(),")"); return r
def gx(n):
    ctypes.set_errno(0); b=ctypes.create_string_buffer(256); r=libc.getxattr(p,n,b,256,0,0); print("  getxattr",n.decode(),"->",r,repr(b.raw[:max(r,0)])); return r,b.raw[:max(r,0)]
def lx():
    ctypes.set_errno(0); b=ctypes.create_string_buffer(1024); r=libc.listxattr(p,b,1024,0); print("  listxattr ->",r,repr(b.raw[:max(r,0)])); return r,b.raw[:max(r,0)]
def rx(n):
    ctypes.set_errno(0); r=libc.removexattr(p,n,0); print("  removexattr",n.decode(),"->",r,"(errno",ctypes.get_errno(),")"); return r
# size probe: getxattr size=0 must report the length
ctypes.set_errno(0); probe=libc.getxattr(p,b"user.none",None,0,0,0); print("  probe missing ->",probe,"(ENOATTR=%d)" % 93)
if sx(b"user.test",b"hello")!=0: fails+=1
r,val=gx(b"user.test")
if not (r==5 and val==b"hello"): fails+=1
lr,lb=lx()
if b"user.test\x00" not in lb: fails+=1
# size probe for existing attr
ctypes.set_errno(0); psz=libc.getxattr(p,b"user.test",None,0,0,0); print("  size probe user.test ->",psz)
if psz!=5: fails+=1
if rx(b"user.test")!=0: fails+=1
r2,val2=gx(b"user.test")
if r2!=-1: fails+=1
lr2,lb2=lx()
if b"user.test" in lb2: fails+=1
print("  PYTHON_XATTR_RESULT", "OK" if fails==0 else "FAIL(%d)"%fails)
sys.exit(1 if fails else 0)
PY
  check "ctypes xattr set/get/list/probe/remove" 0 $?
  # --- xattr CLI round-trip ---
  xattr -w user.hello world "$MNT/f.txt"; check "xattr -w user.hello" 0 $?
  v=$(xattr -p user.hello "$MNT/f.txt" 2>&1)
  [ "$v" = "world" ] && ok "xattr -p user.hello = world" || bad "xattr -p got '$v'"
  note "  xattr -l:"; xattr -l "$MNT/f.txt"
  xattr -d user.hello "$MNT/f.txt"; check "xattr -d user.hello" 0 $?
  note "  after delete, xattr -l:"; xattr -l "$MNT/f.txt"
  if xattr -p user.hello "$MNT/f.txt" >/dev/null 2>&1; then
    bad "user.hello still present after xattr -d"
  else
    ok "user.hello really removed by xattr -d"
  fi
  # --- macOS namespace must be refused (not stored) ---
  python3 - "$MNT" <<'PY2'
import ctypes, ctypes.util, os, sys
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
p = os.path.join(sys.argv[1], "f.txt").encode()
libc.setxattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int]; libc.setxattr.restype=ctypes.c_int
ctypes.set_errno(0); r=libc.setxattr(p,b"com.apple.provenance",b"xyz",3,0,0)
print("  setxattr com.apple.provenance ->",r,"errno",ctypes.get_errno(),"(ENOTSUP=%d)" % 45)
PY2
  if xattr -p com.apple.provenance "$MNT/f.txt" >/dev/null 2>&1; then
    bad "com.apple.* attribute was stored on the image"
  else
    ok "com.apple.* not stored (policy holds)"
  fi
  note "  final xattr -l:"; xattr -l "$MNT/f.txt"
  unmount_image "$MNT"
  fsck "$IMG" xattr && ok "e2fsck rc=0 after xattr scenario" || bad "e2fsck nonzero after xattr scenario"
fi

########################################################################
note "=============================================================="
note "SUMMARY: PASS=$PASS FAIL=$FAIL"
note "=============================================================="
[ "$FAIL" -eq 0 ]
