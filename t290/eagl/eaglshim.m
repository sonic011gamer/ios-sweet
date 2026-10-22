/* Mesa EAGL bridge for the SM-T290.  Only loaded into selected processes.
 * Keep eaglprobe.m for observing Apple's implementation independently.
 */
#import <Foundation/Foundation.h>
#include "surface.h"
#include <dispatch/dispatch.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gl_types.h"

extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *);
mach_port_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef);
kern_return_t IOServiceOpen(mach_port_t, mach_port_t, uint32_t, mach_port_t *);
kern_return_t IOObjectRelease(mach_port_t);
kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);

static mach_port_t log_conn;
static int log_fd = -1;
static void *mesa_lib;
static void *(*mesa_proc)(const char *);
static void *(*mesa_create)(unsigned, void *);
static void (*mesa_destroy)(void *);
static GLboolean (*mesa_current)(void *, void *, GLenum, GLsizei, GLsizei);
static void *(*mesa_image)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void (*mesa_image_destroy)(void *);
static void *gli_funcs[1100];
static pthread_key_t current_key;

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...)
{
    char buf[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (log_conn)
        IOConnectCallStructMethod(log_conn, 7, buf, n, NULL, NULL);
    buf[n++] = '\n';
    if (log_fd >= 0)
        write(log_fd, buf, n);
}

/* Diagnostics from the standalone test must survive unavailable stdio. */
void T290EAGLLog(const char *message);
void T290EAGLLog(const char *message) { plog("eagltest: %s", message); }

#if TARGET_OS_IPHONE
/*
 * Kernel 0129 has no IOSurface WrapClientImage (selector 6). Its generic
 * success reply constructs an empty IOSurfaceClient; CA then dereferences
 * its null shared-state pointer in IOSurfaceGetBulkAttachments. Report
 * unsupported here so CA::Render::Image::io_surface returns NULL and the
 * GLES renderer uploads the image through glTexImage/SubImage instead.
 * Real IOSurfaces (including the display target) still import into Mesa.
 */
extern IOSurfaceRef IOSurfaceWrapClientImage(uint32_t width, uint32_t height,
    uint32_t fourcc, uint32_t row_bytes, uint32_t options, void *address);
static IOSurfaceRef t290_wrap_client_image(uint32_t width, uint32_t height,
    uint32_t fourcc, uint32_t row_bytes, uint32_t options, void *address)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) < 8)
        plog("t290eagl: client image %ux%u fmt %#x: use GL texture upload", width, height, fourcc);
    return NULL;
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } surface_interpose[] = {
    {(const void *)t290_wrap_client_image, (const void *)IOSurfaceWrapClientImage},
};
#endif

/* Adreno imports require aligned rows. Preserve the caller's IOSurface
 * layout with a padded GPU surface, copying rows at the CPU/GPU boundary.
 * This also covers CA's format-less scratch surfaces (format supplied by
 * the EAGL call). The GL texture still renders and samples on the GPU. */
@interface T290SurfaceImage : NSObject {
@public
    IOSurfaceRef source, backing;
    size_t rows, rowBytes, sourceStride, backingStride;
    NSMutableData *snapshot;
    void *image;
    uint32_t seed;
    BOOL gpuDirty;
}
- (BOOL)sourceChanged;
- (void)upload;
- (void)publish;
@end
@implementation T290SurfaceImage
- (BOOL)sourceChanged
{
    return snapshot && IOSurfaceGetSeed(source) != seed;
}
- (void)upload
{
    if (!snapshot) return;
    const uint8_t *p = IOSurfaceGetBaseAddress(source);
    uint8_t *q = IOSurfaceGetBaseAddress(backing), *saved = snapshot.mutableBytes;
    for (size_t y = 0; y < rows; y++) {
        memcpy(q + y * backingStride, p + y * sourceStride, rowBytes);
        memcpy(saved + y * rowBytes, p + y * sourceStride, rowBytes);
    }
    seed = IOSurfaceGetSeed(source);
    gpuDirty = NO;
}
- (void)publish
{
    if (!gpuDirty) return;
    gpuDirty = NO;
    if (!snapshot) return;
    /* An independent CPU update after the last bind belongs to the next
     * upload. Do not overwrite it with an older GPU copy. */
    uint8_t *p = IOSurfaceGetBaseAddress(source), *saved = snapshot.mutableBytes;
    const uint8_t *q = IOSurfaceGetBaseAddress(backing);
    for (size_t y = 0; y < rows; y++) {
        if (!memcmp(saved + y * rowBytes, q + y * backingStride, rowBytes)) continue;
        if (memcmp(saved + y * rowBytes, p + y * sourceStride, rowBytes)) continue;
        memcpy(p + y * sourceStride, q + y * backingStride, rowBytes);
        memcpy(saved + y * rowBytes, q + y * backingStride, rowBytes);
    }
}
- (void)dealloc {
    if (image) mesa_image_destroy(image);
    if (source) CFRelease(source);
    if (backing) CFRelease(backing);
}
@end

@interface T290Sharegroup : NSObject
@property(nonatomic, assign) void *root;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, T290SurfaceImage *> *textures;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, T290SurfaceImage *> *renderbuffers;
@property(nonatomic, strong) NSMapTable<NSString *, T290SurfaceImage *> *surfaces;
@end
@implementation T290Sharegroup
- (void)dealloc { if (_root) mesa_destroy(_root); }
@end

@interface T290Context : NSObject {
@public
    void *_mesa;
    uint32_t _dummy;
    GLuint _drawFbo;
    BOOL _gpuWork;
    uint32_t _errors;
    NSMutableDictionary<NSNumber *, T290SurfaceImage *> *_framebuffers;
    NSMutableDictionary<NSNumber *, NSNumber *> *_attachmentNames;
    uint64_t _waitTicks;
    unsigned _waitCount, _flushCount, _drawCount;
    struct { void *renderer; void *dispatch[1100]; } _macro;
}
@property(nonatomic) unsigned API;
@property(nonatomic, strong) T290Sharegroup *sharegroup;
@property(nonatomic) BOOL multiThreaded;
@property(nonatomic, copy) NSString *debugLabel;
- (instancetype)initWithAPI:(unsigned)api group:(T290Sharegroup *)group;
@end

static void t290_make_current(void *ptr)
{
    T290Context *ctx = (__bridge T290Context *)ptr;
    if (pthread_getspecific(current_key) == ptr)
        return;
    if (!mesa_current(ctx ? ctx->_mesa : NULL, ctx ? &ctx->_dummy : NULL, 0x1401, 1, 1)) {
        plog("t290eagl: MesaMakeCurrent failed %p", ptr);
        return;
    }
    void *old = pthread_getspecific(current_key);
    if (ptr) CFRetain((__bridge CFTypeRef)ctx);
    pthread_setspecific(current_key, ptr);
    if (old) CFRelease(old);
}

static void release_current(void *ptr)
{
    mesa_current(NULL, NULL, 0x1401, 0, 0);
    CFRelease(ptr);
}

#define GLFN(name, ret, args) ((ret (*) args)mesa_proc(name))
static void t290_missing(unsigned slot, const char *name)
{
    static unsigned char warned[1100];
    if (slot < 1100 && !__atomic_exchange_n(&warned[slot], 1, __ATOMIC_RELAXED))
        plog("t290eagl: MISSING GLI slot %u (%s)", slot, name);
}

static void t290_delete_object(GLuint obj)
{
    if (GLFN("glIsShader", GLboolean, (GLuint))(obj))
        GLFN("glDeleteShader", void, (GLuint))(obj);
    else GLFN("glDeleteProgram", void, (GLuint))(obj);
}
static void t290_object_iv(GLuint obj, GLenum pname, GLint *value)
{
    if (pname == 0x8b4e) { /* GL_OBJECT_TYPE_ARB */
        *value = GLFN("glIsShader", GLboolean, (GLuint))(obj) ? 0x8b48 : 0x8b40;
        return;
    }
    const char *name = GLFN("glIsShader", GLboolean, (GLuint))(obj) ? "glGetShaderiv" : "glGetProgramiv";
    GLFN(name, void, (GLuint, GLenum, GLint *))(obj, pname, value);
}
static void t290_object_fv(GLuint obj, GLenum pname, GLfloat *value)
{
    GLint v = 0;
    t290_object_iv(obj, pname, &v);
    *value = v;
}
static void t290_info_log(GLuint obj, GLsizei cap, GLsizei *len, GLchar *buf)
{
    const char *name = GLFN("glIsShader", GLboolean, (GLuint))(obj) ? "glGetShaderInfoLog" : "glGetProgramInfoLog";
    GLFN(name, void, (GLuint, GLsizei, GLsizei *, GLchar *))(obj, cap, len, buf);
    if (buf && cap > 0 && *buf) plog("t290eagl: shader: %.700s", buf);
}
static const GLubyte *t290_get_string(GLenum name)
{
    /* Apple's private extension string. Do not advertise Apple-only state. */
    if (name == 0x8a1e) return (const GLubyte *)"";
    const GLubyte *p = GLFN("glGetString", const GLubyte *, (GLenum))(name);
    return p ? p : (const GLubyte *)"";
}
static void t290_wait(void)
{
    uint64_t start = mach_absolute_time();
    GLFN("glFinish", void, (void))();
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    ctx->_gpuWork = NO;
    ctx->_waitTicks += mach_absolute_time() - start;
    unsigned n = ++ctx->_waitCount;
    if (n <= 3 || n % 120 == 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        plog("t290eagl: GPU wait %u total %.1fms flush %u draw %u", n,
            (double)ctx->_waitTicks * tb.numer / tb.denom / 1e6, ctx->_flushCount, ctx->_drawCount);
    }
}
static void t290_finish(void)
{
    t290_wait();
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    for (T290SurfaceImage *img in ctx.sharegroup.textures.allValues) [img publish];
    for (T290SurfaceImage *img in ctx.sharegroup.renderbuffers.allValues) [img publish];
}

static void t290_finish_object(GLenum object, GLuint name)
{
    /* Apple's object fence is used before CA presents an IOSurface.
     * A full wait is conservative and also completes padded-row copies. */
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (ctx->_gpuWork) t290_wait();
    if (object == 0x1702) [ctx.sharegroup.textures[@(name)] publish];
    else if (object == 0x8d41) [ctx.sharegroup.renderbuffers[@(name)] publish];
}
static void t290_flush(void)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    ++ctx->_flushCount;
    GLFN("glFlush", void, (void))();
    /* The framebuffer driver consumes surface1 on the CPU immediately
     * after CA's flush. Intermediate GPU textures need no CPU round trip. */
    T290SurfaceImage *display = nil;
    for (T290SurfaceImage *img in ctx.sharegroup.textures.allValues)
        if (img->gpuDirty && IOSurfaceGetID(img->source) == 1) { display = img; break; }
    if (display) {
        if (ctx->_gpuWork) t290_wait();
        [display publish];
    }
}
static void t290_bind_framebuffer(GLenum target, GLuint name)
{
    GLFN("glBindFramebuffer", void, (GLenum, GLuint))(target, name);
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (target == 0x8d40 || target == 0x8ca9) ctx->_drawFbo = name;
}
static void t290_framebuffer_texture(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level)
{
    GLFN("glFramebufferTexture2D", void, (GLenum, GLenum, GLenum, GLuint, GLint))(target, attachment, textarget, name, level);
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (target != 0x8ca8 && attachment == 0x8ce0) {
        ctx->_attachmentNames[@(ctx->_drawFbo)] = @(name);
        T290SurfaceImage *img = ctx.sharegroup.textures[@(name)];
        if (img) ctx->_framebuffers[@(ctx->_drawFbo)] = img;
        else [ctx->_framebuffers removeObjectForKey:@(ctx->_drawFbo)];
    }
}
static void t290_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint name)
{
    GLFN("glFramebufferRenderbuffer", void, (GLenum, GLenum, GLenum, GLuint))(target, attachment, rbtarget, name);
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (target != 0x8ca8 && attachment == 0x8ce0) {
        ctx->_attachmentNames[@(ctx->_drawFbo)] = @(0x100000000ull | name);
        T290SurfaceImage *img = ctx.sharegroup.renderbuffers[@(name)];
        if (img) ctx->_framebuffers[@(ctx->_drawFbo)] = img;
        else [ctx->_framebuffers removeObjectForKey:@(ctx->_drawFbo)];
    }
}
static void t290_delete_framebuffers(GLsizei n, const GLuint *names)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    for (GLsizei i = 0; i < n; i++) {
        [ctx->_framebuffers removeObjectForKey:@(names[i])];
        [ctx->_attachmentNames removeObjectForKey:@(names[i])];
        if (ctx->_drawFbo == names[i]) ctx->_drawFbo = 0;
    }
    GLFN("glDeleteFramebuffers", void, (GLsizei, const GLuint *))(n, names);
}
static void t290_before_draw(void)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    ctx->_gpuWork = YES;
    ++ctx->_drawCount;
    T290SurfaceImage *img = ctx->_framebuffers[@(ctx->_drawFbo)];
    if (img) img->gpuDirty = YES;
}
static void t290_texture_write(BOOL redefine)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    ctx->_gpuWork = YES;
    GLint name = 0;
    GLFN("glGetIntegerv", void, (GLenum, GLint *))(0x8069, &name);
    if (redefine) {
        [ctx.sharegroup.textures removeObjectForKey:@(name)];
        for (NSNumber *fbo in ctx->_attachmentNames)
            if ([ctx->_attachmentNames[fbo] unsignedLongLongValue] == (GLuint)name)
                [ctx->_framebuffers removeObjectForKey:fbo];
    }
    else {
        T290SurfaceImage *img = ctx.sharegroup.textures[@(name)];
        if (img) img->gpuDirty = YES;
    }
}
static void t290_after_call(unsigned slot, const char *name)
{
    GLenum err = GLFN("glGetError", GLenum, (void))();
    if (!err) return;
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (err >= 0x500 && err < 0x520) ctx->_errors |= 1u << (err - 0x500);
    static unsigned short counts[1100];
    unsigned n = ++counts[slot];
    if (n <= 4 || (n & (n - 1)) == 0)
        plog("t290eagl: GL error %#x after %s (slot %u count %u)", err, name, slot, n);
}
static GLenum t290_get_error(void)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    if (ctx->_errors) {
        unsigned bit = __builtin_ctz(ctx->_errors);
        ctx->_errors &= ~(1u << bit);
        return 0x500 + bit;
    }
    return GLFN("glGetError", GLenum, (void))();
}
static void t290_bind_texture(GLenum target, GLuint name)
{
    GLFN("glBindTexture", void, (GLenum, GLuint))(target, name);
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    T290SurfaceImage *img = target == 0x0de1 ? ctx.sharegroup.textures[@(name)] : nil;
    if (img && [img sourceChanged]) {
        if (ctx->_gpuWork) t290_wait();
        [img upload];
    }
}
static void t290_delete_textures(GLsizei n, const GLuint *names)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    for (GLsizei i = 0; i < n; i++) {
        T290SurfaceImage *img = ctx.sharegroup.textures[@(names[i])];
        if (img && img->gpuDirty) { t290_finish(); break; }
    }
    for (GLsizei i = 0; i < n; i++) [ctx.sharegroup.textures removeObjectForKey:@(names[i])];
    GLFN("glDeleteTextures", void, (GLsizei, const GLuint *))(n, names);
}
static void t290_delete_renderbuffers(GLsizei n, const GLuint *names)
{
    T290Context *ctx = (__bridge T290Context *)pthread_getspecific(current_key);
    for (GLsizei i = 0; i < n; i++) {
        T290SurfaceImage *img = ctx.sharegroup.renderbuffers[@(names[i])];
        if (img && img->gpuDirty) { t290_finish(); break; }
    }
    for (GLsizei i = 0; i < n; i++) [ctx.sharegroup.renderbuffers removeObjectForKey:@(names[i])];
    GLFN("glDeleteRenderbuffers", void, (GLsizei, const GLuint *))(n, names);
}

#include "dispatch_generated.h"

static uintptr_t unsupported_private(void *ctx, ...)
{
    t290_missing(1099, "unknown private extension");
    return 0;
}

@implementation T290Context
- (instancetype)initWithAPI:(unsigned)api group:(T290Sharegroup *)group
{
    if (!(self = [super init])) return nil;
    if (!mesa_create || (api != 2 && api != 3)) return nil;
    if (!group) {
        group = [T290Sharegroup new];
        group.root = mesa_create(api, NULL);
        if (!group.root) return nil;
        group.textures = [NSMutableDictionary dictionary];
        group.renderbuffers = [NSMutableDictionary dictionary];
        group.surfaces = [NSMapTable strongToWeakObjectsMapTable];
    }
    _sharegroup = group;
    _framebuffers = [NSMutableDictionary dictionary];
    _attachmentNames = [NSMutableDictionary dictionary];
    _API = api;
    _mesa = mesa_create(api, group.root);
    if (!_mesa) return nil;
    _macro.renderer = (__bridge void *)self;
    for (size_t i = 0; i < 1100; i++) _macro.dispatch[i] = (void *)unsupported_private;
    memcpy(_macro.dispatch, gli_adapters, sizeof(gli_adapters));
    plog("t290eagl: created ES%u ctx %p Mesa %p", api, (__bridge void *)self, _mesa);
    return self;
}
- (void)dealloc { if (_mesa) mesa_destroy(_mesa); }
- (void *)getMacroContextPrivate { return &_macro; }
- (void *)GetMacroContextPrivate { return &_macro; }
- (BOOL)isMultiThreaded { return _multiThreaded; }
- (int)getParameter:(unsigned)pname to:(int *)value
{
    if (value) *value = pname == 0x260 ? 1 : 0;
    static unsigned n;
    if (n++ < 12) plog("t290eagl: getParameter %#x", pname);
    return 0;
}
- (int)setParameter:(unsigned)pname to:(const int *)value { return 0; }
- (void)setBlockFence:(dispatch_block_t)block onQueue:(dispatch_queue_t)queue
{
    t290_make_current((__bridge void *)self);
    t290_finish();
    if (block) dispatch_async(queue ?: dispatch_get_global_queue(0, 0), block);
}
- (BOOL)texImageIOSurface:(IOSurfaceRef)surface target:(GLenum)target
          internalFormat:(GLenum)internal width:(GLsizei)w height:(GLsizei)h
                  format:(GLenum)format type:(GLenum)type plane:(unsigned)plane invert:(BOOL)invert
{
    if (!surface || !mesa_image || !mesa_image_destroy) return NO;
    if ((target != 0x0de1 && target != 0x8d41) || w <= 0 || h <= 0) return NO;
    t290_make_current((__bridge void *)self);
    size_t planes = IOSurfaceGetPlaneCount(surface);
    if ((planes && plane >= planes) || (!planes && plane)) return NO;
    size_t stride = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface);
    uintptr_t offset = planes ? (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, plane) -
        (uintptr_t)IOSurfaceGetBaseAddress(surface) : 0;
    uint32_t fourcc = IOSurfaceGetPixelFormat(surface);
    if (!fourcc) {
        /* CA scratch buffers omit PixelFormat. The EAGL arguments carry
         * its format; attachImage's legacy default is RGBA/UNSIGNED_BYTE. */
        if (format == 0x1906) fourcc = 0x41303038;
        else if (format == 0x1909) fourcc = 0x4c303038;
        else if (format == 0x190a) fourcc = 0x4c413038;
        else if (format == 0x80e1) fourcc = 0x42475241;
        else if (format == 0x1908 && (type == 0x1401 || type == 0x8367)) fourcc = 0x52474241;
        static unsigned count;
        if (count++ < 12) plog("t290eagl: inferred fmt %#x for surface %u (internal %#x format %#x type %#x)",
            fourcc, IOSurfaceGetID(surface), internal, format, type);
    }
    GLint name = 0;
    GLFN("glGetIntegerv", void, (GLenum, GLint *))(target == 0x8d41 ? 0x8ca7 : 0x8069, &name);
    if (name <= 0) return NO;
    NSMutableDictionary *images = target == 0x8d41 ? _sharegroup.renderbuffers : _sharegroup.textures;
    NSString *key = [NSString stringWithFormat:@"%u:%d:%d:%u:%u:%zu:%zu",
        IOSurfaceGetID(surface), w, h, fourcc, plane, stride, (size_t)offset];
    T290SurfaceImage *mirror = [_sharegroup.surfaces objectForKey:key];
    if (!mirror) {
        mirror = [T290SurfaceImage new];
        mirror->source = (IOSurfaceRef)CFRetain(surface);
    }
    if (!mirror->image && !planes && (stride & 255)) {
        size_t bpe = fourcc == 0x4c413038 ? 2 :
            (fourcc == 0x4c303038 || fourcc == 0x41303038) ? 1 :
            (fourcc == 0x42475241 || fourcc == 0x52474241) ? 4 : 0;
        if (!bpe || w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / bpe ||
            (size_t)h > IOSurfaceGetHeight(surface) || (size_t)w * bpe > stride ||
            !IOSurfaceGetBaseAddress(surface)) return NO;
        {
            mirror->rows = h;
            mirror->rowBytes = (size_t)w * bpe;
            mirror->sourceStride = stride;
            mirror->backing = IOSurfaceCreate((__bridge CFDictionaryRef)@{
                (__bridge id)kIOSurfaceWidth: @(w), (__bridge id)kIOSurfaceHeight: @(h),
                (__bridge id)kIOSurfaceBytesPerElement: @(bpe),
                (__bridge id)kIOSurfaceBytesPerRow: @((mirror->rowBytes + 255) & ~(size_t)255),
                (__bridge id)kIOSurfacePixelFormat: @(fourcc), (__bridge id)kIOSurfaceIsGlobal: @YES
            });
            if (!mirror->backing) return NO;
            mirror->backingStride = IOSurfaceGetBytesPerRow(mirror->backing);
            if ((mirror->backingStride & 255) || !IOSurfaceGetBaseAddress(mirror->backing)) return NO;
            mirror->snapshot = [NSMutableData dataWithLength:mirror->rowBytes * mirror->rows];
            [mirror upload];
            plog("t290eagl: padded surface %u %dx%d fmt %#x stride %zu -> %zu",
                IOSurfaceGetID(surface), w, h, fourcc, stride, mirror->backingStride);
        }
    }
    if ([mirror sourceChanged]) {
        if (_gpuWork) t290_wait();
        [mirror upload];
    }
    if (!mirror->image) {
        IOSurfaceRef imported = mirror->backing ?: surface;
        if (mirror->backing) {
            stride = mirror->backingStride;
            offset = 0;
        }
        mirror->image = mesa_image(IOSurfaceGetID(imported), w, h, (uint32_t)stride,
            (uint32_t)offset, fourcc);
        if (!mirror->image) {
            plog("t290eagl: import FAILED id %u %dx%d stride %zu fourcc %#x plane %u",
                IOSurfaceGetID(surface), w, h, stride, (unsigned)IOSurfaceGetPixelFormat(surface), plane);
            return NO;
        }
        [_sharegroup.surfaces setObject:mirror forKey:key];
    }
    const char *fn = target == 0x8d41 ? "glEGLImageTargetRenderbufferStorageOES" : "glEGLImageTargetTexture2DOES";
    void (*bind)(GLenum, void *) = (void (*)(GLenum, void *))mesa_proc(fn);
    if (bind) bind(target, mirror->image);
    if (bind) {
        images[@(name)] = mirror;
        uint64_t attachmentName = (uint64_t)(GLuint)name | (target == 0x8d41 ? 0x100000000ull : 0);
        for (NSNumber *fbo in _attachmentNames)
            if ([_attachmentNames[fbo] unsignedLongLongValue] == attachmentName)
                _framebuffers[fbo] = mirror;
    }
    static unsigned count;
    if (count++ < 12) plog("t290eagl: imported surface %u %dx%d target %#x invert %d",
        IOSurfaceGetID(surface), w, h, target, invert);
    return bind != NULL;
}
- (BOOL)texImageIOSurface:(IOSurfaceRef)s target:(GLenum)t internalFormat:(GLenum)i
                   width:(GLsizei)w height:(GLsizei)h format:(GLenum)f type:(GLenum)y plane:(unsigned)p
{
    return [self texImageIOSurface:s target:t internalFormat:i width:w height:h format:f type:y plane:p invert:NO];
}
- (BOOL)attachImage:(GLenum)target toCoreSurface:(IOSurfaceRef)s invertedRender:(BOOL)invert
{
    return [self texImageIOSurface:s target:target internalFormat:0x8058 width:(GLsizei)IOSurfaceGetWidth(s)
        height:(GLsizei)IOSurfaceGetHeight(s) format:0x1908 type:0x1401 plane:0 invert:invert];
}
- (BOOL)renderbufferStorage:(GLenum)target fromDrawable:(id)drawable
{
    plog("t290eagl: unsupported drawable renderbuffer");
    return NO;
}
- (BOOL)presentRenderbuffer:(GLenum)target { t290_make_current((__bridge void *)self); t290_finish(); return YES; }
- (BOOL)presentRenderbuffer:(GLenum)target atTime:(double)time { return [self presentRenderbuffer:target]; }
- (BOOL)presentRenderbuffer:(GLenum)target afterMinimumDuration:(double)t { return [self presentRenderbuffer:target]; }
@end

/* EAGL allocates an Apple object; these initializers substitute our object.
 * Ownership follows the ObjC init method family even though its class differs.
 */
static id init_api(id self, SEL cmd, unsigned api) __attribute__((ns_returns_retained));
static id init_api(id self, SEL cmd, unsigned api) { return [[T290Context alloc] initWithAPI:api group:nil]; }
static id init_props(id self, SEL cmd, unsigned api, id props) __attribute__((ns_returns_retained));
static id init_props(id self, SEL cmd, unsigned api, id props) { return [[T290Context alloc] initWithAPI:api group:nil]; }
static id init_group(id self, SEL cmd, unsigned api, T290Sharegroup *group) __attribute__((ns_returns_retained));
static id init_group(id self, SEL cmd, unsigned api, T290Sharegroup *group) { return [[T290Context alloc] initWithAPI:api group:group]; }
static BOOL set_current(id cls, SEL cmd, id ctx)
{
    if (ctx && ![ctx isKindOfClass:[T290Context class]]) return NO;
    t290_make_current((__bridge void *)ctx);
    return pthread_getspecific(current_key) == (__bridge void *)ctx;
}
static id get_current(id cls, SEL cmd) { return (__bridge id)pthread_getspecific(current_key); }
static void replace(Class c, const char *name, IMP fn, BOOL meta)
{
    Method m = class_getInstanceMethod(meta ? object_getClass(c) : c, sel_registerName(name));
    if (m) method_setImplementation(m, fn);
    else plog("t290eagl: no method %s", name);
}

__attribute__((constructor)) static void bridge_init(void)
{
    mach_port_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
    if (service) { IOServiceOpen(service, mach_task_self(), 0, &log_conn); IOObjectRelease(service); }
    log_fd = open("/private/var/tmp/t290eagl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    pthread_key_create(&current_key, release_current);
    mesa_lib = dlopen(getenv("T290_MESA_LIB") ?: "/usr/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!mesa_lib) { plog("t290eagl: dlopen failed: %s", dlerror()); return; }
    mesa_proc = dlsym(mesa_lib, "OSMesaGetProcAddress");
    mesa_create = dlsym(mesa_lib, "OSMesaT290CreateContext");
    mesa_destroy = dlsym(mesa_lib, "OSMesaDestroyContext");
    mesa_current = dlsym(mesa_lib, "OSMesaMakeCurrent");
    mesa_image = dlsym(mesa_lib, "OSMesaT290CreateImage");
    mesa_image_destroy = dlsym(mesa_lib, "OSMesaT290DestroyImage");
    if (!mesa_proc || !mesa_create || !mesa_destroy || !mesa_current || !mesa_image || !mesa_image_destroy) {
        plog("t290eagl: incompatible Mesa: required export missing");
        mesa_create = NULL;
        return;
    }
    t290_dispatch_init();
    Class c = objc_getClass("EAGLContext");
    if (c) {
        replace(c, "initWithAPI:", (IMP)init_api, NO);
        replace(c, "initWithAPI:properties:", (IMP)init_props, NO);
        replace(c, "initWithAPI:sharegroup:", (IMP)init_group, NO);
        replace(c, "setCurrentContext:", (IMP)set_current, YES);
        replace(c, "currentContext", (IMP)get_current, YES);
    }
    plog("t290eagl: Mesa bridge ready in %s; GLI adapters %zu, EAGL class %p",
        getprogname(), sizeof(gli_adapters) / sizeof(gli_adapters[0]), c);
}
