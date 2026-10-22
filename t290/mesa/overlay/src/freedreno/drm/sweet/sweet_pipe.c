/*
 * Freedreno "sweet" backend, pipe + submit: softpin ringbuffers; each
 * command buffer of a submit becomes one SweetGPU indirect-buffer submit,
 * the last one's fence is the submit's kernel fence.
 */
#include "freedreno_ringbuffer_sp.h"
#include "sweet_priv.h"

static int
sweet_pipe_get_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t *value)
{
   struct sweet_device *sd = to_sweet_device(pipe->dev);
   switch (param) {
   case FD_DEVICE_ID:
   case FD_GPU_ID:
      *value = 504;
      return 0;
   case FD_GMEM_SIZE:
      *value = sd->gmem_size;
      return 0;
   case FD_GMEM_BASE:
      *value = 0x100000;
      return 0;
   case FD_CHIP_ID:
      *value = sd->chip_id;
      return 0;
   case FD_MAX_FREQ:
      *value = 200000000;
      return 0;
   case FD_NR_PRIORITIES:
      *value = 1;
      return 0;
   case FD_CTX_FAULTS:
   case FD_GLOBAL_FAULTS:
   case FD_SUSPEND_COUNT:
      *value = 0;
      return 0;
   case FD_VA_SIZE:
      *value = 0x70000000;
      return 0;
   default:
      return -1;                 /* FD_TIMESTAMP: no always-on counter read yet */
   }
}

static int
sweet_pipe_set_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t value)
{
   return -1;
}

static int
sweet_pipe_wait(struct fd_pipe *pipe, const struct fd_fence *fence, uint64_t timeout)
{
   return sweet_wait_seq(pipe->dev, fence->kfence, timeout);
}

static void
sweet_pipe_destroy(struct fd_pipe *pipe)
{
   fd_pipe_sp_ringpool_fini(pipe);
   free(to_sweet_pipe(pipe));
}

/*
 * Imported IOSurfaces are CPU-cached memory the GPU does not snoop. Tell the
 * kernel which buffers this submit uses so it cleans/invalidates only those
 * (kernel 0133); older kernels flush every imported surface on each call.
 */
static void
prepare_bos(struct sweet_device *sd, struct list_head *submit_list)
{
   uint32_t handles[1024];
   unsigned n = 0;

   if (sd->no_prepare || sd->conn == SWEET_FAKE_CONN)
      return;
   foreach_submit (submit, submit_list) {
      struct fd_submit_sp *s = to_fd_submit_sp(submit);
      for (unsigned i = 0; i < s->nr_bos; i++) {
         if (n == ARRAY_SIZE(handles)) {
            if (IOConnectCallStructMethod(sd->conn, SWEET_GPU_PREPARE, handles,
                                          n * 4, NULL, NULL)) {
               sd->no_prepare = true;
               return;
            }
            n = 0;
         }
         handles[n++] = s->bos[i]->handle;
      }
   }
   if (n && IOConnectCallStructMethod(sd->conn, SWEET_GPU_PREPARE, handles, n * 4, NULL, NULL))
      sd->no_prepare = true;
}

static int
flush_submit_list(struct list_head *submit_list)
{
   struct fd_submit_sp *fd_submit = to_fd_submit_sp(last_submit(submit_list));
   struct fd_pipe *pipe = fd_submit->base.pipe;
   struct sweet_device *sd = to_sweet_device(pipe->dev);
   uint64_t seq = 0;
   int ret = 0;

   prepare_bos(sd, submit_list);

   foreach_submit_safe (submit, submit_list) {
      struct fd_ringbuffer_sp *primary = to_fd_ringbuffer_sp(submit->primary);

      for (unsigned i = 0; i < primary->u.nr_cmds && !ret; i++) {
         struct fd_bo *ring_bo = primary->u.cmds[i].ring_bo;
         uint64_t in[2] = {
            fd_bo_get_iova(ring_bo) + primary->offset,
            primary->u.cmds[i].size / 4,
         };
         uint32_t n = 1;
         kern_return_t kr =
            sweet_call(sd->conn, SWEET_GPU_SUBMIT, in, 2, &seq, &n);
         if (kr) {
            ERROR_MSG("submit failed: %#x", kr);
            ret = -EIO;
         }
      }

      if (submit == last_submit(submit_list))
         break;

      /* merge the deferred submit's bos into the last one, then drop it */
      struct fd_submit_sp *deferred = to_fd_submit_sp(submit);
      for (unsigned i = 0; i < deferred->nr_bos; i++)
         fd_submit_append_bo(fd_submit, deferred->bos[i]);
      list_del(&submit->node);
      fd_submit_del(submit);
   }

   if (!ret) {
      fd_submit->out_fence->kfence = (uint32_t)seq;
      p_atomic_set(&sd->last_seq, (uint32_t)seq);
   }
   if (fd_submit->in_fence_fd != -1)
      close(fd_submit->in_fence_fd);
   return ret;
}

static struct fd_submit *
sweet_submit_new(struct fd_pipe *pipe)
{
   return fd_submit_sp_new(pipe, flush_submit_list);
}

static const struct fd_pipe_funcs funcs = {
   .ringbuffer_new_object = fd_ringbuffer_sp_new_object,
   .submit_new = sweet_submit_new,
   .flush = fd_pipe_sp_flush,
   .get_param = sweet_pipe_get_param,
   .set_param = sweet_pipe_set_param,
   .wait = sweet_pipe_wait,
   .destroy = sweet_pipe_destroy,
};

struct fd_pipe *
sweet_pipe_new(struct fd_device *dev, enum fd_pipe_id id, uint32_t prio)
{
   struct sweet_pipe *sp = calloc(1, sizeof(*sp));
   if (!sp)
      return NULL;
   struct fd_pipe *pipe = &sp->base;
   pipe->funcs = &funcs;
   pipe->dev = dev;
   fd_pipe_sp_ringpool_init(pipe);
   return pipe;
}
