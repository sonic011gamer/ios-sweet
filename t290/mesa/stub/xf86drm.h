/*
 * libdrm stand-in for the iPadOS build of Mesa Freedreno (t290/mesa).
 * Darwin has no DRM: the msm backend is compiled but never used (the sweet
 * backend talks to the SweetGPU IOKit user client). Every call fails.
 */
#ifndef T290_XF86DRM_STUB_H
#define T290_XF86DRM_STUB_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "drm-uapi/drm.h"

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

static inline drmVersionPtr drmGetVersion(int fd) { (void)fd; return NULL; }
static inline void drmFreeVersion(drmVersionPtr v) { (void)v; }
static inline int drmIoctl(int fd, unsigned long req, void *arg) { (void)fd; (void)req; (void)arg; errno = ENODEV; return -1; }
static inline int drmCommandWrite(int fd, unsigned long i, void *d, unsigned long n) { (void)fd; (void)i; (void)d; (void)n; return -ENODEV; }
static inline int drmCommandRead(int fd, unsigned long i, void *d, unsigned long n) { (void)fd; (void)i; (void)d; (void)n; return -ENODEV; }
static inline int drmCommandWriteRead(int fd, unsigned long i, void *d, unsigned long n) { (void)fd; (void)i; (void)d; (void)n; return -ENODEV; }
static inline int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *pfd) { (void)fd; (void)h; (void)f; (void)pfd; return -ENODEV; }
static inline int drmPrimeFDToHandle(int fd, int pfd, uint32_t *h) { (void)fd; (void)pfd; (void)h; return -ENODEV; }
static inline int drmSyncobjCreate(int fd, uint32_t f, uint32_t *h) { (void)fd; (void)f; (void)h; return -ENODEV; }
static inline int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENODEV; }
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#endif
