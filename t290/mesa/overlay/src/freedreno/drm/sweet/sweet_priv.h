/*
 * Freedreno "sweet" backend: the SweetGPU IOKit user client of the SM-T290
 * iPadOS port (xnu iokit/Kernel/arm/SweetGPU.cpp) instead of the msm DRM
 * driver. Buffers are SweetGPU allocations (handle, GPU address, mapping
 * via IOConnectMapMemory64); a submit runs each command buffer as an
 * indirect buffer from the kernel ring, the last one's fence is the
 * submit's kernel fence.
 */
#ifndef SWEET_PRIV_H_
#define SWEET_PRIV_H_

#include <mach/mach.h>

#include "freedreno_priv.h"

/* IOKit, without the (absent) iOS SDK headers */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
struct __CFDictionary;
struct __CFDictionary *IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, const struct __CFDictionary *matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin,
                                        uint64_t *out, uint32_t *nout);
kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task,
                                   mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_port_t task,
                                     mach_vm_address_t addr);

/* SweetGPU selectors */
enum {
   SWEET_GPU_INFO = 0,   /* -> chip id, gmem size, ready, window base */
   SWEET_GPU_ALLOC = 1,  /* size -> handle, gpu address, size */
   SWEET_GPU_FREE = 2,   /* handle */
   SWEET_GPU_SUBMIT = 3, /* ib address, dwords -> fence seq */
   SWEET_GPU_WAIT = 4,   /* seq, timeout ms */
   SWEET_GPU_FENCE = 5,  /* -> last completed seq */
   SWEET_GPU_IMPORT_SURFACE = 6, /* IOSurface id -> handle, gpu address, size */
   SWEET_GPU_LOG = 7,    /* struct in: text for the kernel log */
};

/* fd_bo_from_handle(): an IOSurface id, not a SweetGPU buffer handle */
#define SWEET_IOSURFACE_HANDLE 0x40000000u

struct sweet_device {
   struct fd_device base;
   io_connect_t conn;
   uint64_t chip_id;
   uint32_t gmem_size;
   uint32_t last_seq;    /* last submitted kernel fence */
};
FD_DEFINE_CAST(fd_device, sweet_device);

struct sweet_pipe {
   struct fd_pipe base;
};
FD_DEFINE_CAST(fd_pipe, sweet_pipe);

struct sweet_bo {
   struct fd_bo base;
   uint64_t gpuaddr;
};
FD_DEFINE_CAST(fd_bo, sweet_bo);

/*
 * SweetGPU calls go through these: with SWEET_FAKE=1 in the environment
 * there is no IOKit service, a fake device answers instead (buffers in
 * process memory, submits complete at once) so the CPU side of Mesa can
 * run and be debugged on a Mac.
 */
#define SWEET_FAKE_CONN ((io_connect_t)0x5eed)
kern_return_t sweet_call(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin,
                         uint64_t *out, uint32_t *nout);
kern_return_t sweet_map(io_connect_t c, uint32_t handle, mach_vm_address_t *addr);
void sweet_unmap(io_connect_t c, uint32_t handle, mach_vm_address_t addr);

struct fd_device *sweet_device_new(void);
struct fd_bo *sweet_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags);
struct fd_bo *sweet_bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle);
void sweet_bo_close_handle(struct fd_bo *bo);
struct fd_pipe *sweet_pipe_new(struct fd_device *dev, enum fd_pipe_id id, uint32_t prio);
int sweet_wait_seq(struct fd_device *dev, uint32_t seq, uint64_t timeout_ns);

#endif /* SWEET_PRIV_H_ */
