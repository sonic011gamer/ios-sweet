#!/bin/bash
# regenerate patches/0001 from the pristine Mesa tarball (WSL)
set -e
rm -rf /tmp/mp && mkdir -p /tmp/mp && cd /tmp/mp
tar xf /mnt/d/test1/dl/mesa/mesa.tar.xz
cp -a mesa-24.2.8 b && mv mesa-24.2.8 a
python3 /mnt/d/test1/ios-sweet/t290/mesa/mkpatch-0001.py b
diff -ruN a b > /mnt/d/test1/ios-sweet/t290/mesa/patches/0001-freedreno-sweet-backend.patch || true
wc -l /mnt/d/test1/ios-sweet/t290/mesa/patches/0001-freedreno-sweet-backend.patch
