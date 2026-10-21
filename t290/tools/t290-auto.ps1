# Unattended shim command (t290/shim/diag.c auto_command): write
# "T290CMD1" + nonce + command into microSD sector 33556480 (1 MiB past the
# 16 GiB HFSX root volume), reboot to recovery with t290/tools/reboot2
# (IMEM reason only: Android's "reboot recovery" also sets misc and LK then
# looped into recovery); the shim wipes the command, runs it and reboots to
# Android. Then -Log prints the shim's pstore log.
#   t290-auto.ps1 gpu          run the GPU diagnostics
#   t290-auto.ps1 -Log [file]  after the tablet is back in Android
param([string]$Cmd = 'gpu', [switch]$Log, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$adb = 'D:\test1\tools-win\platform-tools\adb.exe'
$su = '/debug_ramdisk/su'
if ($Log) {
	& $adb wait-for-device
	$text = & $adb shell "$su -c 'cat /sys/fs/pstore/console-ramoops-0'"
	if ($Out) { $text | Out-File -Encoding utf8 $Out }
	$text
	return
}
$b = New-Object byte[] 512
[Text.Encoding]::ASCII.GetBytes('T290CMD1').CopyTo($b, 0)
[BitConverter]::GetBytes([uint32]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() -band 0xffffffff)).CopyTo($b, 8)
[Text.Encoding]::ASCII.GetBytes($Cmd).CopyTo($b, 12)
$f = Join-Path $env:TEMP 't290cmd.bin'
[IO.File]::WriteAllBytes($f, $b)
& $adb push $f /data/local/tmp/t290cmd.bin | Out-Null
& $adb push (Join-Path $PSScriptRoot 'reboot2') /data/local/tmp/reboot2 | Out-Null
& $adb shell "$su -c 'dd if=/data/local/tmp/t290cmd.bin of=/dev/block/mmcblk1 bs=512 seek=33556480 count=1 conv=notrunc && sync'"
$back = & $adb shell "$su -c 'dd if=/dev/block/mmcblk1 bs=512 skip=33556480 count=1 2>/dev/null | head -c 8'"
if ($back -ne 'T290CMD1') { throw "command sector not written (read back '$back')" }
$misc = & $adb shell "$su -c 'dd if=/dev/block/by-name/misc bs=32 count=1 2>/dev/null'"
if ($misc -match 'boot-recovery') { throw 'misc already says boot-recovery: clear it first' }
& $adb shell "$su -c 'chmod 755 /data/local/tmp/reboot2; /data/local/tmp/reboot2 recovery'"
"command '$Cmd' written, rebooting to recovery"
