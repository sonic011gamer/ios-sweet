#!/bin/bash
# Mesa Freedreno (gallium, a5xx) for iPadOS 16 arm64, for the SM-T290 port.
#   t290/mesa/build-ios.sh [MESA_TARBALL]   (macOS with Xcode; CI: .github/workflows/mesa-ios.yml)
# Mesa 24.2.8 + t290/mesa/patches/*.patch + t290/mesa/overlay/ (new files),
# libdrm replaced by t290/mesa/stub (Darwin has no DRM).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ver=24.2.8
tarball=${1:-}
work=${WORK:-$PWD/mesa-work}
mkdir -p "$work"
cd "$work"
if [ -z "$tarball" ]; then
	tarball=$work/mesa-$ver.tar.xz
	[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://archive.mesa3d.org/mesa-$ver.tar.xz"
fi
rm -rf "mesa-$ver"
tar xf "$tarball"
cd "mesa-$ver"
for p in "$here"/patches/*.patch; do
	[ -f "$p" ] || continue
	echo "== $(basename "$p")"
	patch -p1 < "$p"
done
if [ -d "$here/overlay" ]; then
	cp -R "$here/overlay/." .
fi
stub="$here/stub"
meson setup build --cross-file "$here/cross-ios.txt" \
	-Dbuildtype=release -Db_ndebug=true \
	-Dc_args="-I$stub -I$PWD/include" -Dcpp_args="-I$stub -I$PWD/include" \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dllvm=disabled -Dvulkan-drivers= -Dgallium-drivers=softpipe,freedreno \
	-Dosmesa=true -Dshared-glapi=disabled -Dgles1=disabled -Dgles2=disabled \
	-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dbuild-tests=false -Dtools=
ninja -C build src/compiler/spirv/spirv_info.h src/compiler/spirv/vtn_generator_ids.h > /dev/null 2>&1 || true
ninja -C build -k 0 src/gallium/targets/osmesa/libOSMesa.8.dylib 2>&1 | tee "$work/build.log" | grep -E "error|FAILED" | head -200 || true
grep -c "FAILED" "$work/build.log" || true

# OSMesa dylib + the GL test, ad-hoc signed, for /usr/lib/t290 on the tablet
out=$work/out
mkdir -p "$out"
lib=$(ls build/src/gallium/targets/osmesa/libOSMesa*.dylib | head -1)
cp -L "$lib" "$out/libOSMesa.8.dylib"
install_name_tool -id /usr/lib/t290/libOSMesa.8.dylib "$out/libOSMesa.8.dylib"
codesign -s - -f "$out/libOSMesa.8.dylib"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall \
	-Iinclude -o "$out/gltri" "$here/test/gltri.c" "$out/libOSMesa.8.dylib"
codesign -s - -f --entitlements "$here/../ios/ent.plist" "$out/gltri"
otool -L "$out/gltri" "$out/libOSMesa.8.dylib"
ls -la "$out"
