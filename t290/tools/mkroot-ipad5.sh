#!/bin/bash
# Build the iPadOS 16.7.16 (20H392) root image for the t290 port from the
# iPad 5 IPSW: extract OS + SystemOS/AppOS cryptexes, tools/mkiosroot.sh,
# then tools/sweet-root-tweaks.py. Run as root in WSL/Linux (apfs-fuse,
# hfsprogs, rsync, unzip, python3).
#   sudo t290/tools/mkroot-ipad5.sh IPSW WORKDIR [SIZE]
# Output: WORKDIR/ipad5-root.img (pristine copy: WORKDIR/ipad5-root.pristine.img).
set -euo pipefail
IPSW=$(realpath "$1") WORK=$2 SIZE=${3:-16G}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
# BuildManifest (iPad6,11 / j71sap, Erase): OS, Cryptex1,SystemOS, Cryptex1,AppOS
OS=087-86634-044.dmg SYSOS=087-86624-047.dmg APPOS=087-86715-047.dmg
mkdir -p "$WORK" && cd "$WORK"
for f in $OS $SYSOS $APPOS; do
	[[ -s $f ]] || python3 - "$IPSW" "$f" <<'PY'
import sys, zipfile, shutil
z = zipfile.ZipFile(sys.argv[1])
with z.open(sys.argv[2]) as src, open(sys.argv[2] + '.part', 'wb') as dst:
    shutil.copyfileobj(src, dst, 1 << 24)
PY
	[[ -s $f ]] || mv "$f.part" "$f"
	ls -l "$f"
done
rm -f ipad5-root.img
t0=$(date +%s)
bash "$REPO/tools/mkiosroot.sh" "$OS" "$SYSOS" "$APPOS" ipad5-root.img "$SIZE"
fsck.hfsplus -fy ipad5-root.img || true
mkdir -p mnt
mount -t hfsplus -o loop,force ipad5-root.img mnt
python3 "$REPO/tools/sweet-root-tweaks.py" "$WORK/mnt"
sync
umount mnt
fsck.hfsplus -fn ipad5-root.img
cp --sparse=always ipad5-root.img ipad5-root.pristine.img
echo "root built in $(( $(date +%s) - t0 )) s"
du -h --apparent-size ipad5-root.img; du -h ipad5-root.img
