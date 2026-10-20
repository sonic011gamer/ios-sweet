#!/bin/bash
# Build the T290 shim (diagnostics, or iBoot-like xnu loader when given a
# payload) and wrap it like the stock boot/recovery image: gzip Image + the
# stock appended DTBs (magiskboot repack of the stock header), the payload as
# ramdisk, then Samsung's SignerVer02 + AVB footer (fix_bootable_image.py) so
# LK accepts it. Output: out/t290-diag.img (64 MiB, flash to recovery).
#   t290/shim/build.sh STOCK_BOOT.img MAGISKBOOT FIX_BOOTABLE_IMAGE.py [PAYLOAD]
# PAYLOAD: tools/mkpayload.py output (kernel + ADT); without it the shim runs
# the microSD diagnostics. EXTRA_CFLAGS=-DT290_GPU_PROBE builds the GPU probe instead.
set -euo pipefail
cd "$(dirname "$0")"
STOCK=$1 MAGISKBOOT=$2 FIX=$3 PAYLOAD=${4:-}
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra ${EXTRA_CFLAGS:-}"
S=../../shim
mkdir -p out
$CC $CFLAGS -c $S/start.S -o out/start.o
$CC $CFLAGS -c diag.c -o out/diag.o
$CC $CFLAGS -c $S/fdt.c -o out/fdt.o
abs=$(aarch64-linux-gnu-readelf -r out/diag.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in the C objects" >&2; exit 1; }
$CC $CFLAGS -static -T $S/shim.ld -Wl,--build-id=none -Wl,-N \
	out/start.o out/diag.o out/fdt.o -o out/diag.elf
aarch64-linux-gnu-objcopy -O binary out/diag.elf out/Image
size=$(aarch64-linux-gnu-nm out/diag.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Repack the stock header around our Image (kernel stays gzip, DTBs appended).
rm -rf out/pack && mkdir out/pack && cd out/pack
"$MAGISKBOOT" unpack "$STOCK" > /dev/null 2>&1
cp ../Image kernel
if [ -n "$PAYLOAD" ]; then
	# The stock boot image has no ramdisk; magiskboot stores this one as is.
	cp "$PAYLOAD" ramdisk.cpio
fi
"$MAGISKBOOT" repack "$STOCK" new.img > /dev/null 2>&1
if [ -n "$PAYLOAD" ]; then
	# Stock has ramdisk_addr 0; give it TWRP's (LK places it itself anyway).
	printf '\x00\x00\x00\x82' | dd of=new.img bs=1 seek=20 conv=notrunc status=none
	"$MAGISKBOOT" unpack new.img > /dev/null 2>&1 || true
	cmp ramdisk.cpio "$PAYLOAD" || { echo "error: ramdisk does not round-trip" >&2; exit 1; }
fi
cd ../..
rm -f out/t290-diag.img
python3 -I "$FIX" out/pack/new.img out/t290-diag.img
ls -l out/Image out/t290-diag.img
