#!/usr/bin/env python3
"""Expose real GLES contexts through the OSMesa frontend for EAGL."""
import pathlib
import sys

p = pathlib.Path(sys.argv[1]) / 'src/gallium/frontends/osmesa/osmesa.c'
s = p.read_text()

def edit(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)

edit('static struct pipe_frontend_screen *global_fscreen = NULL;', '''
/* Private profile used only by OSMesaT290CreateContext. */
#define OSMESA_T290_ES_PROFILE 0x40
PUBLIC OSMesaContext OSMesaT290CreateContext(unsigned api, OSMesaContext shared);

static struct pipe_frontend_screen *global_fscreen = NULL;''')
edit('profile != OSMESA_COMPAT_PROFILE)', '''profile != OSMESA_COMPAT_PROFILE &&
             profile != OSMESA_T290_ES_PROFILE)''')
edit('''attribs.profile = (profile == OSMESA_CORE_PROFILE)
      ? API_OPENGL_CORE : API_OPENGL_COMPAT;''', '''attribs.profile = profile == OSMESA_T290_ES_PROFILE ? API_OPENGLES2 :
      (profile == OSMESA_CORE_PROFILE ? API_OPENGL_CORE : API_OPENGL_COMPAT);''')
edit('''GLAPI void GLAPIENTRY
OSMesaDestroyContext(OSMesaContext osmesa)''', '''PUBLIC OSMesaContext
OSMesaT290CreateContext(unsigned api, OSMesaContext shared)
{
   if (api != 2 && api != 3)
      return NULL;
   const int attrs[] = {
      OSMESA_PROFILE, OSMESA_T290_ES_PROFILE,
      OSMESA_CONTEXT_MAJOR_VERSION, api,
      OSMESA_CONTEXT_MINOR_VERSION, 0,
      OSMESA_FORMAT, OSMESA_RGBA, 0
   };
   return OSMesaCreateContextAttribs(attrs, shared);
}

GLAPI void GLAPIENTRY
OSMesaDestroyContext(OSMesaContext osmesa)''')
p.write_text(s)
print('ok 0004')
