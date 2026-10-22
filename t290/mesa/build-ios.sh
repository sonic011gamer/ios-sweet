#!/bin/bash
# Mesa Freedreno (gallium, a5xx) for iPadOS 16 arm64, for the SM-T290 port.
#   t290/mesa/build-ios.sh [MESA_TARBALL]   (macOS with Xcode; CI: .github/workflows/mesa-ios.yml)
#   TARGET=mac: the same library for this Mac (debug info), to run glprobe
#   with SWEET_FAKE=1 (fake SweetGPU) and debug Mesa's CPU side without the tablet
# Mesa 24.2.8 + t290/mesa/patches/*.patch + t290/mesa/overlay/ (new files),
# libdrm replaced by t290/mesa/stub (Darwin has no DRM).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ver=24.2.8
tarball=${1:-}
target=${TARGET:-ios}
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
if [ "$target" = mac ]; then
	cfg=(--native-file "$here/native-mac.txt" -Dbuildtype=debugoptimized -Db_ndebug=false)
else
	cfg=(--cross-file "$here/cross-ios.txt" -Dbuildtype=release -Db_ndebug=true)
fi
meson setup build "${cfg[@]}" \
	-Dc_args="-I$stub -I$PWD/include" -Dcpp_args="-I$stub -I$PWD/include" \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dllvm=disabled -Dvulkan-drivers= -Dgallium-drivers=softpipe,freedreno \
	-Dosmesa=true -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled \
	-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dbuild-tests=false -Dtools=
ninja -C build src/compiler/spirv/spirv_info.h src/compiler/spirv/vtn_generator_ids.h > /dev/null 2>&1 || true
ninja -C build src/gallium/targets/osmesa/libOSMesa.8.dylib > "$work/build.log" 2>&1 || {
	tail -100 "$work/build.log"
	exit 1
}

# OSMesa dylib + the GL test, ad-hoc signed, for /usr/lib on the tablet
out=$work/out
mkdir -p "$out"
lib=$(ls build/src/gallium/targets/osmesa/libOSMesa*.dylib | head -1)
glapi=$(ls build/src/mapi/shared-glapi/libglapi*.dylib | head -1)
cp -L "$glapi" "$out/libglapi.0.dylib"
cp -L "$lib" "$out/libOSMesa.8.dylib"
install_name_tool -id /usr/lib/libglapi.0.dylib "$out/libglapi.0.dylib"
# Meson embeds an absolute build path on Darwin.  Rewrite the dependency
# before signing so every artifact can run outside the runner checkout.
dep=$(otool -L "$out/libOSMesa.8.dylib" | awk '/libglapi.*dylib/{print $1; exit}')
install_name_tool -change "$dep" /usr/lib/libglapi.0.dylib "$out/libOSMesa.8.dylib"
if [ "$target" = mac ]; then
	install_name_tool -change /usr/lib/libglapi.0.dylib @loader_path/libglapi.0.dylib "$out/libOSMesa.8.dylib"
	codesign -s - -f "$out/libglapi.0.dylib" "$out/libOSMesa.8.dylib"
	clang -O1 -g -Wall -o "$out/glprobe" "$here/../ios/glprobe.c"
	ls -la "$out"
	exit 0
fi
install_name_tool -id /usr/lib/libOSMesa.8.dylib "$out/libOSMesa.8.dylib"
codesign -s - -f "$out/libglapi.0.dylib" "$out/libOSMesa.8.dylib"
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall \
	-Iinclude -o "$out/gltri" "$here/test/gltri.c" "$out/libOSMesa.8.dylib"
codesign -s - -f --entitlements "$here/../ios/ent.plist" "$out/gltri"
otool -L "$out/gltri" "$out/libOSMesa.8.dylib"
ls -la "$out"
