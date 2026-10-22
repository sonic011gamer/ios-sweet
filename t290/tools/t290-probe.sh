#!/bin/bash
# Read-only probe of the tablet over adb (USB debugging on, Android booted).
# Changes nothing on the device. Output: logs/probe-<date>.txt
#   tools/t290-probe.sh
set -u
cd "$(dirname "$0")/.."
mkdir -p logs
out="logs/probe-$(date +%Y%m%d-%H%M%S).txt"
p() { printf '%-36s %s\n' "$1" "$(adb shell getprop "$1" | tr -d '\r')"; }
{
	echo "== adb devices"; adb devices -l
	echo "== props"
	for k in ro.product.model ro.product.device ro.product.board ro.board.platform \
		ro.hardware ro.build.fingerprint ro.build.version.release ro.bootloader \
		ro.boot.bootloader ro.boot.flash.locked ro.boot.verifiedbootstate \
		ro.boot.warranty_bit ro.boot.hardware.revision ro.boot.em.model \
		sys.oem_unlock_allowed ro.oem_unlock_supported ro.frp.pst \
		ro.product.first_api_level ro.boot.dynamic_partitions ro.treble.enabled; do
		p "$k"
	done
	echo "== kernel"; adb shell uname -a
	echo "== cpuinfo"; adb shell cat /proc/cpuinfo
	echo "== meminfo"; adb shell head -3 /proc/meminfo
	echo "== cmdline (may need root)"; adb shell cat /proc/cmdline 2>&1
	echo "== partitions"; adb shell ls -l /dev/block/by-name/ 2>&1 || adb shell ls -l /dev/block/platform/*/by-name/ 2>&1
	echo "== fb / display"; adb shell wm size; adb shell wm density
	echo "== input devices"; adb shell getevent -pl 2>&1 | grep -E "add device|name:"
	echo "== i2c devices"; adb shell ls /sys/bus/i2c/devices/ 2>&1
	echo "== dt model (if readable)"; adb shell cat /proc/device-tree/model 2>&1; echo
	adb shell cat /proc/device-tree/qcom,board-id 2>&1 | od -An -tx1
} > "$out" 2>&1
echo "saved $out"
