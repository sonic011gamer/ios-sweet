# Files between this PC (through Android) and the iPadOS root on the microSD
# (xnu bsd/dev/sweet_t290_xfer.c, patch 0125). Android cannot mount the HFS+
# root, so the request goes into raw sectors past the 16 GiB root volume and
# the iPadOS kernel carries it out at its next boot.
#   t290-xfer.ps1 -Put 'C:\x\gputest=/usr/bin/gputest:755' -Get '/System/Library/xpc/launchd.plist=C:\x\launchd.plist'
#     (several -Put/-Get allowed, comma-separated) -> boot iPadOS, come back to Android
#   t290-xfer.ps1 -Result      -> per-op results, fetched files written locally
param([string[]]$Put = @(), [string[]]$Get = @(), [switch]$Result)
$ErrorActionPreference = 'Stop'
$adb = 'D:\test1\tools-win\platform-tools\adb.exe'
$su = '/debug_ramdisk/su'
$XferLba = 33558528
$DataLba = $XferLba + 64
$Manifest = Join-Path $env:TEMP 't290-xfer.json'

function Sh([string]$cmd) { & $adb shell "$su -c '$cmd'" }

if ($Result) {
	$tmp = Join-Path $env:TEMP 't290-xfer-hdr.bin'
	Sh "dd if=/dev/block/mmcblk1 of=/data/local/tmp/xfer-hdr.bin bs=512 skip=$XferLba count=33" | Out-Null
	& $adb pull /data/local/tmp/xfer-hdr.bin $tmp | Out-Null
	$h = [IO.File]::ReadAllBytes($tmp)
	if ([Text.Encoding]::ASCII.GetString($h, 0, 8) -ne 'T290XFR1') { throw 'no request on the card' }
	$nops = [BitConverter]::ToUInt32($h, 12); $state = [BitConverter]::ToUInt32($h, 16)
	"request: $nops ops, state $state ($(if ($state -eq 1) {'done'} else {'NOT done yet'})), ok $([BitConverter]::ToUInt32($h, 20))"
	$m = if (Test-Path $Manifest) { Get-Content $Manifest | ConvertFrom-Json } else { @() }
	for ($i = 0; $i -lt $nops; $i++) {
		$o = 512 * (1 + $i)
		$op = [BitConverter]::ToUInt32($h, $o); $lba = [BitConverter]::ToUInt64($h, $o + 16)
		$res = [BitConverter]::ToInt64($h, $o + 24)
		$path = [Text.Encoding]::UTF8.GetString($h, $o + 64, 448).TrimEnd([char]0)
		"  $(if ($op -eq 1) {'put'} else {'get'}) $path -> $res"
		if ($op -eq 2 -and $res -gt 0 -and $state -eq 1) {
			$local = ($m | Where-Object { $_.remote -eq $path }).local
			if (-not $local) { $local = Join-Path $env:TEMP ([IO.Path]::GetFileName($path)) }
			$n = [math]::Ceiling($res / 512)
			Sh "dd if=/dev/block/mmcblk1 of=/data/local/tmp/xfer-get.bin bs=512 skip=$lba count=$n" | Out-Null
			& $adb pull /data/local/tmp/xfer-get.bin "$local.part" | Out-Null
			$bytes = [IO.File]::ReadAllBytes("$local.part")
			[IO.File]::WriteAllBytes($local, $bytes[0..($res - 1)])
			Remove-Item "$local.part"
			"     saved $local ($res bytes)"
		}
	}
	return
}

$ops = @(); $blobs = @(); $lba = $DataLba; $man = @()
foreach ($p in $Put) {
	$src, $rest = $p -split '=', 2
	$dst, $mode = $rest -split ':(?=[0-7]+$)', 2
	if (-not $mode) { $mode = '644' }
	$data = [IO.File]::ReadAllBytes($src)
	$ops += @{ op = 1; mode = [Convert]::ToUInt32($mode, 8); size = [uint64]$data.Length; lba = [uint64]$lba; path = $dst }
	$blobs += @{ lba = $lba; data = $data }
	$lba += [math]::Ceiling($data.Length / 512) + 8
}
foreach ($g in $Get) {
	$remote, $local = $g -split '=', 2
	$cap = 16MB
	$ops += @{ op = 2; mode = 0; size = [uint64]$cap; lba = [uint64]$lba; path = $remote }
	$man += @{ remote = $remote; local = $local }
	$lba += $cap / 512 + 8
}
if ($ops.Count -eq 0 -or $ops.Count -gt 32) { throw '1..32 operations' }
$total = ($lba - $XferLba) * 512
if ($blobs.Count -eq 0) { $total = (1 + $ops.Count) * 512 }
$img = New-Object byte[] ([int](($blobs | ForEach-Object { ($_.lba - $XferLba) * 512 + $_.data.Length } | Measure-Object -Maximum).Maximum + 512))
if ($img.Length -lt (1 + $ops.Count) * 512) { $img = New-Object byte[] ((1 + $ops.Count) * 512) }
[Text.Encoding]::ASCII.GetBytes('T290XFR1').CopyTo($img, 0)
[BitConverter]::GetBytes([uint32]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() -band 0xffffffff)).CopyTo($img, 8)
[BitConverter]::GetBytes([uint32]$ops.Count).CopyTo($img, 12)
for ($i = 0; $i -lt $ops.Count; $i++) {
	$o = 512 * (1 + $i); $x = $ops[$i]
	[BitConverter]::GetBytes([uint32]$x.op).CopyTo($img, $o)
	[BitConverter]::GetBytes([uint32]$x.mode).CopyTo($img, $o + 4)
	[BitConverter]::GetBytes([uint64]$x.size).CopyTo($img, $o + 8)
	[BitConverter]::GetBytes([uint64]$x.lba).CopyTo($img, $o + 16)
	[Text.Encoding]::UTF8.GetBytes($x.path).CopyTo($img, $o + 64)
}
foreach ($b in $blobs) { $b.data.CopyTo($img, ($b.lba - $XferLba) * 512) }
$f = Join-Path $env:TEMP 't290-xfer.bin'
[IO.File]::WriteAllBytes($f, $img)
ConvertTo-Json -InputObject @($man) | Out-File -Encoding utf8 $Manifest
& $adb push $f /data/local/tmp/xfer.bin | Out-Null
Sh "dd if=/data/local/tmp/xfer.bin of=/dev/block/mmcblk1 bs=512 seek=$XferLba conv=notrunc && sync" | Out-Null
$back = Sh "dd if=/dev/block/mmcblk1 bs=512 skip=$XferLba count=1 2>/dev/null | head -c 8"
if ($back -ne 'T290XFR1') { throw "request not written (read back '$back')" }
"request written: $($ops.Count) ops ($($Put.Count) put, $($Get.Count) get); boot iPadOS, then -Result"
