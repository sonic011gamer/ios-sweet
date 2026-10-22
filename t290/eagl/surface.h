#pragma once
#include <CoreFoundation/CoreFoundation.h>
#if __has_include(<IOSurface/IOSurface.h>)
#include <IOSurface/IOSurface.h>
#else
/* The iOS SDK links IOSurface but does not ship its C headers. */
typedef struct __IOSurface *IOSurfaceRef;
IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
uint32_t IOSurfaceGetID(IOSurfaceRef surface);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef surface);
void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef surface, size_t plane);
size_t IOSurfaceGetWidth(IOSurfaceRef surface);
size_t IOSurfaceGetHeight(IOSurfaceRef surface);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef surface, size_t plane);
extern const CFStringRef kIOSurfaceWidth, kIOSurfaceHeight;
extern const CFStringRef kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow;
extern const CFStringRef kIOSurfacePixelFormat, kIOSurfaceIsGlobal;
#endif
