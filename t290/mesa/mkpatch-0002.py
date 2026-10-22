#!/usr/bin/env python3
"""Edits for t290/mesa/patches/0002-osmesa-freedreno-sweet.patch in a Mesa tree (argv[1]):
OSMesa on Darwin renders with Freedreno on the SweetGPU device (FD_SWEET_FD)
instead of softpipe; OSMESA_SOFTPIPE=1 keeps softpipe."""
import os
import sys

t = sys.argv[1]


def edit(path, old, new):
    p = os.path.join(t, path)
    s = open(p).read()
    assert s.count(old) == 1, (path, old)
    open(p, 'w').write(s.replace(old, new))


edit('src/gallium/targets/osmesa/osmesa_target.c', '''#include "sw/null/null_sw_winsys.h"
''', '''#include "sw/null/null_sw_winsys.h"

#ifdef T290_SWEET
/* SM-T290 iPadOS: Freedreno on the SweetGPU user client (t290/mesa) */
#include <stdio.h>
#include <stdlib.h>
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/u_math.h"
#include "util/u_inlines.h"
#include "freedreno/freedreno_screen.h"
#include "freedreno/drm/freedreno_drmif.h"

static const driOptionDescription t290_fd_options[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "freedreno/driinfo_freedreno.h"
};

static struct pipe_screen *
t290_freedreno_screen(void)
{
   static driOptionCache defaults, options;
   static struct pipe_screen_config config;
   driParseOptionInfo(&defaults, t290_fd_options, ARRAY_SIZE(t290_fd_options));
   driParseConfigFiles(&options, &defaults, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
   config.options = &options;
   config.options_info = &defaults;
   return fd_screen_create(FD_SWEET_FD, &config, NULL);
}
#endif
''')
edit('src/gallium/targets/osmesa/osmesa_target.c', '''   struct sw_winsys *winsys;
   struct pipe_screen *screen;
''', '''   struct sw_winsys *winsys;
   struct pipe_screen *screen;

#ifdef T290_SWEET
   if (!getenv("OSMESA_SOFTPIPE")) {
      screen = t290_freedreno_screen();
      if (screen)
         return debug_screen_wrap(screen);
      fprintf(stderr, "T290: Freedreno screen failed, falling back to softpipe\\n");
   }
#endif
''')
edit('src/gallium/targets/osmesa/meson.build', '''osmesa_link_with = []
osmesa_link_args = []
osmesa_link_deps = []
''', '''osmesa_link_with = []
osmesa_link_args = []
osmesa_link_deps = []
osmesa_c_args = []
osmesa_deps = []

# SM-T290 iPadOS: Freedreno on SweetGPU (t290/mesa)
if host_machine.system() == 'darwin' and with_gallium_freedreno
  osmesa_c_args += '-DT290_SWEET'
  osmesa_deps += driver_freedreno
  osmesa_link_args += ['-framework', 'IOKit', '-framework', 'CoreFoundation', '-framework', 'IOSurface']
endif
''')
edit('src/gallium/targets/osmesa/meson.build', '''  'osmesa_target.c',
  gnu_symbol_visibility : 'hidden',''', '''  'osmesa_target.c',
  c_args : osmesa_c_args,
  gnu_symbol_visibility : 'hidden',''')
edit('src/gallium/targets/osmesa/meson.build', '''    dep_ws2_32, dep_selinux, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
  ],''', '''    dep_ws2_32, dep_selinux, dep_thread, dep_clock, dep_unwind, driver_swrast, idep_mesautil,
    osmesa_deps,
  ],''')
print('ok 0002')
edit('src/gallium/targets/osmesa/meson.build', '''osmesa_c_args = []
osmesa_deps = []
''', '''osmesa_c_args = []
osmesa_deps = []
osmesa_incs = []
''')
edit('src/gallium/targets/osmesa/meson.build', '''  osmesa_c_args += '-DT290_SWEET'
''', '''  osmesa_c_args += '-DT290_SWEET'
  osmesa_incs += inc_freedreno
''')
edit('src/gallium/targets/osmesa/meson.build', '''    inc_gallium_drivers,
  ],''', '''    inc_gallium_drivers, osmesa_incs,
  ],''')
print('ok 0002 incs')
edit('src/gallium/targets/osmesa/osmesa_target.c', '''static const driOptionDescription t290_fd_options[] = {''', '''/* no KMS display on iOS: freedreno only calls these with a renderonly object */
#include "renderonly/renderonly.h"
struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
                                          struct winsys_handle *out_handle)
{
   return NULL;
}

void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
}

static const driOptionDescription t290_fd_options[] = {''')
print('ok 0002 renderonly')
