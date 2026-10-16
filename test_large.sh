#!/bin/bash
# test_large.sh — large-file + many-file + deep-tree stress on an already
# mounted lwext4fs (macFUSE) filesystem.
#
# Usage: test_large.sh <mountpoint>
#
# Creates a scratch directory, writes many small files and a couple of large
# random files (verifying them with md5), builds a deep directory tree, then
# removes everything. Exits non-zero on any mismatch.
set -u

MP="${1:-}"
if [ -z "$MP" ] || ! mount | grep -q " $MP "; then
	echo "usage: test_large.sh <mountpoint>   (must be a mounted lwext4fs)"
	exit 2
fi

D="$MP/lwext4fs-stress"
RC=0

echo "== many small files =="
rm -rf "$D" 2>/dev/null
mkdir -p "$D" || { echo "  cannot create $D"; exit 1; }
for i in $(seq 1 300); do
	printf 'small %d %s\n' "$i" "$(openssl rand -hex 12)" > "$D/small_$i.txt"
done
echo "  created $(ls "$D"/small_*.txt 2>/dev/null | wc -l | tr -d ' ') files"

echo "== large files (md5 round-trip) =="
for mb in 64 256; do
	dd if=/dev/urandom of="/tmp/lwext4fs_src_$mb.bin" bs=1048576 count=$mb 2>/dev/null
	cp "/tmp/lwext4fs_src_$mb.bin" "$D/big_${mb}m.bin"
	a=$(md5 -q "/tmp/lwext4fs_src_$mb.bin")
	b=$(md5 -q "$D/big_${mb}m.bin")
	if [ "$a" = "$b" ]; then
		echo "  big_${mb}m.bin  MATCH  ($a)"
	else
		echo "  big_${mb}m.bin  MISMATCH  ($a != $b)"; RC=1
	fi
done

echo "== deep directory tree =="
mkdir -p "$D/a/b/c/d/e" && echo deep > "$D/a/b/c/d/e/deep.txt" \
	&& echo "  read back: $(cat "$D/a/b/c/d/e/deep.txt")" || RC=1

echo "== df =="
df -h "$MP" | tail -1

echo "== cleanup =="
rm -rf "$D"
rm -f /tmp/lwext4fs_src_*.bin
if [ -e "$D" ]; then echo "  cleanup FAILED"; RC=1; else echo "  cleanup OK"; fi

echo "test_large: $([ $RC -eq 0 ] && echo PASS || echo FAIL)"
exit $RC
