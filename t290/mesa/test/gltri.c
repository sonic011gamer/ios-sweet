/*
 * gltri: first OpenGL on the SM-T290 GPU from iPadOS. OSMesa (t290/mesa,
 * patch 0002) renders with Freedreno on SweetGPU: clear to dark blue, one
 * RGB triangle, read back, check pixels, time 100 frames, save a PPM.
 * Output: stdout (/dev/console via launchd) and /private/var/tmp/gltri.ppm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#include <GL/osmesa.h>
#include <GL/gl.h>

#define W 256
#define H 256

static double
ms(uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static void
draw(float spin)
{
	glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glLoadIdentity();
	glRotatef(spin, 0, 0, 1);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0);
	glVertex2f(-0.8f, -0.8f);
	glColor3f(0, 1, 0);
	glVertex2f(0.8f, -0.8f);
	glColor3f(0, 0, 1);
	glVertex2f(0.0f, 0.8f);
	glEnd();
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("gltri: start\n");
	uint64_t t0 = mach_absolute_time();
	OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	if (!ctx) {
		printf("gltri: OSMesaCreateContextExt failed\n");
		return 1;
	}
	unsigned char *buf = calloc(W * H, 4);
	if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
		printf("gltri: OSMesaMakeCurrent failed\n");
		return 1;
	}
	printf("gltri: context in %.1f ms, GL_RENDERER \"%s\", GL_VERSION \"%s\"\n", ms(t0),
	    (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

	glViewport(0, 0, W, H);
	t0 = mach_absolute_time();
	draw(0);
	glFinish();
	printf("gltri: first frame %.2f ms, gl error %#x\n", ms(t0), glGetError());

	/* OSMesa rows are bottom-up; corner = clear colour, centre = triangle */
	unsigned char *c = buf, *m = buf + ((H / 2) * W + W / 2) * 4;
	printf("gltri: corner %u,%u,%u  centre %u,%u,%u -> %s\n", c[0], c[1], c[2], m[0], m[1], m[2],
	    c[2] > 100 && c[0] < 30 && (m[0] | m[1] | m[2]) && !(m[0] < 30 && m[1] < 30 && m[2] > 100) ?
	    "TRIANGLE DRAWN" : "unexpected");

	t0 = mach_absolute_time();
	for (int i = 0; i < 100; i++) {
		draw((float)i * 3.6f);
		glFinish();
	}
	double t = ms(t0);
	printf("gltri: 100 frames %.1f ms (%.1f fps at %dx%d)\n", t, 100000.0 / t, W, H);

	FILE *f = fopen("/private/var/tmp/gltri.ppm", "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (int y = H - 1; y >= 0; y--)
			for (int x = 0; x < W; x++)
				fwrite(buf + (y * W + x) * 4, 1, 3, f);
		fclose(f);
		printf("gltri: saved /private/var/tmp/gltri.ppm\n");
	}
	OSMesaDestroyContext(ctx);
	return 0;
}
