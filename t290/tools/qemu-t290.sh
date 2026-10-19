#!/bin/bash
# Boot a SWEET_BOARD=t290 kernel in QEMU virt as a Cortex-A53 with a GICv2,
# RAM-disk root = the tiny test init (tools/sweet-init.S), 800x1280 screen.
#   t290/tools/qemu-t290.sh KERNEL OUTDIR [SECS]
# Needs: clang (arm64-apple-ios target), qemu-system-aarch64, llvm-nm, python3.
# QEMU 6.2 (Ubuntu 22.04) enforces the ISA of cortex-a53 (LSE -> UNDEF) but not
# the missing 16K granule; QEMU >= 7.2 also rejects 16K translation tables.
set -euo pipefail
K=$1 OUT=$2 SECS=${3:-90}
cd "$(dirname "$0")/../.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
clang -target arm64-apple-ios16.0 -c tools/sweet-init.S -o "$W/init.o"
python3 tools/mkinit.py "$W/init.o" "$W/sweet-init"
python3 t290/tools/mkadt_t290.py "$W/q.adt" --qemu
python3 tools/mkpayload.py "$K" "$W/q.adt" "$W/payload.bin" --ramdisk "$W/sweet-init" \
	--cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"
rm -rf "$OUT"
python3 tools/qemu-sweet.py "$W/payload.bin" "$OUT" --secs "$SECS" --kernel "$K" --fail-on-panic \
	--cpu cortex-a53 --gic-version 2 --fb 0x90000000,800,1280
