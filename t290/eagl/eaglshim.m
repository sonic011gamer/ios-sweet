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
static BOOL trace_render;
static pthread_key_t current_key;
/* The pthread key owns the context and runs its exit destructor. The raw
 * TLS mirror avoids a pthread lookup at every context-first GLI call. */
static __thread void *thread_current;

/* Mesa entry points the bridge calls itself, resolved once at load time:
 * OSMesaGetProcAddress is a linear strcmp search over the whole glapi
 * table, far too slow for every draw and error check. */
static struct {
    GLenum (*GetError)(void);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*Finish)(void), (*Flush)(void);
    GLboolean (*IsShader)(GLuint);
    void (*DeleteShader)(GLuint), (*DeleteProgram)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *), (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    const GLubyte *(*GetString)(GLenum);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*DeleteTextures)(GLsizei, const GLuint *), (*DeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*ActiveTexture)(GLenum), (*Enable)(GLenum), (*Disable)(GLenum);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei), (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*EGLImageTargetTexture2DOES)(GLenum, void *);
    void (*EGLImageTargetRenderbufferStorageOES)(GLenum, void *);
    void (*TexParameteri)(GLenum, GLenum, GLint), (*GenerateMipmap)(GLenum);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*AttachShader)(GLuint, GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    void (*GetTexParameteriv)(GLenum, GLenum, GLint *);
    void (*GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
} gl;

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

/* Adreno imports require rows aligned to 64 pixels (Freedreno's GMEM
 * resolve). Other layouts get a padded GPU surface; rows are copied at the
 * CPU/GPU boundary. This also covers CA's format-less scratch surfaces
 * (format supplied by the EAGL call). The GL texture still renders and
 * samples on the GPU.
 *
 * CA treats IOSurface textures as zero-copy: it neither rebinds nor
 * re-specifies a texture after another process draws new pixels, and it
 * presents after redrawing only the damaged rectangle. So CPU updates
 * (IOSurface seed) are checked before every draw for each bound texture,
 * and GPU writes are tracked as a row range (viewport/scissor) so only
 * those rows are copied back. */
@interface T290SurfaceImage : NSObject {
@public
    IOSurfaceRef source, backing;
    size_t rows, rowBytes, sourceStride, sourceOffset, backingStride;
    size_t dirtyStart, dirtyEnd;        /* GPU-written rows not yet published */
    void *image;
    uint32_t seed;
    BOOL gpuDirty;
    BOOL scanout;                       /* full-screen: CA may present it */
}
- (void)upload;
- (void)publish;
@end

static struct {
    uint64_t publishBytes, uploadBytes, waitTicks, frameT0, glTicks, copyTicks;
    unsigned frames, uploads;
} stats;

/* CPU wrote the original surface since the last upload (IOSurface seed) */
static inline BOOL image_stale(T290SurfaceImage *img)
{
    return img->backing && IOSurfaceGetSeed(img->source) != img->seed;
}

static inline void image_mark(T290SurfaceImage *img, size_t y0, size_t y1)
{
    if (y0 < img->dirtyStart) img->dirtyStart = y0;
    if (y1 > img->dirtyEnd) img->dirtyEnd = y1;
    img->gpuDirty = YES;
}

@implementation T290SurfaceImage
- (instancetype)init
{
    if ((self = [super init])) dirtyStart = SIZE_MAX;
    return self;
}
/* Caller has waited for GPU work that may still read the backing. */
- (void)upload
{
    if (!backing) return;
    /* GPU results not yet copied out must not be lost to an older copy. */
    [self publish];
    uint64_t t0 = mach_absolute_time();
    const uint8_t *p = (const uint8_t *)IOSurfaceGetBaseAddress(source) + sourceOffset;
    uint8_t *q = IOSurfaceGetBaseAddress(backing);
    seed = IOSurfaceGetSeed(source);
    for (size_t y = 0; y < rows; y++)
        memcpy(q + y * backingStride, p + y * sourceStride, rowBytes);
    stats.uploadBytes += rows * rowBytes;
    stats.uploads++;
    stats.copyTicks += mach_absolute_time() - t0;
}
/* Caller has waited for the GPU writes. */
- (void)publish
{
    if (!gpuDirty) return;
    gpuDirty = NO;
    size_t y0 = dirtyStart, y1 = dirtyEnd < rows ? dirtyEnd : rows;
    dirtyStart = SIZE_MAX;
    dirtyEnd = 0;
    if (!backing || y0 >= y1) return;
    uint64_t t0 = mach_absolute_time();
    uint8_t *p = (uint8_t *)IOSurfaceGetBaseAddress(source) + sourceOffset;
    const uint8_t *q = IOSurfaceGetBaseAddress(backing);
    for (size_t y = y0; y < y1; y++)
        memcpy(p + y * sourceStride, q + y * backingStride, rowBytes);
    stats.publishBytes += (y1 - y0) * rowBytes;
    stats.copyTicks += mach_absolute_time() - t0;
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
@property(nonatomic, strong) NSMutableSet<T290SurfaceImage *> *dirty;   /* padded, GPU-written */
@property(nonatomic, weak) T290SurfaceImage *display;                  /* surface 1 (scanout) */
@property(nonatomic, strong) NSMutableIndexSet *mipmapped;             /* generated complete chains */
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, NSMutableDictionary *> *mipLevels;
@end
@implementation T290Sharegroup
- (void)dealloc { if (_root) mesa_destroy(_root); }
@end

#define T290_UNITS 16
enum { DRAW_PRIMITIVES, DRAW_CLEAR, DRAW_ALL_ROWS };

@interface T290Context : NSObject {
@public
    void *_mesa;
    uint32_t _dummy;
    GLuint _drawFbo;
    BOOL _gpuWork;
    uint32_t _errors;
    NSMutableDictionary<NSNumber *, T290SurfaceImage *> *_framebuffers;
    NSMutableDictionary<NSNumber *, NSNumber *> *_attachmentNames;
    T290SurfaceImage *_drawImage;       /* padded or scanout image behind _drawFbo */
    BOOL _presentDirty;                 /* GPU drew into a scanout surface */
    T290SurfaceImage *_bound[T290_UNITS];  /* padded 2D texture per unit */
    GLuint _boundName[T290_UNITS], _unit, _units;
    GLint _viewport[4], _scissor[4];
    BOOL _viewportKnown, _scissorKnown, _scissorOn;
    int _maxLevelState;                 /* 0 unknown, 1 TEXTURE_MAX_LEVEL usable, 2 not */
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
    if (thread_current == ptr)
        return;
    T290Context *ctx = (__bridge T290Context *)ptr;
    if (!mesa_current(ctx ? ctx->_mesa : NULL, ctx ? &ctx->_dummy : NULL, 0x1401, 1, 1)) {
        plog("t290eagl: MesaMakeCurrent failed %p", ptr);
        return;
    }
    void *old = thread_current;
    if (ptr) CFRetain((__bridge CFTypeRef)ctx);
    pthread_setspecific(current_key, ptr);
    thread_current = ptr;
    if (old) CFRelease(old);
}

static void release_current(void *ptr)
{
    mesa_current(NULL, NULL, 0x1401, 0, 0);
    thread_current = NULL;
    CFRelease(ptr);
}

static inline T290Context *current_ctx(void)
{
    return (__bridge T290Context *)thread_current;
}

static void t290_missing(unsigned slot, const char *name)
{
    static unsigned char warned[1100];
    if (slot < 1100 && !__atomic_exchange_n(&warned[slot], 1, __ATOMIC_RELAXED))
        plog("t290eagl: MISSING GLI slot %u (%s)", slot, name);
}

static void t290_delete_object(GLuint obj)
{
    if (gl.IsShader(obj)) gl.DeleteShader(obj);
    else gl.DeleteProgram(obj);
}
static void t290_object_iv(GLuint obj, GLenum pname, GLint *value)
{
    if (pname == 0x8b4e) { /* GL_OBJECT_TYPE_ARB */
        *value = gl.IsShader(obj) ? 0x8b48 : 0x8b40;
        return;
    }
    if (gl.IsShader(obj)) gl.GetShaderiv(obj, pname, value);
    else gl.GetProgramiv(obj, pname, value);
}
static void t290_object_fv(GLuint obj, GLenum pname, GLfloat *value)
{
    GLint v = 0;
    t290_object_iv(obj, pname, &v);
    *value = v;
}
static void t290_info_log(GLuint obj, GLsizei cap, GLsizei *len, GLchar *buf)
{
    if (gl.IsShader(obj)) gl.GetShaderInfoLog(obj, cap, len, buf);
    else gl.GetProgramInfoLog(obj, cap, len, buf);
    if (buf && cap > 0 && *buf) plog("t290eagl: shader: %.700s", buf);
}
static const GLubyte *t290_get_string(GLenum name)
{
    /* Apple's private extension string. Do not advertise Apple-only state. */
    if (name == 0x8a1e) return (const GLubyte *)"";
    const GLubyte *p = gl.GetString(name);
    return p ? p : (const GLubyte *)"";
}

static double ticks_ms(uint64_t ticks)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)ticks * tb.numer / tb.denom / 1e6;
}

static void t290_wait(void)
{
    uint64_t start = mach_absolute_time();
    gl.Finish();
    T290Context *ctx = current_ctx();
    ctx->_gpuWork = NO;
    uint64_t t = mach_absolute_time() - start;
    ctx->_waitTicks += t;
    stats.waitTicks += t;
    unsigned n = ++ctx->_waitCount;
    if (n <= 3)
        plog("t290eagl: GPU wait %u total %.1fms flush %u draw %u", n,
            ticks_ms(ctx->_waitTicks), ctx->_flushCount, ctx->_drawCount);
}
static void publish_dirty(T290Sharegroup *group)
{
    if (!group.dirty.count) return;
    for (T290SurfaceImage *img in group.dirty) [img publish];
    [group.dirty removeAllObjects];
}
static void t290_finish(void)
{
    T290Context *ctx = current_ctx();
    t290_wait();
    publish_dirty(ctx.sharegroup);
}

static void t290_finish_object(GLenum object, GLuint name)
{
    /* Apple's object fence is used before CA presents or samples an
     * IOSurface: a padded one needs its GPU rows copied back. */
    T290Context *ctx = current_ctx();
    T290SurfaceImage *img = object == 0x1702 ? ctx.sharegroup.textures[@(name)] :
        object == 0x8d41 ? ctx.sharegroup.renderbuffers[@(name)] : nil;
    if (img && img->backing && !img->gpuDirty) return;    /* already copied out */
    if (ctx->_gpuWork) t290_wait();
    if (img) {
        [img publish];
        [ctx.sharegroup.dirty removeObject:img];
    }
}

/* Once per 300 presented frames: where the CPU time of the bridge goes. */
static void frame_stats(T290Context *ctx)
{
    uint64_t now = mach_absolute_time();
    if (!stats.frames++) {
        stats.frameT0 = now;
        return;
    }
    if (stats.frames < 301) return;
    double ms = ticks_ms(now - stats.frameT0);
    /* frame time = GL calls (Mesa CPU work incl. GPU waits and copies) + the rest (CA) */
    plog("t290eagl: %.1f fps over 300 frames; per frame %.1fms: GL calls %.1fms (GPU wait %.1fms, "
         "copies %.1fms: out %zuKB in %zuKB, %u uploads), outside GL %.1fms; draws %u flushes %u",
        300e3 / ms, ms / 300, ticks_ms(stats.glTicks) / 300, ticks_ms(stats.waitTicks) / 300,
        ticks_ms(stats.copyTicks) / 300, (size_t)(stats.publishBytes / 300 / 1024),
        (size_t)(stats.uploadBytes / 300 / 1024), stats.uploads,
        (ms - ticks_ms(stats.glTicks)) / 300, ctx->_drawCount, ctx->_flushCount);
    memset(&stats, 0, sizeof(stats));
    stats.frames = 1;
    stats.frameT0 = now;
}

static void t290_flush(void)
{
    T290Context *ctx = current_ctx();
    ++ctx->_flushCount;
    /* CA flushes, then swaps a full-screen surface (1 or its second
     * buffer) and the framebuffer driver converts it on the CPU at once:
     * the frame must be complete. Intermediate GPU textures need no wait. */
    if (!ctx->_presentDirty) {
        gl.Flush();
        return;
    }
    ctx->_presentDirty = NO;
    /* Finish already submits the pending commands. A preceding Flush
     * forced a second trip through Mesa's submit path for every frame. */
    if (ctx->_gpuWork) t290_wait();
    else gl.Flush();
    T290SurfaceImage *display = ctx.sharegroup.display;
    if (display && display->gpuDirty) {
        [display publish];
        [ctx.sharegroup.dirty removeObject:display];
    }
    frame_stats(ctx);
}

static void update_draw_image(T290Context *ctx)
{
    T290SurfaceImage *img = ctx->_framebuffers[@(ctx->_drawFbo)];
    ctx->_drawImage = img && (img->backing || img->scanout) ? img : nil;
}
static void t290_bind_framebuffer(GLenum target, GLuint name)
{
    gl.BindFramebuffer(target, name);
    T290Context *ctx = current_ctx();
    if (target == 0x8d40 || target == 0x8ca9) {
        ctx->_drawFbo = name;
        update_draw_image(ctx);
    }
}
static void t290_framebuffer_texture(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level)
{
    gl.FramebufferTexture2D(target, attachment, textarget, name, level);
    T290Context *ctx = current_ctx();
    if (target != 0x8ca8 && attachment == 0x8ce0) {
        ctx->_attachmentNames[@(ctx->_drawFbo)] = @(name);
        T290SurfaceImage *img = ctx.sharegroup.textures[@(name)];
        if (img) ctx->_framebuffers[@(ctx->_drawFbo)] = img;
        else [ctx->_framebuffers removeObjectForKey:@(ctx->_drawFbo)];
        update_draw_image(ctx);
    }
}
static void t290_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint name)
{
    gl.FramebufferRenderbuffer(target, attachment, rbtarget, name);
    T290Context *ctx = current_ctx();
    if (target != 0x8ca8 && attachment == 0x8ce0) {
        ctx->_attachmentNames[@(ctx->_drawFbo)] = @(0x100000000ull | name);
        T290SurfaceImage *img = ctx.sharegroup.renderbuffers[@(name)];
        if (img) ctx->_framebuffers[@(ctx->_drawFbo)] = img;
        else [ctx->_framebuffers removeObjectForKey:@(ctx->_drawFbo)];
        update_draw_image(ctx);
    }
}
static void t290_delete_framebuffers(GLsizei n, const GLuint *names)
{
    T290Context *ctx = current_ctx();
    for (GLsizei i = 0; i < n; i++) {
        [ctx->_framebuffers removeObjectForKey:@(names[i])];
        [ctx->_attachmentNames removeObjectForKey:@(names[i])];
        if (ctx->_drawFbo == names[i]) ctx->_drawFbo = 0;
    }
    update_draw_image(ctx);
    gl.DeleteFramebuffers(n, names);
}

static void t290_active_texture(GLenum unit)
{
    gl.ActiveTexture(unit);
    T290Context *ctx = current_ctx();
    if (unit >= 0x84c0 && unit < 0x84c0 + T290_UNITS) ctx->_unit = unit - 0x84c0;
}
static void t290_enable(GLenum cap)
{
    gl.Enable(cap);
    if (cap == 0x0c11) current_ctx()->_scissorOn = YES;
}
static void t290_disable(GLenum cap)
{
    gl.Disable(cap);
    if (cap == 0x0c11) current_ctx()->_scissorOn = NO;
}
static void t290_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    gl.Scissor(x, y, w, h);
    T290Context *ctx = current_ctx();
    ctx->_scissor[0] = x, ctx->_scissor[1] = y, ctx->_scissor[2] = w, ctx->_scissor[3] = h;
    ctx->_scissorKnown = YES;
}
static void t290_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    gl.Viewport(x, y, w, h);
    T290Context *ctx = current_ctx();
    ctx->_viewport[0] = x, ctx->_viewport[1] = y, ctx->_viewport[2] = w, ctx->_viewport[3] = h;
    ctx->_viewportKnown = YES;
}

static void t290_before_draw(int kind)
{
    T290Context *ctx = current_ctx();
    ++ctx->_drawCount;
    /* New pixels from another process in a padded texture we sample. */
    for (GLuint u = 0; u < ctx->_units; u++) {
        T290SurfaceImage *img = ctx->_bound[u];
        if (img && image_stale(img)) {
            if (ctx->_gpuWork) t290_wait();
            [img upload];
        }
    }
    ctx->_gpuWork = YES;
    T290SurfaceImage *img = ctx->_drawImage;
    if (!img) return;
    if (img->scanout) ctx->_presentDirty = YES;
    if (!img->backing) return;          /* zero-copy: nothing to copy back */
    /* Rows the GL call can touch: FBO row 0 is the first row in memory. */
    long y0 = 0, y1 = (long)img->rows;
    if (kind == DRAW_PRIMITIVES) {
        if (ctx->_viewportKnown) {
            y0 = MAX(y0, (long)ctx->_viewport[1]);
            y1 = MIN(y1, (long)ctx->_viewport[1] + ctx->_viewport[3]);
        }
    }
    if (kind != DRAW_ALL_ROWS && ctx->_scissorOn && ctx->_scissorKnown) {
        y0 = MAX(y0, (long)ctx->_scissor[1]);
        y1 = MIN(y1, (long)ctx->_scissor[1] + ctx->_scissor[3]);
    }
    if (y0 >= y1) return;
    if (!img->gpuDirty) [ctx.sharegroup.dirty addObject:img];
    image_mark(img, (size_t)y0, (size_t)y1);
}

/* Bounded diagnostics for disappearing content. Enabled by a file marker;
 * normal rendering does not query or log this state. */
static void t290_trace_draw(T290Context *ctx)
{
    if (!trace_render || !gl.GetTexLevelParameteriv) return;
    unsigned n = ctx->_drawCount;
    if (n > 48 && (n > 1024 || (n & (n - 1)))) return;
    GLenum pending = gl.GetError();
    if (pending >= 0x500 && pending < 0x520) ctx->_errors |= 1u << (pending - 0x500);
    GLint program = 0, active = 0, viewport[4] = {0}, mask[4] = {0};
    gl.GetIntegerv(0x8b8d, &program);
    gl.GetIntegerv(0x84e0, &active);
    gl.GetIntegerv(0x0ba2, viewport);
    gl.GetIntegerv(0x0c23, mask);
    plog("TRACE draw %u ctx %p program %d fbo %u viewport %d,%d,%d,%d color-mask %d%d%d%d",
        n, (__bridge void *)ctx, program, ctx->_drawFbo, viewport[0], viewport[1],
        viewport[2], viewport[3], mask[0], mask[1], mask[2], mask[3]);
    for (GLuint u = 0; u < ctx->_units; u++) {
        if (!ctx->_boundName[u]) continue;
        gl.ActiveTexture(0x84c0 + u);
        GLint width = 0, height = 0, format = 0, min = 0, max = 0, swizzle[4] = {0};
        gl.GetTexLevelParameteriv(0x0de1, 0, 0x1000, &width);
        gl.GetTexLevelParameteriv(0x0de1, 0, 0x1001, &height);
        gl.GetTexLevelParameteriv(0x0de1, 0, 0x1003, &format);
        gl.GetTexParameteriv(0x0de1, 0x2801, &min);
        gl.GetTexParameteriv(0x0de1, 0x813d, &max);
        for (unsigned c = 0; c < 4; c++) gl.GetTexParameteriv(0x0de1, 0x8e42 + c, &swizzle[c]);
        T290SurfaceImage *img = ctx.sharegroup.textures[@(ctx->_boundName[u])];
        plog("TRACE unit %u tex %u surface %u %dx%d internal %#x min %#x max %d swizzle %x,%x,%x,%x",
            u, ctx->_boundName[u], img ? IOSurfaceGetID(img->source) : 0, width, height,
            format, min, max, swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
    }
    gl.ActiveTexture((GLenum)active);
    GLenum diagnostic_error = gl.GetError();
    if (diagnostic_error) plog("TRACE query error %#x", diagnostic_error);
}

static void t290_shader_source(GLuint shader, GLsizei count, const GLchar *const *strings,
    const GLint *lengths)
{
    gl.ShaderSource(shader, count, strings, lengths);
    if (!trace_render) return;
    static size_t total;
    for (GLsizei i = 0; i < count && total < 49152; i++) {
        size_t size = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        for (size_t offset = 0; offset < size && total < 49152; offset += 600) {
            size_t part = MIN((size_t)600, size - offset);
            plog("TRACE shader %u part %d offset %zu: %.*s", shader, i, offset,
                (int)part, strings[i] + offset);
            total += part;
        }
    }
}

static void t290_attach_shader(GLuint program, GLuint shader)
{
    gl.AttachShader(program, shader);
    if (trace_render) plog("TRACE program %u shader %u", program, shader);
}

static GLint t290_uniform_location(GLuint program, const GLchar *name)
{
    GLint location = gl.GetUniformLocation(program, name);
    static unsigned logged;
    if (trace_render && logged++ < 128)
        plog("TRACE uniform program %u %s location %d", program, name, location);
    return location;
}
static void t290_texture_write(BOOL redefine)
{
    T290Context *ctx = current_ctx();
    ctx->_gpuWork = YES;
    GLint name = 0;
    gl.GetIntegerv(0x8069, &name);
    if (redefine) {
        [ctx.sharegroup.textures removeObjectForKey:@(name)];
        for (NSNumber *fbo in ctx->_attachmentNames)
            if ([ctx->_attachmentNames[fbo] unsignedLongLongValue] == (GLuint)name)
                [ctx->_framebuffers removeObjectForKey:fbo];
        update_draw_image(ctx);
        if (ctx->_boundName[ctx->_unit] == (GLuint)name) ctx->_bound[ctx->_unit] = nil;
    }
    else {
        T290SurfaceImage *img = ctx.sharegroup.textures[@(name)];
        if (img && img->backing) {
            if (!img->gpuDirty) [ctx.sharegroup.dirty addObject:img];
            image_mark(img, 0, img->rows);
        }
    }
}
static void t290_after_call(unsigned slot, const char *name)
{
    GLenum err = gl.GetError();
    if (!err) return;
    T290Context *ctx = current_ctx();
    if (err >= 0x500 && err < 0x520) ctx->_errors |= 1u << (err - 0x500);
    static unsigned short counts[1100];
    unsigned n = ++counts[slot];
    if (n <= 4 || (n & (n - 1)) == 0)
        plog("t290eagl: GL error %#x after %s (slot %u count %u)", err, name, slot, n);
}
static GLenum t290_get_error(void)
{
    T290Context *ctx = current_ctx();
    if (ctx->_errors) {
        unsigned bit = __builtin_ctz(ctx->_errors);
        ctx->_errors &= ~(1u << bit);
        return 0x500 + bit;
    }
    return gl.GetError();
}
static void bind_unit(T290Context *ctx, GLuint name, T290SurfaceImage *img)
{
    GLuint u = ctx->_unit;
    ctx->_boundName[u] = name;
    ctx->_bound[u] = img && img->backing ? img : nil;
    if ((ctx->_bound[u] || (trace_render && name)) && u >= ctx->_units) ctx->_units = u + 1;
}
static void t290_bind_texture(GLenum target, GLuint name)
{
    gl.BindTexture(target, name);
    if (target != 0x0de1) return;
    T290Context *ctx = current_ctx();
    T290SurfaceImage *img = name ? ctx.sharegroup.textures[@(name)] : nil;
    bind_unit(ctx, name, img);
    if (img && image_stale(img)) {
        if (ctx->_gpuWork) t290_wait();
        [img upload];
    }
}
static void forget_images(T290Context *ctx, NSArray<T290SurfaceImage *> *gone)
{
    if (!gone.count) return;
    for (GLuint u = 0; u < ctx->_units; u++)
        if (ctx->_bound[u] && [gone containsObject:ctx->_bound[u]]) ctx->_bound[u] = nil;
}
static void t290_delete_textures(GLsizei n, const GLuint *names)
{
    T290Context *ctx = current_ctx();
    NSMutableArray<T290SurfaceImage *> *gone = [NSMutableArray array];
    BOOL dirty = NO;
    for (GLsizei i = 0; i < n; i++) {
        T290SurfaceImage *img = ctx.sharegroup.textures[@(names[i])];
        if (!img) continue;
        [gone addObject:img];
        dirty |= img->gpuDirty;
    }
    if (dirty) t290_finish();
    for (GLsizei i = 0; i < n; i++) {
        [ctx.sharegroup.textures removeObjectForKey:@(names[i])];
        [ctx.sharegroup.mipmapped removeIndex:names[i]];
        [ctx.sharegroup.mipLevels removeObjectForKey:@(names[i])];
    }
    forget_images(ctx, gone);
    gl.DeleteTextures(n, names);
}
static void t290_delete_renderbuffers(GLsizei n, const GLuint *names)
{
    T290Context *ctx = current_ctx();
    for (GLsizei i = 0; i < n; i++) {
        T290SurfaceImage *img = ctx.sharegroup.renderbuffers[@(names[i])];
        if (img && img->gpuDirty) { t290_finish(); break; }
    }
    for (GLsizei i = 0; i < n; i++) [ctx.sharegroup.renderbuffers removeObjectForKey:@(names[i])];
    gl.DeleteRenderbuffers(n, names);
}

/* Time spent inside GL calls (generated adapters), for the frame statistics. */
static inline void gl_timer_end(uint64_t *t0) { stats.glTicks += mach_absolute_time() - *t0; }
#define T290_GL_TIMER __attribute__((cleanup(gl_timer_end), unused)) uint64_t gl_t0 = mach_absolute_time()

/*
 * GLES samples a texture whose minification filter needs mipmaps it does not
 * have as black/transparent, without any GL error. CA's trilinear layers
 * (icons, wallpaper) hit this with single-level uploads and IOSurfaces, which
 * Apple's driver evidently tolerates. Clamp such textures to level 0 until
 * the client defines further levels or generates them.
 */
static void set_max_level(T290Context *ctx, GLenum target, GLint level)
{
    if (!ctx->_maxLevelState) {
        const char *ext = (const char *)gl.GetString(0x1f03);
        ctx->_maxLevelState = ctx.API >= 3 || (ext && strstr(ext, "GL_APPLE_texture_max_level")) ? 1 : 2;
    }
    if (ctx->_maxLevelState != 1) return;
    GLenum pending = gl.GetError();      /* keep the client's error for its glGetError */
    if (pending >= 0x500 && pending < 0x520) ctx->_errors |= 1u << (pending - 0x500);
    gl.TexParameteri(target, 0x813d, level);
    gl.GetError();
}
static void tex_level_defined(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h,
    GLenum uploadFormat, GLenum uploadType)
{
    if (target != 0x0de1 || level < 0 || level > 31 || w <= 0 || h <= 0) return;
    T290Context *ctx = current_ctx();
    GLint name = 0;
    gl.GetIntegerv(0x8069, &name);
    NSMutableDictionary *levels = ctx.sharegroup.mipLevels[@(name)];
    if (!levels) ctx.sharegroup.mipLevels[@(name)] = levels = [NSMutableDictionary dictionary];
    /* The same unsized RGBA internal format can select different Mesa storage
     * for RGBA and BGRA uploads. Their old mip levels are then incompatible.
     * Keep the upload signature, not just the caller's internal-format token. */
    NSArray *definition = @[@(internal), @(w), @(h), @(uploadFormat), @(uploadType)];
    if (!level && ![levels[@0] isEqual:definition]) [ctx.sharegroup.mipmapped removeIndex:name];
    levels[@(level)] = definition;
    if ([ctx.sharegroup.mipmapped containsIndex:name]) return;
    NSArray *base = levels[@0];
    if (!base) return;
    GLenum format = [base[0] unsignedIntValue];
    GLsizei width = [base[1] intValue], height = [base[2] intValue];
    GLint last = 0;
    /* CA can supply a partial mip chain. Restoring MAX_LEVEL=1000 as
     * soon as level 1 arrived made a missing final level turn the whole
     * texture black. Expose only contiguous levels of matching geometry. */
    while (width > 1 || height > 1) {
        width = MAX(1, width / 2);
        height = MAX(1, height / 2);
        NSArray *next = levels[@(last + 1)];
        if (!next || [next[0] unsignedIntValue] != format ||
            ![next[3] isEqual:base[3]] || ![next[4] isEqual:base[4]] ||
            [next[1] intValue] != width || [next[2] intValue] != height) break;
        last++;
    }
    set_max_level(ctx, target, last);
}
static void log_upload_format(const char *what, GLenum a, GLenum b, GLenum c)
{
    static uint32_t seen[48];
    static unsigned n;
    uint32_t key = (a & 0xffff) ^ (b << 16) ^ (c << 8);
    for (unsigned i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 48) {
        seen[n++] = key;
        plog("t290eagl: %s internal %#x format %#x type %#x", what, a, b, c);
    }
}
static void t290_tex_image2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
    GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    gl.TexImage2D(target, level, internal, w, h, border, format, type, pixels);
    tex_level_defined(target, level, (GLenum)internal, w, h, format, type);
    log_upload_format("texture upload", internal, format, type);
}
static void t290_compressed_tex_image2d(GLenum target, GLint level, GLenum internal, GLsizei w,
    GLsizei h, GLint border, GLsizei size, const GLvoid *data)
{
    current_ctx()->_gpuWork = YES;
    gl.CompressedTexImage2D(target, level, internal, w, h, border, size, data);
    tex_level_defined(target, level, internal, w, h, 0, 0);
    log_upload_format("compressed upload", internal, 0, 0);
}
static void t290_generate_mipmap(GLenum target)
{
    T290Context *ctx = current_ctx();
    ctx->_gpuWork = YES;
    if (target == 0x0de1) {
        GLint name = 0;
        gl.GetIntegerv(0x8069, &name);
        [ctx.sharegroup.mipmapped addIndex:name];
        set_max_level(ctx, target, 1000);
    }
    gl.GenerateMipmap(target);
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
        group.dirty = [NSMutableSet set];
        group.mipmapped = [NSMutableIndexSet indexSet];
        group.mipLevels = [NSMutableDictionary dictionary];
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
    if (planes) {
        /* 4:2:0 video/wallpaper surfaces ('420f'...): CA samples each plane
         * as its own texture and converts YCbCr in its shader. */
        uint32_t pf = format == 0x1903 || format == 0x1909 ? 0x52303038 :   /* R8 */
            format == 0x8227 ? 0x52473838 :                                  /* RG8 */
            format == 0x190a ? 0x4c413038 : 0;                               /* LA8 */
        static unsigned n;
        if (n++ < 16 || !pf)
            plog("t290eagl: plane %u of surface %u fmt %#x %dx%d stride %zu offset %zu: GL internal %#x "
                 "format %#x type %#x -> %#x", plane, IOSurfaceGetID(surface), fourcc, w, h, stride,
                 (size_t)offset, internal, format, type, pf);
        if (!pf) return NO;
        fourcc = pf;
    }
    GLint name = 0;
    gl.GetIntegerv(target == 0x8d41 ? 0x8ca7 : 0x8069, &name);
    if (name <= 0) return NO;
    NSMutableDictionary *images = target == 0x8d41 ? _sharegroup.renderbuffers : _sharegroup.textures;
    NSString *key = [NSString stringWithFormat:@"%u:%d:%d:%u:%u:%zu:%zu",
        IOSurfaceGetID(surface), w, h, fourcc, plane, stride, (size_t)offset];
    T290SurfaceImage *mirror = [_sharegroup.surfaces objectForKey:key];
    if (!mirror) {
        mirror = [T290SurfaceImage new];
        mirror->source = (IOSurfaceRef)CFRetain(surface);
    }
    /* Freedreno's import pitch: 64 pixels (cpp 4: 256 bytes, LA08: 128). */
    size_t bpe = fourcc == 0x4c413038 || fourcc == 0x52473838 ? 2 :
        (fourcc == 0x4c303038 || fourcc == 0x41303038 || fourcc == 0x52303038) ? 1 :
        (fourcc == 0x42475241 || fourcc == 0x52474241) ? 4 : 0;
    if (!mirror->image && bpe && (stride % (64 * bpe))) {
        size_t height = planes ? IOSurfaceGetHeightOfPlane(surface, plane) : IOSurfaceGetHeight(surface);
        if ((size_t)w > SIZE_MAX / bpe || (size_t)h > height ||
            (size_t)w * bpe > stride || !IOSurfaceGetBaseAddress(surface)) return NO;
        mirror->rows = h;
        mirror->rowBytes = (size_t)w * bpe;
        mirror->sourceStride = stride;
        mirror->sourceOffset = offset;
        mirror->backing = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (__bridge id)kIOSurfaceWidth: @(w), (__bridge id)kIOSurfaceHeight: @(h),
            (__bridge id)kIOSurfaceBytesPerElement: @(bpe),
            (__bridge id)kIOSurfaceBytesPerRow: @((mirror->rowBytes + 255) & ~(size_t)255),
            (__bridge id)kIOSurfacePixelFormat: @(fourcc), (__bridge id)kIOSurfaceIsGlobal: @YES
        });
        if (!mirror->backing) return NO;
        mirror->backingStride = IOSurfaceGetBytesPerRow(mirror->backing);
        if ((mirror->backingStride % (64 * bpe)) || !IOSurfaceGetBaseAddress(mirror->backing)) return NO;
        mirror->seed = IOSurfaceGetSeed(surface) + 1;    /* first upload below */
        static unsigned count;
        if (count++ < 24) plog("t290eagl: padded surface %u %dx%d fmt %#x stride %zu -> %zu",
            IOSurfaceGetID(surface), w, h, fourcc, stride, mirror->backingStride);
    }
    if (image_stale(mirror)) {
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
            plog("t290eagl: import FAILED id %u %dx%d stride %zu offset %zu fourcc %#x (surface %#x) plane %u",
                IOSurfaceGetID(surface), w, h, stride, (size_t)offset, fourcc,
                (unsigned)IOSurfaceGetPixelFormat(surface), plane);
            return NO;
        }
        [_sharegroup.surfaces setObject:mirror forKey:key];
    }
    if (target == 0x8d41) gl.EGLImageTargetRenderbufferStorageOES(target, mirror->image);
    else {
        gl.EGLImageTargetTexture2DOES(target, mirror->image);
        [_sharegroup.mipmapped removeIndex:name];
        _sharegroup.mipLevels[@(name)] = [NSMutableDictionary dictionaryWithObject:
            @[@(internal), @(w), @(h), @(format), @(type)] forKey:@0];
        set_max_level(self, target, 0);     /* an IOSurface has no mipmaps */
    }
    images[@(name)] = mirror;
    /* the scanout surfaces: surface 1 and CA's other full-screen buffer */
    if ((w >= 800 && h >= 1280) || IOSurfaceGetID(surface) == 1) mirror->scanout = YES;
    if (IOSurfaceGetID(surface) == 1) _sharegroup.display = mirror;
    uint64_t attachmentName = (uint64_t)(GLuint)name | (target == 0x8d41 ? 0x100000000ull : 0);
    for (NSNumber *fbo in _attachmentNames)
        if ([_attachmentNames[fbo] unsignedLongLongValue] == attachmentName)
            _framebuffers[fbo] = mirror;
    update_draw_image(self);
    if (target == 0x0de1) bind_unit(self, (GLuint)name, mirror);
    static unsigned count;
    if (count++ < 12) plog("t290eagl: imported surface %u %dx%d target %#x invert %d",
        IOSurfaceGetID(surface), w, h, target, invert);
    return YES;
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
    return thread_current == (__bridge void *)ctx;
}
static id get_current(id cls, SEL cmd) { return (__bridge id)thread_current; }
static void replace(Class c, const char *name, IMP fn, BOOL meta)
{
    Method m = class_getInstanceMethod(meta ? object_getClass(c) : c, sel_registerName(name));
    if (m) method_setImplementation(m, fn);
    else plog("t290eagl: no method %s", name);
}

__attribute__((constructor)) static void bridge_init(void)
{
    if (!strcmp(getprogname(), "backboardd")) {
        int trace_fd = open("/private/var/tmp/t290eagl.trace", O_RDONLY);
        if (trace_fd >= 0) {
            char enabled = 0;
            trace_render = read(trace_fd, &enabled, 1) == 1 && enabled == '1';
            close(trace_fd);
        }
    }
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
#define LOAD(f) (gl.f = (__typeof__(gl.f))mesa_proc("gl" #f))
    if (!LOAD(GetError) || !LOAD(GetIntegerv) || !LOAD(Finish) || !LOAD(Flush) || !LOAD(IsShader) ||
        !LOAD(DeleteShader) || !LOAD(DeleteProgram) || !LOAD(GetShaderiv) || !LOAD(GetProgramiv) ||
        !LOAD(GetShaderInfoLog) || !LOAD(GetProgramInfoLog) || !LOAD(GetString) ||
        !LOAD(BindFramebuffer) || !LOAD(FramebufferTexture2D) || !LOAD(FramebufferRenderbuffer) ||
        !LOAD(DeleteFramebuffers) || !LOAD(BindTexture) || !LOAD(DeleteTextures) ||
        !LOAD(DeleteRenderbuffers) || !LOAD(ActiveTexture) || !LOAD(Enable) || !LOAD(Disable) ||
        !LOAD(Scissor) || !LOAD(Viewport) || !LOAD(EGLImageTargetTexture2DOES) ||
        !LOAD(EGLImageTargetRenderbufferStorageOES) || !LOAD(TexParameteri) || !LOAD(GenerateMipmap) ||
        !LOAD(TexImage2D) || !LOAD(CompressedTexImage2D) || !LOAD(ShaderSource) ||
        !LOAD(GetTexParameteriv) || !LOAD(AttachShader) || !LOAD(GetUniformLocation)) {
        plog("t290eagl: incompatible Mesa: GL entry point missing");
        mesa_create = NULL;
        return;
    }
    LOAD(GetTexLevelParameteriv);  /* optional, diagnostic ES3 query */
#undef LOAD
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
