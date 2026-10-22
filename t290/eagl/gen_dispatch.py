#!/usr/bin/env python3
"""Generate context-first GLI adapters from the SDK, without copying headers.

QuartzCore's macro context is {renderer, inline GLIFunctionDispatch}.  Mesa
entry points have no renderer argument.  Each typed adapter removes it and
selects the correct Mesa context, preserving integer/float/stack arguments.
"""
import argparse
import pathlib
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('sdk', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
parser.add_argument('--layout', type=pathlib.Path,
                    help='cached gli_layout.py output, for builds without the macOS SDK')
parser.add_argument('--gl-headers', type=pathlib.Path, action='append', default=[],
                    help='additional directory of GL declarations (for example Mesa include/GL)')
options = parser.parse_args()
sdk = options.sdk
headers = sdk / 'System/Library/Frameworks/OpenGL.framework/Headers'
if options.layout:
    members = []
    for index, line in enumerate(options.layout.read_text().splitlines()):
        entry, ret, params = line.split(' | ', 2)
        slot, name = entry.split(' ', 1)
        if int(slot) != index:
            raise ValueError(f'non-contiguous GLI layout at {entry}')
        members.append(f'{ret} (*{name})({params})')
    # The cached ABI must be the same 975-slot layout used on the tablet.
    if len(members) != 975 or '(*finish)' not in members[89]:
        raise ValueError('unexpected GLI layout')
    header_dirs = [sdk / 'System/Library/Frameworks/OpenGLES.framework/Headers']
else:
    s = (headers / 'gliDispatch.h').read_text()
    s = re.sub(r'/\*.*?\*/|//[^\n]*', '', s, flags=re.S)
    body = re.search(r'__GLIFunctionDispatchRec\s*\{(.*?)\}\s*GLIFunctionDispatch', s, re.S)[1]
    members = [x.strip() for x in body.split(';') if x.strip()]
    header_dirs = [headers]
header_dirs += options.gl_headers
names = set(re.findall(r'\bgl[A-Z]\w+(?=\s*\()', '\n'.join(
    p.read_text() for directory in header_dirs for p in directory.rglob('*.h'))))
normal = {n[2:].lower(): n for n in sorted(names)}
aliases = {
    'create_shader_object_ARB': 'glCreateShader',
    'create_program_object_ARB': 'glCreateProgram',
    'attach_object_ARB': 'glAttachShader',
    'detach_object_ARB': 'glDetachShader',
    'use_program_object_ARB': 'glUseProgram',
    'get_attached_objects_ARB': 'glGetAttachedShaders',
}
special = {
    'delete_object_ARB': 't290_delete_object',
    'get_object_parameteriv_ARB': 't290_object_iv',
    'get_object_parameterfv_ARB': 't290_object_fv',
    'get_info_log_ARB': 't290_info_log',
    'get_string': 't290_get_string',
    'get_error': 't290_get_error',
    'finish': 't290_finish',
    'flush': 't290_flush',
    'flush_render_APPLE': 't290_flush',
    'finish_object_APPLE': 't290_finish_object',
    'bind_texture': 't290_bind_texture',
    'delete_textures': 't290_delete_textures',
    'delete_renderbuffers_EXT': 't290_delete_renderbuffers',
    'bind_framebuffer_EXT': 't290_bind_framebuffer',
    'framebuffer_texture2D_EXT': 't290_framebuffer_texture',
    'framebuffer_renderbuffer_EXT': 't290_framebuffer_renderbuffer',
    'delete_framebuffers_EXT': 't290_delete_framebuffers',
    'active_texture': 't290_active_texture',
    'enable': 't290_enable',
    'disable': 't290_disable',
    'scissor': 't290_scissor',
    'viewport': 't290_viewport',
    'tex_image2D': 't290_tex_image2d',
    'compressed_tex_image2D': 't290_compressed_tex_image2d',
    'generate_mipmap_EXT': 't290_generate_mipmap',
}
# GL calls writing the draw framebuffer -> rows they can touch (eaglshim.m)
draw_calls = {
    'draw_arrays': 'DRAW_PRIMITIVES', 'draw_elements': 'DRAW_PRIMITIVES',
    'draw_range_elements': 'DRAW_PRIMITIVES', 'draw_arrays_instanced': 'DRAW_PRIMITIVES',
    'draw_elements_instanced': 'DRAW_PRIMITIVES', 'clear': 'DRAW_CLEAR',
    'draw_elements_base_vertex': 'DRAW_PRIMITIVES',
    'draw_range_elements_base_vertex': 'DRAW_PRIMITIVES',
    'draw_elements_instanced_base_vertex': 'DRAW_PRIMITIVES',
    'multi_draw_arrays': 'DRAW_PRIMITIVES', 'multi_draw_elements': 'DRAW_PRIMITIVES',
    'multi_draw_elements_base_vertex': 'DRAW_PRIMITIVES',
    'draw_arrays_indirect': 'DRAW_PRIMITIVES', 'draw_elements_indirect': 'DRAW_PRIMITIVES',
    'clear_bufferiv': 'DRAW_CLEAR', 'clear_bufferuiv': 'DRAW_CLEAR',
    'clear_bufferfv': 'DRAW_CLEAR', 'clear_bufferfi': 'DRAW_CLEAR',
    'blit_framebuffer_EXT': 'DRAW_ALL_ROWS',
}
texture_writes = {'copy_tex_sub_image2D', 'tex_sub_image2D', 'compressed_tex_sub_image2D'}
texture_definitions = {'tex_image2D', 'copy_tex_image2D', 'compressed_tex_image2D', 'tex_storage2D'}
check_calls = set(draw_calls) | texture_writes | texture_definitions | {
    'pixel_storei', 'use_program_object_ARB', 'compile_shader_ARB', 'link_program_ARB',
    'framebuffer_texture2D_EXT', 'framebuffer_renderbuffer_EXT', 'bind_framebuffer_EXT',
    'tex_parameteri', 'enable', 'disable', 'blend_func', 'blend_func_separate',
    'vertex_attrib_pointer', 'enable_vertex_attrib_array',
}
rows = []
out = ['/* Generated by gen_dispatch.py; SDK declarations stay on the runner. */']
for i, member in enumerate(members):
    m = re.fullmatch(r'(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)', ' '.join(member.split()))
    if not m:
        raise ValueError(member)
    ret, name, args = m.groups()
    ret = ret.strip()
    params = args.split(',')[1:]
    args = ','.join(params).strip() or 'void'
    argv = [re.search(r'(\w+)\s*$', p.strip())[1] for p in params]
    key = re.sub(r'_(?:ARB|EXT|APPLE|OES|NV|ATI)$', '', name).replace('_', '').lower()
    candidates = [aliases.get(name), normal.get(key), normal.get(name.replace('_', '').lower())]
    candidates = list(dict.fromkeys(x for x in candidates if x))
    out.append(f'static {ret} gli_{i}(GLIContext ctx' + (', ' + args if params else '') + ') {')
    out.append('  T290_GL_TIMER;')
    out.append('  t290_make_current(ctx);')
    if name in draw_calls:
        out.append(f'  t290_before_draw({draw_calls[name]});')
    if name in texture_writes | texture_definitions:
        out.append(f'  t290_texture_write({"YES" if name in texture_definitions else "NO"});')
    call = special.get(name)
    if call:
        out.append('  ' + ('' if ret == 'void' else 'return ') + f'{call}(' + ', '.join(argv) + ');')
    else:
        out.append(f'  typedef {ret} (*Fn)({args});')
        out.append(f'  Fn fn = (Fn)gli_funcs[{i}];')
        out.append(f'  if (!fn) {{ t290_missing({i}, "{name}"); ' + ('return;' if ret == 'void' else f'return ({ret})0;') + ' }')
        out.append('  ' + ('' if ret == 'void' else 'return ') + 'fn(' + ', '.join(argv) + ');')
    if name in check_calls:
        assert ret == 'void', (name, ret)
        out.append(f'  t290_after_call({i}, "{name}");')
    out.append('}')
    rows.append((i, name, candidates))
out.append('static void t290_dispatch_init(void) {')
for i, name, candidates in rows:
    for candidate in candidates:
        out.append(f'  if (!gli_funcs[{i}]) gli_funcs[{i}] = mesa_proc("{candidate}");')
out.append('}')
out.append('static void *const gli_adapters[] = {')
out.extend(f'  (void *)gli_{i}, /* {name} */' for i, name, _ in rows)
out.append('};')
options.output.write_text('\n'.join(out) + '\n')
print(f'{len(rows)} GLI adapters generated')
