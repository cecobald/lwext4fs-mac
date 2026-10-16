#!/usr/bin/env bash
#
# build.sh — one-shot build for lwext4fs-mac.
#
# Fetches the lwext4 library at a pinned commit, applies the patches under
# patches/, builds lwext4 (static libs) and finally builds lwext4fs-mac.
#
# Requirements: git, cc, make, cmake, pkg-config, and macFUSE (pkg-config "fuse").
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

LWEXT4_REPO="${LWEXT4_REPO:-https://github.com/gkostka/lwext4.git}"
LWEXT4_COMMIT="${LWEXT4_COMMIT:-58bcf89a121b72d4fb66334f1693d3b30e4cb9c5}"
VENDOR="$HERE/vendor/lwext4"

echo "==> checking dependencies"
for t in git cc make cmake pkg-config; do
	command -v "$t" >/dev/null 2>&1 || { echo "ERROR: missing '$t'"; exit 1; }
done
if ! pkg-config --exists fuse; then
	echo "ERROR: macFUSE (pkg-config 'fuse') not found."
	echo "       Install macFUSE 4.x: https://macfuse.github.io/  (or: brew install --cask macfuse)"
	exit 1
fi

echo "==> fetching lwext4 @ $LWEXT4_COMMIT"
if [ ! -d "$VENDOR/.git" ]; then
	git clone "$LWEXT4_REPO" "$VENDOR"
fi
git -C "$VENDOR" fetch --quiet origin "$LWEXT4_COMMIT" 2>/dev/null || true
git -C "$VENDOR" checkout --quiet "$LWEXT4_COMMIT"

echo "==> applying patches"
shopt -s nullglob
for p in "$HERE"/patches/*.patch; do
	if git -C "$VENDOR" apply --check "$p" 2>/dev/null; then
		git -C "$VENDOR" apply "$p"
		echo "    applied $(basename "$p")"
	else
		echo "    (skip) $(basename "$p") — already applied or does not apply"
	fi
done

echo "==> building lwext4"
( cd "$VENDOR" && rm -rf build_generic && mkdir build_generic && cd build_generic && \
  cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=../toolchain/generic.cmake \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. >/dev/null && \
  make -j"$(sysctl -n hw.ncpu)" >/dev/null )

echo "==> building lwext4fs-mac"
make clean >/dev/null 2>&1 || true
make LWEXT4="$VENDOR"

echo
echo "==> done: $HERE/lwext4fs-mac"
echo "    run:  sudo ./lwext4fs-mac <device|image> <mountpoint> [-o rw]"
