/*
 * Freedreno "sweet" backend, device: one SweetGPU user client connection.
 */
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "sweet_priv.h"

/* SWEET_FAKE=1: no GPU, buffers in process memory, submits done at once */
#define FAKE_MAX_BO 16384
static struct {
   pthread_mutex_t lock;
   void *ptr[FAKE_MAX_BO];
   uint64_t size[FAKE_MAX_BO];
   bool external[FAKE_MAX_BO];   /* imported IOSurface: not ours to unmap */
   uint64_t next_va;
   uint32_t seq;
} fake = { PTHREAD_MUTEX_INITIALIZER, {0}, {0}, {0}, 0x10000000, 0 };

/* IOSurface.framework (fake imports use the host's real surfaces) */
void *IOSurfaceLookup(uint32_t id);
void *IOSurfaceGetBaseAddress(void *surface);
size_t IOSurfaceGetAllocSize(void *surface);

static unsigned
fake_slot(void)
{
   unsigned h;
   for (h = 1; h < FAKE_MAX_BO && fake.ptr[h]; h++)
      ;
   return h;
}

static kern_return_t
fake_call(uint32_t sel, const uint64_t *in, uint64_t *out, uint32_t *nout)
{
   kern_return_t kr = KERN_SUCCESS;
   pthread_mutex_lock(&fake.lock);
   switch (sel) {
   case SWEET_GPU_INFO:
      out[0] = 0x05000400;
      out[1] = 0x22000;
      out[2] = 1;
      out[3] = 0x10000000;
      *nout = 4;
      break;
   case SWEET_GPU_ALLOC: {
      uint64_t size = (in[0] + 4095) & ~4095ull;
      unsigned h = fake_slot();
      void *p = h < FAKE_MAX_BO ? mmap(NULL, size, PROT_READ | PROT_WRITE,
                                       MAP_ANON | MAP_PRIVATE, -1, 0) : MAP_FAILED;
      if (p == MAP_FAILED) {
         kr = KERN_NO_SPACE;
         break;
      }
      fake.ptr[h] = p;
      fake.size[h] = size;
      out[0] = h;
      out[1] = fake.next_va;
      out[2] = size;
      *nout = 3;
      fake.next_va += size;
      break;
   }
   case SWEET_GPU_FREE:
      if (in[0] < FAKE_MAX_BO && fake.ptr[in[0]]) {
         if (!fake.external[in[0]])
            munmap(fake.ptr[in[0]], fake.size[in[0]]);
         fake.ptr[in[0]] = NULL;
         fake.external[in[0]] = false;
      }
      break;
   case SWEET_GPU_IMPORT_SURFACE: {
      void *s = IOSurfaceLookup((uint32_t)in[0]);
      unsigned h = fake_slot();
      if (!s || h >= FAKE_MAX_BO) {
         kr = KERN_INVALID_ARGUMENT;
         break;
      }
      uint64_t size = (IOSurfaceGetAllocSize(s) + 4095) & ~4095ull;
      fake.ptr[h] = IOSurfaceGetBaseAddress(s);
      fake.size[h] = size;
      fake.external[h] = true;
      out[0] = h;
      out[1] = fake.next_va;
      out[2] = size;
      *nout = 3;
      fake.next_va += size;
      break;
   }
   case SWEET_GPU_SUBMIT:
      out[0] = ++fake.seq;
      *nout = 1;
      break;
   case SWEET_GPU_WAIT:
      break;
   case SWEET_GPU_FENCE:
      out[0] = fake.seq;
      *nout = 1;
      break;
   default:
      kr = KERN_INVALID_ARGUMENT;
   }
   pthread_mutex_unlock(&fake.lock);
   return kr;
}

kern_return_t
sweet_call(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out,
           uint32_t *nout)
{
   if (c == SWEET_FAKE_CONN)
      return fake_call(sel, in, out, nout);
   return IOConnectCallScalarMethod(c, sel, in, nin, out, nout);
}

kern_return_t
sweet_map(io_connect_t c, uint32_t handle, mach_vm_address_t *addr)
{
   if (c == SWEET_FAKE_CONN) {
      if (handle >= FAKE_MAX_BO || !fake.ptr[handle])
         return KERN_INVALID_ARGUMENT;
      *addr = (mach_vm_address_t)(uintptr_t)fake.ptr[handle];
      return KERN_SUCCESS;
   }
   mach_vm_size_t size = 0;
   return IOConnectMapMemory64(c, handle, mach_task_self(), addr, &size, 1 /* kIOMapAnywhere */);
}

void
sweet_unmap(io_connect_t c, uint32_t handle, mach_vm_address_t addr)
{
   if (c != SWEET_FAKE_CONN)
      IOConnectUnmapMemory64(c, handle, mach_task_self(), addr);
}

static void sweet_close(io_connect_t conn);

static void
sweet_device_destroy(struct fd_device *dev)
{
   struct sweet_device *sd = to_sweet_device(dev);
   if (sd->conn)
      sweet_close(sd->conn);
}

static const struct fd_device_funcs funcs = {
   .bo_new = sweet_bo_new,
   .bo_from_handle = sweet_bo_from_handle,
   .bo_close_handle = sweet_bo_close_handle,
   .pipe_new = sweet_pipe_new,
   .destroy = sweet_device_destroy,
};

int
sweet_wait_seq(struct fd_device *dev, uint32_t seq, uint64_t timeout_ns)
{
   struct sweet_device *sd = to_sweet_device(dev);
   uint64_t ms = timeout_ns / 1000000;
   uint64_t in[2] = { seq, ms > 0xffffffff ? 0xffffffff : (ms ? ms : 1) };
   kern_return_t kr = sweet_call(sd->conn, SWEET_GPU_WAIT, in, 2, NULL, NULL);
   return kr ? -ETIMEDOUT : 0;
}

static io_connect_t
sweet_open(void)
{
   const char *fake_env = getenv("SWEET_FAKE");
   if (fake_env && *fake_env == '1')
      return SWEET_FAKE_CONN;
   io_service_t svc =
      IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
   if (!svc) {
      ERROR_MSG("no SweetGPU service");
      return 0;
   }
   io_connect_t conn = 0;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   IOObjectRelease(svc);
   if (kr) {
      ERROR_MSG("SweetGPU open failed: %#x", kr);
      return 0;
   }
   return conn;
}

static void
sweet_close(io_connect_t conn)
{
   if (conn != SWEET_FAKE_CONN)
      IOServiceClose(conn);
}

struct fd_device *
sweet_device_new(void)
{
   io_connect_t conn = sweet_open();
   if (!conn)
      return NULL;
   uint64_t out[4] = {0};
   uint32_t n = 4;
   kern_return_t kr = sweet_call(conn, SWEET_GPU_INFO, NULL, 0, out, &n);
   if (kr || !out[2]) {
      ERROR_MSG("SweetGPU not ready: %#x", kr);
      sweet_close(conn);
      return NULL;
   }

   struct sweet_device *sd = calloc(1, sizeof(*sd));
   if (!sd) {
      sweet_close(conn);
      return NULL;
   }
   struct fd_device *dev = &sd->base;
   dev->funcs = &funcs;
   /* softpin submits, submit queues; no fault counters (ROBUSTNESS) */
   dev->version = FD_VERSION_SOFTPIN;
   dev->bo_size = sizeof(struct sweet_bo);
   sd->conn = conn;
   sd->chip_id = out[0];
   sd->gmem_size = (uint32_t)out[1];
   INFO_MSG("SweetGPU: chip %#" PRIx64 ", gmem %#x", sd->chip_id, sd->gmem_size);
   return dev;
}
