#!/usr/bin/env python3
"""Edits for t290/mesa/patches/0003-osmesa-iosurface-images.patch in a Mesa
tree (argv[1]): OSMesa hands out EGLImage-style handles for IOSurfaces so the
EAGL shim (t290/eagl) can bind them with glEGLImageTargetTexture2DOES:
OSMesaT290CreateImage imports the surface's pixels into the GPU (Freedreno
resource_from_handle, sweet backend handle 0x40000000 | IOSurface id)."""
import os
import sys

t = sys.argv[1]


def edit(path, old, new):
    p = os.path.join(t, path)
    s = open(p).read()
    assert s.count(old) == 1, (path, old)
    open(p, 'w').write(s.replace(old, new))


f = 'src/gallium/frontends/osmesa/osmesa.c'
edit(f, '''static struct pipe_frontend_screen *global_fscreen = NULL;
''', '''static struct pipe_frontend_screen *global_fscreen = NULL;

/*
 * SM-T290 iPadOS (t290/eagl): IOSurfaces as EGLImages. The handle is a
 * struct t290_image; glEGLImageTargetTexture2DOES binds it to a texture,
 * which CoreAnimation samples or renders into through a framebuffer.
 */
#define T290_IMAGE_MAGIC 0x54323930u
#define T290_IOSURFACE_HANDLE 0x40000000u    /* sweet backend: IOSurface id */

struct t290_image {
   uint32_t magic;
   struct pipe_resource *res;
   enum pipe_format format;
   unsigned internalformat;
};

static bool
t290_validate_egl_image(struct pipe_frontend_screen *fscreen, void *egl_image)
{
   struct t290_image *img = egl_image;
   return img && img->magic == T290_IMAGE_MAGIC && img->res;
}

static bool
t290_get_egl_image(struct pipe_frontend_screen *fscreen, void *egl_image,
                   struct st_egl_image *out)
{
   struct t290_image *img = egl_image;
   if (!t290_validate_egl_image(fscreen, egl_image))
      return false;
   memset(out, 0, sizeof(*out));
   pipe_resource_reference(&out->texture, img->res);
   out->format = img->format;
   out->internalformat = img->internalformat;
   return true;
}
''')
edit(f, '''      global_fscreen->get_egl_image = NULL;
''', '''      global_fscreen->get_egl_image = t290_get_egl_image;
      global_fscreen->validate_egl_image = t290_validate_egl_image;
''')
edit(f, '''GLAPI OSMesaContext GLAPIENTRY
OSMesaCreateContext(GLenum format, OSMesaContext sharelist)''', '''/*
 * IOSurface pixel format (fourcc) -> gallium format and GL internal format;
 * PIPE_FORMAT_NONE: not importable (the shim uploads through the CPU).
 */
static enum pipe_format
t290_format(uint32_t fourcc, unsigned *internalformat)
{
   switch (fourcc) {
   case 0x42475241: /* 'BGRA' */
      *internalformat = GL_RGBA8;
      return PIPE_FORMAT_B8G8R8A8_UNORM;
   case 0x52474241: /* 'RGBA' */
      *internalformat = GL_RGBA8;
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case 0x4c303038: /* 'L008' */
      *internalformat = GL_LUMINANCE8;
      return PIPE_FORMAT_L8_UNORM;
   case 0x41303038: /* 'A008' */
      *internalformat = GL_ALPHA8;
      return PIPE_FORMAT_A8_UNORM;
   case 0x4c413038: /* 'LA08' */
      *internalformat = GL_LUMINANCE8_ALPHA8;
      return PIPE_FORMAT_L8A8_UNORM;
   default:
      return PIPE_FORMAT_NONE;
   }
}

PUBLIC void *
OSMesaT290CreateImage(uint32_t surface_id, uint32_t width, uint32_t height,
                      uint32_t bytes_per_row, uint32_t offset, uint32_t fourcc)
{
   struct pipe_screen *screen = get_st_manager()->screen;
   unsigned internalformat = 0;
   enum pipe_format format = t290_format(fourcc, &internalformat);
   if (!screen || !screen->resource_from_handle || format == PIPE_FORMAT_NONE)
      return NULL;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = format;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED;
   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET))
      templ.bind |= PIPE_BIND_RENDER_TARGET;

   struct winsys_handle wh;
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_KMS;
   wh.handle = T290_IOSURFACE_HANDLE | surface_id;
   wh.stride = bytes_per_row;
   wh.offset = offset;
   wh.modifier = 0; /* DRM_FORMAT_MOD_LINEAR */

   struct pipe_resource *res =
      screen->resource_from_handle(screen, &templ, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   if (!res)
      return NULL;
   struct t290_image *img = CALLOC_STRUCT(t290_image);
   if (!img) {
      pipe_resource_reference(&res, NULL);
      return NULL;
   }
   img->magic = T290_IMAGE_MAGIC;
   img->res = res;
   img->format = format;
   img->internalformat = internalformat;
   return img;
}

PUBLIC void
OSMesaT290DestroyImage(void *image)
{
   struct t290_image *img = image;
   if (!img || img->magic != T290_IMAGE_MAGIC)
      return;
   img->magic = 0;
   pipe_resource_reference(&img->res, NULL);
   FREE(img);
}

GLAPI OSMesaContext GLAPIENTRY
OSMesaCreateContext(GLenum format, OSMesaContext sharelist)''')
edit(f, '''#include "frontend/api.h"
''', '''#include "frontend/api.h"
#include "frontend/winsys_handle.h"
''')
print('ok 0003')
