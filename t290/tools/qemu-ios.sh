#!/bin/bash
# Boot the iPadOS 16 root (tools/mkiosroot.sh + tools/sweet-root-tweaks.py,
# iPad 5 IPSW) on a SWEET_BOARD=t290 kernel in QEMU: Cortex-A53, GICv2, root on
# a virtio-blk disk (rd=vblkroot0), 1536x2048 framebuffer = the iPad 5 panel.
#   t290/tools/qemu-ios.sh KERNEL ROOT.img OUTDIR [SECS] [SHOT_INTERVAL]
# OUTDIR gets shot-SSSS.png every SHOT_INTERVAL seconds (boot timeline), the
# final fb.png/fb.txt, regs.txt and qemu.log. The disk is written (vblkrw=1):
# keep a pristine copy of ROOT.img. SMP=N (default 4) vCPUs, PSCI over HVC.
# GDBPORT=P exposes the gdb stub for t290/tools/kmem.py while it runs.
set -euo pipefail
K=$1 ROOT=$2 OUT=$3 SECS=${4:-3600} SHOT=${5:-30} SMP=${SMP:-4}
cd "$(dirname "$0")/../.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
python3 t290/tools/mkadt_t290.py "$W/q.adt" --qemu --smp "$SMP" --fb 1536x2048
# sweet's phone boot-args (README) with the virtio root instead of UFS.
ARGS="debug=0xc serial=3 cpus=$SMP sweetpsci=hvc keepsyms=1 -enable_kprintf_spam -noprogress"
ARGS="$ARGS rd=vblkroot0 vblkrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1"
ARGS="$ARGS wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=10 msgbuf=8388608 sweetsample=SpringBoard,backboardd,com.apple.datamigrator"
python3 tools/mkpayload.py "$K" "$W/q.adt" "$W/payload.bin" --cmdline "$ARGS"
rm -rf "$OUT"
python3 tools/qemu-sweet.py "$W/payload.bin" "$OUT" --secs "$SECS" --shots "$SHOT" --kernel "$K" \
	--cpu cortex-a53 --gic-version 2 --smp "$SMP" --fb 0x90000000,1536,2048 --disk "$ROOT" \
	${GDBPORT:+--gdbport $GDBPORT}
