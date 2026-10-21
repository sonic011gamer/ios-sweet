#!/usr/bin/env python3
"""Generate t290/mesa/patches/0001-freedreno-sweet-backend.patch edits in a Mesa tree (argv[1])."""
import sys, os
t = sys.argv[1]
def edit(path, old, new):
    p = os.path.join(t, path); s = open(p).read()
    assert s.count(old) == 1, (path, old)
    open(p, 'w').write(s.replace(old, new))

edit('src/freedreno/drm/meson.build', '''libfreedreno_drm = static_library(''', '''# SM-T290 iPadOS: the SweetGPU IOKit user client (t290/mesa)
if host_machine.system() == 'darwin'
  libfreedreno_drm_files += files(
    'sweet/sweet_bo.c',
    'sweet/sweet_device.c',
    'sweet/sweet_pipe.c',
    'sweet/sweet_priv.h',
  )
  libfreedreno_drm_flags += '-DHAVE_FREEDRENO_SWEET'
endif

libfreedreno_drm = static_library(''')

edit('src/freedreno/drm/freedreno_device.c', '''#ifdef HAVE_FREEDRENO_VIRTIO
struct fd_device *virtio_device_new(int fd, drmVersionPtr version);
#endif''', '''#ifdef HAVE_FREEDRENO_VIRTIO
struct fd_device *virtio_device_new(int fd, drmVersionPtr version);
#endif
#ifdef HAVE_FREEDRENO_SWEET
struct fd_device *sweet_device_new(void);
#endif''')
edit('src/freedreno/drm/freedreno_device.c', '''   os_get_page_size(&os_page_size);

   /* figure out if we are kgsl or msm drm driver: */
   version = drmGetVersion(fd);''', '''   os_get_page_size(&os_page_size);

#ifdef HAVE_FREEDRENO_SWEET
   if (fd == FD_SWEET_FD) {
      dev = sweet_device_new();
      if (!dev)
         return NULL;
      goto common;
   }
#endif

   /* figure out if we are kgsl or msm drm driver: */
   version = drmGetVersion(fd);''')
edit('src/freedreno/drm/freedreno_device.c', '''   if (!dev)
      return NULL;

   fd_drm_perfetto_init();''', '''   if (!dev)
      return NULL;

#ifdef HAVE_FREEDRENO_SWEET
common:
#endif
   fd_drm_perfetto_init();''')
edit('src/freedreno/drm/freedreno_drmif.h', '''enum fd_param_id {''', '''/* fd_device_new(FD_SWEET_FD): the SweetGPU user client (SM-T290 iPadOS) */
#define FD_SWEET_FD (-1000)

enum fd_param_id {''')
edit('src/freedreno/common/freedreno_devices.py', '''add_gpus([
        GPUId(506),''', '''add_gpus([
        GPUId(504),     # SM-T290 (SDM429)
        GPUId(506),''')
print('ok')
edit('src/freedreno/meson.build', '''if system_has_kms_drm
  subdir('drm')
endif''', '''if system_has_kms_drm or host_machine.system() == 'darwin'
  subdir('drm')
endif''')
print('ok darwin drm')
