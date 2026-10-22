#!/usr/bin/env bash
# Build the tablet bridge in Linux/WSL; does not install or flash anything.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
toolroot=${T290_TOOLCHAIN:-$HOME/t290-toolchain}
sdk=${T290_IOS_SDK:-$toolroot/iPhoneOS16.5.sdk}
layout=${T290_GLI_LAYOUT:-$toolroot/gli-layout.txt}
gl_headers=${T290_GL_HEADERS:-$HOME/mesa-src/mesa-24.2.8/include/GL}
out=${1:-$toolroot/build-eagl}
cc=${CC:-clang}
ld=${LD64:-ld64.lld-14}
ldid=${LDID:-$toolroot/ldid-src/ldid}
for tool in "$cc" "$ld" "$ldid" python3; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
test -f "$sdk/SDKSettings.plist"
test -f "$layout"
test -f "$gl_headers/gl.h"
mkdir -p "$out/source"
out=$(cd "$out" && pwd)
cp "$here/eaglshim.m" "$here/eagltest.m" "$here/gl_types.h" "$here/surface.h" "$out/source/"
python3 "$here/gen_dispatch.py" "$sdk" "$out/source/dispatch_generated.h" \
    --layout "$layout" --gl-headers "$gl_headers"
flags=(-target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -fobjc-arc)
link=(-arch arm64 -platform_version ios 16.0 16.5 -syslibroot "$sdk")
"$cc" "${flags[@]}" -c "$out/source/eaglshim.m" -o "$out/eaglshim.o"
"$ld" "${link[@]}" -dylib -install_name /usr/lib/t290eagl.dylib \
    -o "$out/t290eagl.dylib" "$out/eaglshim.o" \
    -framework Foundation -framework IOKit -framework IOSurface -lSystem -lobjc
"$cc" "${flags[@]}" -c "$out/source/eagltest.m" -o "$out/eagltest.o"
"$ld" "${link[@]}" -o "$out/eagltest" "$out/eagltest.o" \
    -framework Foundation -framework OpenGLES -framework IOSurface -lSystem -lobjc
"$ldid" -S -Cadhoc -Hsha256 "$out/t290eagl.dylib"
"$ldid" -S"$here/../ios/ent.plist" -Cadhoc -Hsha256 "$out/eagltest"
python3 "$here/verify-local.py" "$out" "$here/../ios/ent.plist"
(cd "$out" && sha256sum t290eagl.dylib eagltest > SHA256SUMS)
{
    "$cc" --version
    "$ld" --version
    sha256sum "$layout" "$out/source/dispatch_generated.h"
    sha256sum "$here/eaglshim.m" "$here/eagltest.m" "$here/gen_dispatch.py"
} > "$out/build-info.txt"
echo "Local ARM64 iOS build complete: $out (${SECONDS}s)"
