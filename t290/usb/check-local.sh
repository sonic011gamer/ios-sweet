#!/bin/bash
# Syntax-check only the changed kernel files using a cached xnu-buildtree.
set -euo pipefail
base=${1:-/home/kirill/bt}
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d /tmp/t290-usb-check.XXXXXX)
echo "$work"
cp -a "$base/xnu" "$work/xnu"
for name in build fakeroot sdk-kernel-platform; do ln -s "$base/$name" "$work/$name"; done
python3 "$repo/t290/usb/mkpatch.py" "$base/xnu" "$repo/xnu/patches/0131-t290-usb-screen-debug.patch"
patch -d "$work/xnu" -p1 < "$repo/xnu/patches/0131-t290-usb-screen-debug.patch"
export XNU_CC_EXTRA="-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang"
export XNU_CC_POST="-DSWEET_T290=1 -march=armv8-a+crc+crypto -UARM_LARGE_MEMORY -UVM_KERNEL_LINK_ADDRESS -DVM_KERNEL_LINK_ADDRESS=0xfffffff007004000"
python3 "$repo/tools/xnu-cc.py" "$base/run.log" "$work" \
    bsd/dev/sweet_t290_usb.c bsd/dev/sweet_sdhc.c iokit/Kernel/arm/SweetFramebuffer.cpp
