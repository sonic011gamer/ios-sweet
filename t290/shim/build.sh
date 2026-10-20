#!/bin/bash
# Build the T290 diagnostic shim and wrap it like the stock boot/recovery
# image: gzip Image + the stock appended DTBs (magiskboot repack of the stock
# header), then Samsung's SignerVer02 + AVB footer (fix_bootable_image.py) so
# LK accepts it. Output: out/t290-diag.img (64 MiB, flash to recovery).
#   t290/shim/build.sh STOCK_BOOT.img MAGISKBOOT FIX_BOOTABLE_IMAGE.py
set -euo pipefail
cd "$(dirname "$0")"
STOCK=$1 MAGISKBOOT=$2 FIX=$3
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
S=../../shim
mkdir -p out
$CC $CFLAGS -c $S/start.S -o out/start.o
$CC $CFLAGS -c diag.c -o out/diag.o
abs=$(aarch64-linux-gnu-readelf -r out/diag.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in diag.o" >&2; exit 1; }
$CC $CFLAGS -static -T $S/shim.ld -Wl,--build-id=none -Wl,-N \
	out/start.o out/diag.o -o out/diag.elf
aarch64-linux-gnu-objcopy -O binary out/diag.elf out/Image
size=$(aarch64-linux-gnu-nm out/diag.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Repack the stock header around our Image (kernel stays gzip, DTBs appended).
rm -rf out/pack && mkdir out/pack && cd out/pack
"$MAGISKBOOT" unpack "$STOCK" > /dev/null 2>&1
cp ../Image kernel
"$MAGISKBOOT" repack "$STOCK" new.img > /dev/null 2>&1
cd ../..
rm -f out/t290-diag.img
python3 -I "$FIX" out/pack/new.img out/t290-diag.img
ls -l out/Image out/t290-diag.img
