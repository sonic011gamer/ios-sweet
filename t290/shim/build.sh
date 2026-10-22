#!/bin/bash
# Build the T290 shim (diagnostics, or iBoot-like xnu loader when given a
# payload) and wrap it into a standard boot image.
# Output: out/t290-diag.img
#   t290/shim/build.sh DTB.img [PAYLOAD]
set -euo pipefail
cd "$(dirname "$0")"

DTB=$1
PAYLOAD=${2:-}

MKBOOTIMG=${MKBOOTIMG:-mkbootimg}

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
    out/start.o out/diag.o out/fdt.o ${FWOBJ:-} -o out/diag.elf
aarch64-linux-gnu-objcopy -O binary out/diag.elf out/Image
size=$(aarch64-linux-gnu-nm out/diag.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image

# Compress the Image using gzip and append the provided DTB
gzip -9c out/Image > out/Image.gz
cat "$DTB" >> out/Image.gz

# Prepare mkbootimg arguments
MKBOOTIMG_ARGS=("--kernel" "out/Image.gz")

if [ -n "$PAYLOAD" ]; then
    MKBOOTIMG_ARGS+=("--ramdisk" "$PAYLOAD")
fi

# Build the standard Android boot image
rm -f out/t290-diag.img
"$MKBOOTIMG" "${MKBOOTIMG_ARGS[@]}" -o out/t290-diag.img

ls -l out/Image out/t290-diag.img