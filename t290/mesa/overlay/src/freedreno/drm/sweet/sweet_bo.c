/*
 * Freedreno "sweet" backend, buffers: SweetGPU allocations (write-combined,
 * mapped into the GPU's user window by the kernel).
 */
#include "sweet_priv.h"

static int
sweet_bo_offset(struct fd_bo *bo, uint64_t *offset)
{
   *offset = 0;
   return 0;
}

static void *
sweet_bo_map(struct fd_bo *bo)
{
   struct sweet_device *sd = to_sweet_device(bo->dev);
   mach_vm_address_t addr = 0;
   mach_vm_size_t size = 0;
   kern_return_t kr = IOConnectMapMemory64(sd->conn, bo->handle, mach_task_self(), &addr, &size,
                                           1 /* kIOMapAnywhere */);
   if (kr) {
      ERROR_MSG("map of bo %u failed: %#x", bo->handle, kr);
      return NULL;
   }
   return (void *)(uintptr_t)addr;
}

/*
 * No per-bo busy tracking in the kernel yet: wait for everything submitted
 * so far (correct, conservative). NOSYNC only asks whether it is busy.
 */
static int
sweet_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   struct sweet_device *sd = to_sweet_device(bo->dev);
   uint32_t seq = p_atomic_read(&sd->last_seq);
   if (!seq)
      return 0;
   if (op & FD_BO_PREP_NOSYNC) {
      uint64_t done = 0;
      uint32_t n = 1;
      IOConnectCallScalarMethod(sd->conn, SWEET_GPU_FENCE, NULL, 0, &done, &n);
      return fd_fence_before((uint32_t)done, seq) ? -EBUSY : 0;
   }
   return sweet_wait_seq(bo->dev, seq, 5000000000ull);
}

static int
sweet_bo_madvise(struct fd_bo *bo, int willneed)
{
   return willneed;
}

static uint64_t
sweet_bo_iova(struct fd_bo *bo)
{
   return to_sweet_bo(bo)->gpuaddr;
}

static void
sweet_bo_set_name(struct fd_bo *bo, const char *fmt, va_list ap)
{
}

static void
sweet_bo_destroy(struct fd_bo *bo)
{
   struct sweet_device *sd = to_sweet_device(bo->dev);
   if (bo->map) {
      IOConnectUnmapMemory64(sd->conn, bo->handle, mach_task_self(),
                             (mach_vm_address_t)(uintptr_t)bo->map);
      bo->map = NULL;
   }
   fd_bo_fini_common(bo);
}

static const struct fd_bo_funcs funcs = {
   .offset = sweet_bo_offset,
   .map = sweet_bo_map,
   .cpu_prep = sweet_bo_cpu_prep,
   .madvise = sweet_bo_madvise,
   .iova = sweet_bo_iova,
   .set_name = sweet_bo_set_name,
   .destroy = sweet_bo_destroy,
};

void
sweet_bo_close_handle(struct fd_bo *bo)
{
   struct sweet_device *sd = to_sweet_device(bo->dev);
   uint64_t h = bo->handle;
   IOConnectCallScalarMethod(sd->conn, SWEET_GPU_FREE, &h, 1, NULL, NULL);
}

static struct fd_bo *
bo_wrap(struct fd_device *dev, uint32_t size, uint32_t handle, uint64_t gpuaddr)
{
   struct sweet_bo *sbo = calloc(1, sizeof(*sbo));
   if (!sbo)
      return NULL;
   struct fd_bo *bo = &sbo->base;
   bo->size = size;
   bo->handle = handle;
   bo->funcs = &funcs;
   sbo->gpuaddr = gpuaddr;
   fd_bo_init_common(bo, dev);
   return bo;
}

struct fd_bo *
sweet_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   struct sweet_device *sd = to_sweet_device(dev);
   uint64_t in = size, out[3] = {0};
   uint32_t n = 3;
   kern_return_t kr = IOConnectCallScalarMethod(sd->conn, SWEET_GPU_ALLOC, &in, 1, out, &n);
   if (kr) {
      ERROR_MSG("alloc of %u bytes failed: %#x", size, kr);
      return NULL;
   }
   return bo_wrap(dev, size, (uint32_t)out[0], out[1]);
}

/* no import between processes yet */
struct fd_bo *
sweet_bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   return NULL;
}
