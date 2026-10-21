/*
 * Freedreno "sweet" backend, device: one SweetGPU user client connection.
 */
#include "sweet_priv.h"

static void
sweet_device_destroy(struct fd_device *dev)
{
   struct sweet_device *sd = to_sweet_device(dev);
   if (sd->conn)
      IOServiceClose(sd->conn);
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
   kern_return_t kr = IOConnectCallScalarMethod(sd->conn, SWEET_GPU_WAIT, in, 2, NULL, NULL);
   return kr ? -ETIMEDOUT : 0;
}

struct fd_device *
sweet_device_new(void)
{
   io_service_t svc =
      IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetGPU"));
   if (!svc) {
      ERROR_MSG("no SweetGPU service");
      return NULL;
   }
   io_connect_t conn = 0;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   IOObjectRelease(svc);
   if (kr) {
      ERROR_MSG("SweetGPU open failed: %#x", kr);
      return NULL;
   }
   uint64_t out[4] = {0};
   uint32_t n = 4;
   kr = IOConnectCallScalarMethod(conn, SWEET_GPU_INFO, NULL, 0, out, &n);
   if (kr || !out[2]) {
      ERROR_MSG("SweetGPU not ready: %#x", kr);
      IOServiceClose(conn);
      return NULL;
   }

   struct sweet_device *sd = calloc(1, sizeof(*sd));
   if (!sd) {
      IOServiceClose(conn);
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
