#!/bin/bash
# Build the iPadOS 16.7.16 (20H392) root image for the t290 port from the
# iPad 5 IPSW, on a Linux without the hfsplus driver (WSL2):
#   1. extract OS + SystemOS/AppOS cryptexes from the IPSW
#   2. stage the root in a plain directory: the tools/mkiosroot.sh layout
#      (cryptexes, dyld cache links, logd stores, system keybag), then
#      tools/sweet-root-tweaks.py
#   3. tar it, make an empty journaled case-sensitive HFS+ image
#   4. a throwaway KVM guest (Ubuntu kernel + hfsplus.ko + busybox) untars
#      the stage into the HFS+ image
#   5. fsck.hfsplus, keep a pristine copy
# RESUME=1 reuses WORKDIR/stage.tar. Run as root (apfs-fuse, hfsprogs, rsync, python3, qemu-system-x86_64, /dev/kvm).
#   t290/tools/mkroot-ipad5.sh IPSW WORKDIR VMKIT [SIZE]
# VMKIT = extracted debs: boot/vmlinuz-*, lib/modules/*/.../{virtio_blk,hfsplus}.ko, bin/busybox
set -euo pipefail
IPSW=$(realpath "$1") WORK=$2 KIT=$3 SIZE=${4:-16G}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
# BuildManifest (iPad6,11 / j71sap, Erase): OS, Cryptex1,SystemOS, Cryptex1,AppOS
OS=087-86634-044.dmg SYSOS=087-86624-047.dmg APPOS=087-86715-047.dmg
mkdir -p "$WORK" && cd "$WORK"
WORK=$(pwd)
t0=$(date +%s)
step() { echo "== [$(( $(date +%s) - t0 ))s] $*"; }

if [[ ${RESUME:-0} == 1 && -s $WORK/stage.tar ]]; then
	step "resume: reusing $WORK/stage.tar"
else
step "extract disk images"
for f in $OS $SYSOS $APPOS; do
	[[ -s $f ]] || python3 - "$IPSW" "$f" <<'PY'
import sys, zipfile, shutil
z = zipfile.ZipFile(sys.argv[1])
with z.open(sys.argv[2]) as src, open(sys.argv[2] + '.part', 'wb') as dst:
    shutil.copyfileobj(src, dst, 1 << 24)
PY
	[[ -s $f ]] || mv "$f.part" "$f"
done

step "stage the root"
mounts=()
cleanup() { for m in "${mounts[@]}"; do mountpoint -q "$m" && umount "$m"; done; true; }
trap cleanup EXIT
apfs_mount() {	# apfs-fuse parses UDIF only for *.dmg names; ',' in paths breaks its options
	ln -sf "$WORK/$1" "$WORK/$2.dmg"
	mkdir -p "$WORK/$2"
	mountpoint -q "$WORK/$2" || apfs-fuse "$WORK/$2.dmg" "$WORK/$2"
	mounts+=("$WORK/$2")
}
apfs_mount $OS os
apfs_mount $SYSOS sysos
apfs_mount $APPOS appos
S=$WORK/stage
rm -rf "$S" && mkdir "$S"
rsync -aH --numeric-ids "$WORK/os/root/" "$S/"
mkdir -p "$S/private/preboot/Cryptexes/OS" "$S/private/preboot/Cryptexes/App"
rsync -aH --numeric-ids "$WORK/sysos/root/" "$S/private/preboot/Cryptexes/OS/"
rsync -aH --numeric-ids "$WORK/appos/root/" "$S/private/preboot/Cryptexes/App/"
cleanup; mounts=()

# Shared cache where dyld looks at ignition level 1 (hard links).
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$S/System/Library/Caches/com.apple.dyld"
for f in "$S/$dsc"/*; do ln -f "$f" "$S/System/Library/Caches/com.apple.dyld/${f##*/}"; done
# Cryptex files without a counterpart in the OS root, linked to their root path.
n=0
for c in OS App; do
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e "$S/$f" && ! -L "$S/$f" ]]; then
			mkdir -p "$S/$(dirname "$f")"
			src="$S/private/preboot/Cryptexes/$c/$f"
			# Linux hfsplus cannot hard-link symlinks: copy those.
			if [[ -L $src ]]; then cp -P "$src" "$S/$f"; else ln "$src" "$S/$f"; fi
			n=$((n + 1))
		fi
	done < <(cd "$S/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*')
done
echo "linked $n cryptex files into the root"
# logd's stores (owner _logd 272, .migrated markers).
db=$S/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"; done
# Plain-plist system keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$S/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	install -m 0600 -o 0 -g 0 /dev/stdin "$S/private/var/keybags/systembag.kb"
python3 "$REPO/tools/sweet-root-tweaks.py" "$S"
du -sh "$S"

step "tar the stage"
tar -cf "$WORK/stage.tar" --format=gnu --numeric-owner -C "$S" .
ls -l "$WORK/stage.tar"
fi

step "empty HFS+ image ($SIZE)"
rm -f ipad5-root.img
truncate -s "$SIZE" ipad5-root.img
mkfs.hfsplus -s -J -v iOSRoot ipad5-root.img

step "untar into HFS+ in a KVM guest"
I=$WORK/initrd; rm -rf "$I"; mkdir -p "$I/bin" "$I/m"
cp "$KIT/bin/busybox" "$I/bin/busybox"
cp "$(find "$KIT/lib/modules" -name 'virtio_blk.ko*' | head -1)" "$I/m/virtio_blk.ko"
cp "$(find "$KIT/lib/modules" -name 'hfsplus.ko*' | head -1)" "$I/m/hfsplus.ko"
cp "$(find "$KIT/lib/modules" -name 'nls_utf8.ko*' | head -1)" "$I/m/nls_utf8.ko"
cat > "$I/init" <<'INIT'
#!/bin/busybox sh
B=/bin/busybox
$B mkdir -p /proc /sys /dev /t
$B mount -t proc proc /proc
$B mount -t sysfs sys /sys
$B mount -t devtmpfs dev /dev
$B insmod /m/virtio_blk.ko
$B insmod /m/nls_utf8.ko
$B insmod /m/hfsplus.ko
$B sleep 1
if $B mount -t hfsplus -o force /dev/vda /t; then
	cd /t && $B tar -xf /dev/vdb && echo "VMSTEP TAR_OK" || echo "VMSTEP TAR_FAIL"
	cd /
	$B sync
	$B df -h /t
	$B umount /t && echo "VMSTEP UMOUNT_OK"
else
	echo "VMSTEP MOUNT_FAIL"
fi
$B poweroff -f
INIT
chmod +x "$I/init"
(cd "$I" && find . | cpio -o -H newc --quiet | gzip -1 > "$WORK/initrd.gz")
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 2 -nographic -no-reboot \
	-kernel "$(ls "$KIT"/boot/vmlinuz-* | head -1)" -initrd "$WORK/initrd.gz" \
	-append "console=ttyS0 panic=-1 quiet" \
	-drive file="$WORK/ipad5-root.img",if=virtio,format=raw,cache=unsafe \
	-drive file="$WORK/stage.tar",if=virtio,format=raw,readonly=on | tee "$WORK/vm.log" | grep -a VMSTEP
if ! grep -q "VMSTEP TAR_OK" "$WORK/vm.log" || ! grep -q "VMSTEP UMOUNT_OK" "$WORK/vm.log"; then
	echo "KVM guest did not fill the image (see $WORK/vm.log)" >&2
	exit 1
fi

step "fsck"
# Linux hfsplus decomposes some non-ASCII names differently from Apple's tables.
fsck.hfsplus -f -y ipad5-root.img | grep -v 'Illegal name' || true
fsck.hfsplus -f -n ipad5-root.img
cp --sparse=always ipad5-root.img ipad5-root.pristine.img
step "done"
du -h --apparent-size ipad5-root.img; du -h ipad5-root.img
