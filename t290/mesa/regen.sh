#!/bin/bash
# regenerate t290/mesa/patches/000N from the pristine Mesa tarball (WSL), each
# patch on top of the previous ones
set -e
P=/mnt/d/test1/ios-sweet/t290/mesa
rm -rf /tmp/mp && mkdir -p /tmp/mp && cd /tmp/mp
tar xf /mnt/d/test1/dl/mesa/mesa.tar.xz
mv mesa-24.2.8 base
for n in 0001 0002 0003 0004; do
	name=$(ls "$P"/mkpatch-$n.py)
	rm -rf a b && cp -a base a && cp -a base b
	python3 "$name" b
	out=$(ls "$P"/patches/$n-*.patch 2>/dev/null || true)
	[ -n "$out" ] || out="$P/patches/$n-new.patch"
	diff -ruN a b > "$out" || true
	echo "$out: $(wc -l < "$out") lines"
	rm -rf base && mv b base
done
