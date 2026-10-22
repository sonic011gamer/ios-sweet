/* Exercise the actual EAGL object and GLI ABI; no Apple GL C entry points. */
#import <Foundation/Foundation.h>
#include "surface.h"
#include <dlfcn.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "gl_types.h"

@interface NSObject (EAGLTest)
- (id)initWithAPI:(unsigned)api;
- (id)initWithAPI:(unsigned)api group:(id)group;
+ (BOOL)setCurrentContext:(id)context;
- (void *)getMacroContextPrivate;
- (BOOL)attachImage:(GLenum)target toCoreSurface:(IOSurfaceRef)s invertedRender:(BOOL)inverted;
@end

struct Macro { void *renderer; void *slots[1100]; };
#define CALL(slot, ret, types, ...) ((ret (*) types)m->slots[slot])(m->renderer, ##__VA_ARGS__)

static int tlog(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int tlog(const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    fputs(buf, stdout);
    void (*report)(const char *) = dlsym(RTLD_DEFAULT, "T290EAGLLog");
    if (report) report(buf);
    return n;
}
#define printf tlog

/* Actual failing layouts from the kernel-0130 compositor log. Exercise
 * output copies through Apple's object fence, not just glFinish. */
static int test_row_layouts(id ctx, struct Macro *m)
{
    const unsigned widths[] = {120, 800};
    const unsigned heights[] = {120, 1280};
    const uint32_t formats[] = {0, 0x42475241};
    for (unsigned c = 0; c < 2; c++) {
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (__bridge id)kIOSurfaceWidth: @(widths[c]), (__bridge id)kIOSurfaceHeight: @(heights[c]),
            (__bridge id)kIOSurfaceBytesPerElement: @4,
            (__bridge id)kIOSurfaceBytesPerRow: @(widths[c] * 4),
            (__bridge id)kIOSurfacePixelFormat: @(formats[c]), (__bridge id)kIOSurfaceIsGlobal: @YES
        });
        if (!s) return 20;
        size_t stride = IOSurfaceGetBytesPerRow(s);
        memset(IOSurfaceGetBaseAddress(s), 0x5a, stride * heights[c]);
        GLuint tex = 0, fbo = 0;
        CALL(98, void, (void *, GLsizei, GLuint *), 1, &tex);
        CALL(5, void, (void *, GLenum, GLuint), 0x0de1, tex);
        if (![ctx attachImage:0x0de1 toCoreSurface:s invertedRender:NO]) return 21;
        CALL(674, void, (void *, GLsizei, GLuint *), 1, &fbo);
        CALL(672, void, (void *, GLenum, GLuint), 0x8d40, fbo);
        CALL(677, void, (void *, GLenum, GLenum, GLenum, GLuint, GLint), 0x8d40, 0x8ce0, 0x0de1, tex, 0);
        if (CALL(675, GLenum, (void *, GLenum), 0x8d40) != 0x8cd5) return 22;
        CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 0.25f, 0.5f, 0.75f, 1.0f);
        CALL(10, void, (void *, GLbitfield), 0x4000);
        CALL(470, void, (void *, GLenum, GLuint), 0x1702, tex);
        if (CALL(102, GLenum, (void *))) return 23;
        const uint8_t *p = IOSurfaceGetBaseAddress(s);
        if (!getenv("SWEET_FAKE")) {
            for (size_t y = 0; y < heights[c]; y++) {
                for (size_t x = 0; x < widths[c]; x++) {
                    const uint8_t *q = p + y * stride + x * 4;
                    if (abs(q[0] - (c ? 191 : 64)) > 1 || abs(q[1] - 128) > 1 ||
                        abs(q[2] - (c ? 64 : 191)) > 1 || q[3] != 255) return 24;
                }
                /* Row tails survive only the bridge's padded copy. A GPU-aligned
                 * surface (kernel 0133) is rendered in place: GMEM resolves
                 * whole tiles, so its tail bytes are scratch, as on Apple's GPUs. */
                if (stride % 256)
                    for (size_t x = widths[c] * 4; x < stride; x++) if (p[y * stride + x] != 0x5a) return 25;
            }
        }
        printf("PASS output %ux%u fmt %#x stride %zu, object fence and all rows\n",
            widths[c], heights[c], formats[c], stride);
        /* Two texture names must share the same IOSurface storage. The
         * old per-texture shadows returned red/old pixels through tex1
         * after tex2 rendered into a separate shadow of the same surface. */
        GLuint alias = 0;
        CALL(98, void, (void *, GLsizei, GLuint *), 1, &alias);
        CALL(5, void, (void *, GLenum, GLuint), 0x0de1, alias);
        if (![ctx attachImage:0x0de1 toCoreSurface:s invertedRender:NO]) return 26;
        CALL(677, void, (void *, GLenum, GLenum, GLenum, GLuint, GLint), 0x8d40, 0x8ce0, 0x0de1, alias, 0);
        CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 0, 1, 0, 1);
        CALL(10, void, (void *, GLbitfield), 0x4000);
        CALL(470, void, (void *, GLenum, GLuint), 0x1702, tex);
        if (CALL(102, GLenum, (void *))) return 27;
        if (!getenv("SWEET_FAKE")) {
            const uint8_t *p = IOSurfaceGetBaseAddress(s);
            for (size_t y = 0; y < heights[c]; y++) for (size_t x = 0; x < widths[c]; x++) {
                const uint8_t *q = p + y * stride + x * 4;
                if (q[0] || q[1] != 255 || q[2] || q[3] != 255) return 28;
            }
        }
        printf("PASS shared IOSurface storage across two texture names\n");
        /* Scissored redraw (CA's damage rectangle): rows 10-19 change, the
         * rest must keep the earlier green after the object fence. */
        CALL(72, void, (void *, GLenum), 0x0c11);
        CALL(251, void, (void *, GLint, GLint, GLsizei, GLsizei), 0, 10, (GLsizei)widths[c], 10);
        CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 1, 0, 0, 1);
        CALL(10, void, (void *, GLbitfield), 0x4000);
        CALL(63, void, (void *, GLenum), 0x0c11);
        CALL(470, void, (void *, GLenum, GLuint), 0x1702, tex);
        if (CALL(102, GLenum, (void *))) return 29;
        if (!getenv("SWEET_FAKE")) {
            const uint8_t *p = IOSurfaceGetBaseAddress(s);
            for (size_t y = 0; y < heights[c]; y++) {
                BOOL red = y >= 10 && y < 20;
                const uint8_t *q = p + y * stride + (widths[c] / 2) * 4;
                unsigned r = c ? q[2] : q[0], g = q[1], b = c ? q[0] : q[2];
                if (r != (red ? 255 : 0) || g != (red ? 0 : 255) || b || q[3] != 255) return 19;
            }
        }
        printf("PASS scissored redraw copies its rows only\n");
        CALL(59, void, (void *, GLsizei, const GLuint *), 1, &alias);
        CALL(673, void, (void *, GLsizei, const GLuint *), 1, &fbo);
        CALL(59, void, (void *, GLsizei, const GLuint *), 1, &tex);
        CFRelease(s);
    }
    return 0;
}

static int test_luminance_alpha(id ctx, struct Macro *m, GLuint targetFbo, unsigned api)
{
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (__bridge id)kIOSurfaceWidth: @228, (__bridge id)kIOSurfaceHeight: @162,
        (__bridge id)kIOSurfaceBytesPerElement: @2, (__bridge id)kIOSurfaceBytesPerRow: @480,
        (__bridge id)kIOSurfacePixelFormat: @0x4c413038, (__bridge id)kIOSurfaceIsGlobal: @YES
    });
    if (!s) return 30;
    GLuint tex = 0;
    CALL(98, void, (void *, GLsizei, GLuint *), 1, &tex);
    CALL(5, void, (void *, GLenum, GLuint), 0x0de1, tex);
    if (![ctx attachImage:0x0de1 toCoreSurface:s invertedRender:NO]) return 31;
    /* trilinear on a single-level IOSurface texture, as CA does for icons */
    CALL(304, void, (void *, GLenum, GLenum, GLint), 0x0de1, 0x2801, 0x2703);
    CALL(304, void, (void *, GLenum, GLenum, GLint), 0x0de1, 0x2800, 0x2600);
    const char *sources[] = {
        "attribute vec2 position; varying vec2 uv; void main(){uv=position*0.5+0.5;gl_Position=vec4(position,0.0,1.0);}",
        "precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}"
    };
    GLuint shaders[2], program = CALL(597, GLuint, (void *));
    for (unsigned j = 0; j < 2; j++) {
        shaders[j] = CALL(594, GLuint, (void *, GLenum), j ? 0x8b30 : 0x8b31);
        CALL(595, void, (void *, GLuint, GLsizei, const char *const *, const GLint *), shaders[j], 1, &sources[j], NULL);
        CALL(596, void, (void *, GLuint), shaders[j]);
        GLint ok = 0;
        CALL(622, void, (void *, GLuint, GLenum, GLint *), shaders[j], 0x8b81, &ok);
        if (!ok) return 32;
        CALL(598, void, (void *, GLuint, GLuint), program, shaders[j]);
    }
    CALL(630, void, (void *, GLuint, GLuint, const char *), program, 0, "position");
    CALL(599, void, (void *, GLuint), program);
    GLint ok = 0;
    CALL(622, void, (void *, GLuint, GLenum, GLint *), program, 0x8b82, &ok);
    if (!ok) return 33;
    CALL(600, void, (void *, GLuint), program);
    CALL(672, void, (void *, GLenum, GLuint), 0x8d40, targetFbo);
    CALL(335, void, (void *, GLint, GLint, GLsizei, GLsizei), 0, 0, 64, 64);
    for (unsigned round = 0; round < 2; round++) {
        if (IOSurfaceLock(s, 0, NULL)) return 36;
        uint8_t *p = IOSurfaceGetBaseAddress(s);
        size_t stride = IOSurfaceGetBytesPerRow(s);
        int lum = round ? 63 : 127, alpha = round ? 128 : 255;
        for (size_t y = 0; y < 162; y++) for (size_t x = 0; x < 228; x++) {
            p[y * stride + x * 2] = lum;
            p[y * stride + x * 2 + 1] = alpha;
        }
        if (IOSurfaceUnlock(s, 0, NULL)) return 37;
        /* Round 1 keeps the texture bound, as CA does: the new CPU pixels
         * must still reach the padded GPU copy before the draw. */
        if (!round) CALL(5, void, (void *, GLenum, GLuint), 0x0de1, tex);
        CALL(65, void, (void *, GLenum, GLint, GLsizei), 4, 0, 3);
        CALL(90, void, (void *));
        uint8_t pixel[4] = {0};
        CALL(237, void, (void *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *), 32, 32, 1, 1, 0x1908, 0x1401, pixel);
        GLenum err = CALL(102, GLenum, (void *));
        printf("LA08 round %u stride %zu sample %u,%u,%u,%u error %#x\n",
            round, stride, pixel[0], pixel[1], pixel[2], pixel[3], err);
        if (err) return 34;
        if (!getenv("SWEET_FAKE") && (abs(pixel[0] - lum) > 1 || abs(pixel[1] - lum) > 1 ||
            abs(pixel[2] - lum) > 1 || abs(pixel[3] - alpha) > 1)) return 35;
    }
    /* Formats actually seen in backboardd: alpha glyphs, BGRA images,
     * half-float intermediate layers and ASTC icons. Exercise both direct
     * sampling and alpha composition; a successful import alone is not
     * evidence that text, colors and transparency are correct. */
    const struct {
        const char *name;
        GLenum internal, format, type;
        unsigned cpp;
        uint8_t value[8], expected[4];
        BOOL straightAlpha;
    } cases[] = {
        {"RGBA icon", 0x1908, 0x1908, 0x1401, 4, {128, 0, 0, 128}, {128, 0, 0, 128}, NO},
        {"BGRA icon", 0x1908, 0x80e1, 0x1401, 4, {0, 0, 128, 128}, {128, 0, 0, 128}, NO},
        {"A8 glyph", 0x1906, 0x1906, 0x1401, 1, {128}, {255, 255, 255, 128}, YES},
        {"L8 glyph", 0x1909, 0x1909, 0x1401, 1, {96}, {96, 96, 96, 255}, NO},
        {"RGBA16F layer", 0x881a, 0x1908, 0x140b, 8,
            {0x00, 0x38, 0x00, 0x34, 0x00, 0x3c, 0x00, 0x38}, {128, 64, 255, 128}, NO},
        {"ASTC icon", 0x93b0, 0, 0, 0, {0}, {128, 0, 0, 128}, NO},
    };
    GLuint upload = 0;
    CALL(98, void, (void *, GLsizei, GLuint *), 1, &upload);
    CALL(5, void, (void *, GLenum, GLuint), 0x0de1, upload);
    CALL(195, void, (void *, GLenum, GLint), 0x0cf5, 1); /* unpack alignment */
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (cases[c].type == 0x140b && api < 3) continue;
        /* Reuse the same texture after the RGBA partial chain. BGRA may
         * choose different Mesa storage despite the same unsized internal
         * format; old RGBA mip levels must not make it incomplete. */
        if (cases[c].cpp) {
            uint8_t data[16 * 8];
            for (unsigned p = 0; p < 16; p++) memcpy(data + p * cases[c].cpp, cases[c].value, cases[c].cpp);
            CALL(301, void, (void *, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *),
                0x0de1, 0, cases[c].internal, 4, 4, 0, cases[c].format, cases[c].type, data);
        } else {
            /* ASTC LDR void-extent block, constant RGBA16 UNORM. */
            const uint8_t block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0x80, 0x80, 0, 0, 0, 0, 0x80, 0x80};
            CALL(380, void, (void *, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *),
                0x0de1, 0, cases[c].internal, 4, 4, 0, sizeof(block), block);
        }
        /* Default mipmapped filter on a one-level icon must remain usable. */
        CALL(304, void, (void *, GLenum, GLenum, GLint), 0x0de1, 0x2801, 0x2703);
        CALL(304, void, (void *, GLenum, GLenum, GLint), 0x0de1, 0x2800, 0x2600);
        for (unsigned blend = 0; blend < 2; blend++) {
            CALL(63, void, (void *, GLenum), 0x0be2);
            CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 0, 0, 1, 1);
            CALL(10, void, (void *, GLbitfield), 0x4000);
            if (blend) {
                CALL(72, void, (void *, GLenum), 0x0be2);
                CALL(336, void, (void *, GLenum, GLenum, GLenum, GLenum),
                    cases[c].straightAlpha ? 0x0302 : 1, 0x0303, 1, 0x0303);
            }
            CALL(65, void, (void *, GLenum, GLint, GLsizei), 4, 0, 3);
            uint8_t pixel[4] = {0};
            CALL(237, void, (void *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *),
                32, 32, 1, 1, 0x1908, 0x1401, pixel);
            GLenum err = CALL(102, GLenum, (void *));
            printf("format %s blend %u sample %u,%u,%u,%u error %#x\n",
                cases[c].name, blend, pixel[0], pixel[1], pixel[2], pixel[3], err);
            if (err) return 40 + c;
            if (!getenv("SWEET_FAKE")) for (unsigned k = 0; k < 4; k++) {
                int expected = cases[c].expected[k];
                if (blend) {
                    if (k == 3) expected = 255;
                    else {
                        if (cases[c].straightAlpha) expected = (expected * cases[c].expected[3] + 127) / 255;
                        if (k == 2) expected = MIN(255, expected + 255 - cases[c].expected[3]);
                    }
                }
                if (abs(pixel[k] - expected) > 2) return 50 + c;
            }
        }
        if (!c) {
            uint8_t mip[4 * 4];
            for (unsigned p = 0; p < 4; p++) memcpy(mip + p * 4, cases[c].value, 4);
            CALL(301, void, (void *, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *),
                0x0de1, 1, 0x1908, 2, 2, 0, 0x1908, 0x1401, mip);
            CALL(63, void, (void *, GLenum), 0x0be2);
            CALL(65, void, (void *, GLenum, GLint, GLsizei), 4, 0, 3);
            uint8_t pixel[4] = {0};
            CALL(237, void, (void *, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *),
                32, 32, 1, 1, 0x1908, 0x1401, pixel);
            GLenum err = CALL(102, GLenum, (void *));
            printf("partial mip chain 4x4 + 2x2 (no 1x1) sample %u,%u,%u,%u error %#x\n",
                pixel[0], pixel[1], pixel[2], pixel[3], err);
            if (err) return 60;
            if (!getenv("SWEET_FAKE")) for (unsigned k = 0; k < 4; k++)
                if (abs(pixel[k] - cases[c].expected[k]) > 2) return 61;
        }
    }
    CALL(63, void, (void *, GLenum), 0x0be2);
    CALL(195, void, (void *, GLenum, GLint), 0x0cf5, 4);
    CALL(59, void, (void *, GLsizei, const GLuint *), 1, &upload);
    CALL(600, void, (void *, GLuint), 0);
    CALL(591, void, (void *, GLuint), program);
    for (unsigned j = 0; j < 2; j++) CALL(591, void, (void *, GLuint), shaders[j]);
    CALL(59, void, (void *, GLsizei, const GLuint *), 1, &tex);
    CFRelease(s);
    return 0;
}

static int test_main(void)
{
    @autoreleasepool {
#if TARGET_OS_IPHONE
        /* Regression: the unimplemented kernel selector used to return a
         * seemingly valid client with null shared state, crashing CA. */
        extern IOSurfaceRef IOSurfaceWrapClientImage(uint32_t, uint32_t,
            uint32_t, uint32_t, uint32_t, void *);
        uint8_t image[64 * 64 * 4] = {0};
        IOSurfaceRef wrapped = IOSurfaceWrapClientImage(64, 64, 0x42475241, 256, 0, image);
        if (wrapped) {
            printf("FAIL: client-image interposition did not take effect\n");
            CFRelease(wrapped);
            return 12;
        }
        printf("PASS: unsupported client-image wrapping uses GL uploads\n");
#endif
        /* macOS has no EAGLContext: exercise the same implementation directly. */
        Class cls = NSClassFromString(@"EAGLContext") ?: NSClassFromString(@"T290Context");
        for (unsigned api = 2; api <= 3; api++) {
            id ctx;
            if ([NSStringFromClass(cls) isEqualToString:@"T290Context"])
                ctx = [[cls alloc] initWithAPI:api group:nil];
            else ctx = [[cls alloc] initWithAPI:api];
            if (!ctx) { fprintf(stderr, "FAIL ES%u context\n", api); return 1; }
            struct Macro *m = [ctx getMacroContextPrivate];
            const char *renderer = (const char *)CALL(117, const GLubyte *, (void *, GLenum), 0x1f01);
            const char *version = (const char *)CALL(117, const GLubyte *, (void *, GLenum), 0x1f02);
            printf("ES%u renderer %s version %s\n", api, renderer, version);
            if (!renderer || !strstr(renderer, "FD504") || !strstr(version, "OpenGL ES")) return 2;
            IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{
                (__bridge id)kIOSurfaceWidth: @64, (__bridge id)kIOSurfaceHeight: @64,
                (__bridge id)kIOSurfaceBytesPerElement: @4, (__bridge id)kIOSurfaceBytesPerRow: @256,
                (__bridge id)kIOSurfacePixelFormat: @0x42475241, (__bridge id)kIOSurfaceIsGlobal: @YES
            });
            if (!surf) return 3;
            GLuint tex = 0, fbo = 0;
            CALL(98, void, (void *, GLsizei, GLuint *), 1, &tex);
            CALL(5, void, (void *, GLenum, GLuint), 0x0de1, tex);
            if (![ctx attachImage:0x0de1 toCoreSurface:surf invertedRender:NO]) return 4;
            CALL(674, void, (void *, GLsizei, GLuint *), 1, &fbo);
            CALL(672, void, (void *, GLenum, GLuint), 0x8d40, fbo);
            CALL(677, void, (void *, GLenum, GLenum, GLenum, GLuint, GLint), 0x8d40, 0x8ce0, 0x0de1, tex, 0);
            GLenum status = CALL(675, GLenum, (void *, GLenum), 0x8d40);
            printf("FBO status %#x\n", status);
            if (status != 0x8cd5) return 5;
            CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 0.25f, 0.5f, 0.75f, 1.0f);
            CALL(10, void, (void *, GLbitfield), 0x4000);
            CALL(89, void, (void *));
            GLenum err = CALL(102, GLenum, (void *));
            uint8_t *pixel = IOSurfaceGetBaseAddress(surf);
            printf("clear GL error %#x, IOSurface BGRA %u,%u,%u,%u\n", err, pixel[0], pixel[1], pixel[2], pixel[3]);
            if (err) return 6;
            /* SWEET_FAKE validates command generation only; no pixels execute. */
            if (!getenv("SWEET_FAKE") &&
                (abs(pixel[0] - 191) > 1 || abs(pixel[1] - 128) > 1 || abs(pixel[2] - 64) > 1 || pixel[3] != 255)) return 7;
            const char *sources[] = {
                "attribute vec2 position; void main(){gl_Position=vec4(position,0.0,1.0);}",
                "precision mediump float; void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}"
            };
            GLuint shaders[2];
            for (unsigned j = 0; j < 2; j++) {
                shaders[j] = CALL(594, GLuint, (void *, GLenum), j ? 0x8b30 : 0x8b31);
                CALL(595, void, (void *, GLuint, GLsizei, const char *const *, const GLint *), shaders[j], 1, &sources[j], NULL);
                CALL(596, void, (void *, GLuint), shaders[j]);
                GLint ok = 0;
                CALL(622, void, (void *, GLuint, GLenum, GLint *), shaders[j], 0x8b81, &ok);
                if (!ok) {
                    char log[1024] = {0};
                    CALL(623, void, (void *, GLuint, GLsizei, GLsizei *, char *), shaders[j], sizeof(log), NULL, log);
                    fprintf(stderr, "FAIL shader %u: %s\n", j, log);
                    return 8;
                }
            }
            GLuint program = CALL(597, GLuint, (void *));
            for (unsigned j = 0; j < 2; j++) CALL(598, void, (void *, GLuint, GLuint), program, shaders[j]);
            CALL(630, void, (void *, GLuint, GLuint, const char *), program, 0, "position");
            CALL(599, void, (void *, GLuint), program);
            GLint linked = 0;
            CALL(622, void, (void *, GLuint, GLenum, GLint *), program, 0x8b82, &linked);
            if (!linked) return 9;
            CALL(600, void, (void *, GLuint), program);
            GLuint vbo = 0;
            const GLfloat vertices[] = {-1,-1, 1,-1, 0,1};
            CALL(644, void, (void *, GLsizei, GLuint *), 1, &vbo);
            CALL(642, void, (void *, GLenum, GLuint), 0x8892, vbo);
            CALL(646, void, (void *, GLenum, GLsizeiptr, const void *, GLenum), 0x8892, sizeof(vertices), vertices, 0x88e4);
            CALL(511, void, (void *, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *), 0, 2, 0x1406, 0, 0, NULL);
            CALL(512, void, (void *, GLuint), 0);
            CALL(335, void, (void *, GLint, GLint, GLsizei, GLsizei), 0, 0, 64, 64);
            CALL(65, void, (void *, GLenum, GLint, GLsizei), 4, 0, 3);
            CALL(89, void, (void *));
            err = CALL(102, GLenum, (void *));
            pixel += 32 * 256 + 32 * 4;
            printf("triangle GL error %#x, centre BGRA %u,%u,%u,%u\n", err, pixel[0], pixel[1], pixel[2], pixel[3]);
            if (err) return 10;
            if (!getenv("SWEET_FAKE") && (pixel[0] || pixel[1] || pixel[2] != 255 || pixel[3] != 255)) return 11;
            int layouts = test_row_layouts(ctx, m);
            if (layouts) return layouts;
            int la = test_luminance_alpha(ctx, m, fbo, api);
            if (la) return la;
            CALL(643, void, (void *, GLsizei, const GLuint *), 1, &vbo);
            CALL(600, void, (void *, GLuint), 0);
            CALL(591, void, (void *, GLuint), program);
            for (unsigned j = 0; j < 2; j++) CALL(591, void, (void *, GLuint), shaders[j]);
            CALL(673, void, (void *, GLsizei, const GLuint *), 1, &fbo);
            CALL(59, void, (void *, GLsizei, const GLuint *), 1, &tex);
            CFRelease(surf);
        }
        printf("PASS EAGL ES2/ES3, GLI dispatch, IOSurface framebuffer and shaders%s\n",
            getenv("SWEET_FAKE") ? " (fake GPU; pixels not verified)" : " and GPU pixels");
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int result = test_main();
    printf("EAGL test exit %d\n", result);
    return result;
}
