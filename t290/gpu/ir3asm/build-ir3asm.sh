#!/bin/bash
# Build t290/gpu/ir3asm/ir3asm.cc inside an extracted Mesa 24.2.8 tree that
# was configured with -Dgallium-drivers=freedreno -Dtools=freedreno
# (build dir build-fd). Usage: build-ir3asm.sh ~/mesa-src/mesa-24.2.8
set -euo pipefail
MESA=$1
HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE/ir3asm.cc" "$MESA/src/freedreno/computerator/ir3asm.cc"
MB="$MESA/src/freedreno/computerator/meson.build"
if ! grep -q "'ir3asm'" "$MB"; then
	cat >> "$MB" <<'EOT'

# t290: assembler only, no DRM device (ios-sweet t290/gpu/ir3asm)
executable(
  'ir3asm',
  ['ir3asm.cc', freedreno_xml_header_files, ir3_parser[1]],
  include_directories : [inc_freedreno, inc_include, inc_src],
  link_with : [libfreedreno_drm, libfreedreno_ir3, libfreedreno_common],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  cpp_args : [computerator_cpp_args],
  build_by_default : with_tools.contains('freedreno'),
)
EOT
fi
ninja -C "$MESA/build-fd" src/freedreno/computerator/ir3asm
ls -l "$MESA/build-fd/src/freedreno/computerator/ir3asm"
