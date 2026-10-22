/* Exercise the actual EAGL object and GLI ABI; no Apple GL C entry points. */
#import <Foundation/Foundation.h>
#include "surface.h"
#include <dlfcn.h>
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

static int test_main(void)
{
    @autoreleasepool {
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
