#!/bin/bash
# Recovery-partition image that boots the iPadOS 16 root on the microSD card:
# t290 shim (LK -> xnu loader) + payload (SWEET_BOARD=t290 kernel + ADT).
#   t290/tools/mkboot-t290.sh KERNEL OUT.img STOCK_BOOT.img MAGISKBOOT FIX_BOOTABLE_IMAGE.py
# The card must hold the root image itself (no partition table), written e.g.
# with dd from Android: ipad5-root.img -> /dev/block/mmcblk1.
# EXTRA="..." appends boot-args (e.g. EXTRA="sdhs=0" keeps the card at 25 MHz).
set -euo pipefail
K=$1 OUT=$2 STOCK=$3 MAGISKBOOT=$4 FIX=$5
cd "$(dirname "$0")/../.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
python3 t290/tools/mkadt_t290.py "$W/t290.adt"
# sweet's phone boot-args (README) with the microSD root. CPUS=N cores
# (default 1; secondaries start with PSCI CPU_ON over SMC).
ARGS="debug=0xc serial=3 cpus=${CPUS:-1} keepsyms=1 -enable_kprintf_spam -noprogress"
ARGS="$ARGS rd=sdroot0 sdrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1"
ARGS="$ARGS wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=10 msgbuf=8388608"
ARGS="$ARGS sweetkeys=0 sweettouch=0 ${EXTRA:-}"
python3 tools/mkpayload.py "$K" "$W/t290.adt" "$W/payload.bin" --cmdline "$ARGS"
bash t290/shim/build.sh "$STOCK" "$MAGISKBOOT" "$FIX" "$W/payload.bin"
cp t290/shim/out/t290-diag.img "$OUT"
ls -l "$OUT"
