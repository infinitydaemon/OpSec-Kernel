/*
 * Copyright © 2008 Jérôme Glisse
 * Copyright © 2010 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "amdgpu_cs.h"
#include "util/detect_os.h"
#include "amdgpu_winsys.h"
#include "util/os_time.h"
#include <inttypes.h>
#include <stdio.h>

#include "amd/common/sid.h"

/* Some BSDs don't define ENODATA (and ENODATA is replaced with different error
 * codes in the kernel).
 */
#if DETECT_OS_OPENBSD
#define ENODATA ENOTSUP
#elif DETECT_OS_FREEBSD || DETECT_OS_DRAGONFLY
#define ENODATA ECONNREFUSED
#endif

/* FENCES */

void amdgpu_fence_destroy(struct amdgpu_fence *fence)
{
   ac_drm_cs_destroy_syncobj(fence->aws->fd, fence->syncobj);

   if (fence->ctx)
      amdgpu_ctx_reference(&fence->ctx, NULL);

   util_queue_fence_destroy(&fence->submitted);
   FREE(fence);
}

static struct pipe_fence_handle *
amdgpu_fence_create(struct amdgpu_cs *acs)
{
   struct amdgpu_fence *fence = CALLOC_STRUCT(amdgpu_fence);
   struct amdgpu_ctx *ctx = acs->ctx;

   fence->reference.count = 1;
   fence->aws = ctx->aws;
   amdgpu_ctx_reference(&fence->ctx, ctx);
   fence->ctx = ctx;
   fence->ip_type = acs->ip_type;
   if (ac_drm_cs_create_syncobj2(ctx->aws->fd, 0, &fence->syncobj)) {
      free(fence);
      return NULL;
   }

   util_queue_fence_init(&fence->submitted);
   util_queue_fence_reset(&fence->submitted);
   fence->queue_index = acs->queue_index;
   return (struct pipe_fence_handle *)fence;
}

static struct pipe_fence_handle *
amdgpu_fence_import_syncobj(struct radeon_winsys *rws, int fd)
{
   struct amdgpu_winsys *aws = amdgpu_winsys(rws);
   struct amdgpu_fence *fence = CALLOC_STRUCT(amdgpu_fence);
   int r;

   if (!fence)
      return NULL;

   pipe_reference_init(&fence->reference, 1);
   fence->aws = aws;
   fence->ip_type = 0xffffffff;

   r = ac_drm_cs_import_syncobj(aws->fd, fd, &fence->syncobj);
   if (r) {
      FREE(fence);
      return NULL;
   }

   util_queue_fence_init(&fence->submitted);
   fence->imported = true;

   return (struct pipe_fence_handle*)fence;
}

static struct pipe_fence_handle *
amdgpu_fence_import_sync_file(struct radeon_winsys *rws, int fd)
{
   struct amdgpu_winsys *aws = amdgpu_winsys(rws);
   struct amdgpu_fence *fence = CALLOC_STRUCT(amdgpu_fence);

   if (!fence)
      return NULL;

   pipe_reference_init(&fence->reference, 1);
   fence->aws = aws;
   /* fence->ctx == NULL means that the fence is syncobj-based. */

   /* Convert sync_file into syncobj. */
   int r = ac_drm_cs_create_syncobj(aws->fd, &fence->syncobj);
   if (r) {
      FREE(fence);
      return NULL;
   }

   r = ac_drm_cs_syncobj_import_sync_file(aws->fd, fence->syncobj, fd);
   if (r) {
      ac_drm_cs_destroy_syncobj(aws->fd, fence->syncobj);
      FREE(fence);
      return NULL;
   }

   util_queue_fence_init(&fence->submitted);
   fence->imported = true;

   return (struct pipe_fence_handle*)fence;
}

static int amdgpu_fence_export_sync_file(struct radeon_winsys *rws,
                                         struct pipe_fence_handle *pfence)
{
   struct amdgpu_winsys *aws = amdgpu_winsys(rws);
   struct amdgpu_fence *fence = (struct amdgpu_fence*)pfence;
   int fd, r;

   util_queue_fence_wait(&fence->submitted);

   /* Convert syncobj into sync_file. */
   r = ac_drm_cs_syncobj_export_sync_file(aws->fd, fence->syncobj, &fd);
   return r ? -1 : fd;
}

static int amdgpu_export_signalled_sync_file(struct radeon_winsys *rws)
{
   struct amdgpu_winsys *aws = amdgpu_winsys(rws);
   uint32_t syncobj;
   int fd = -1;

   int r = ac_drm_cs_create_syncobj2(aws->fd, DRM_SYNCOBJ_CREATE_SIGNALED,
                                     &syncobj);
   if (r) {
      return -1;
   }

   r = ac_drm_cs_syncobj_export_sync_file(aws->fd, syncobj, &fd);
   if (r) {
      fd = -1;
   }

   ac_drm_cs_destroy_syncobj(aws->fd, syncobj);
   return fd;
}

static void amdgpu_fence_submitted(struct pipe_fence_handle *fence,
                                   uint64_t seq_no,
                                   uint64_t *user_fence_cpu_address)
{
   struct amdgpu_fence *afence = (struct amdgpu_fence*)fence;

   afence->seq_no = seq_no;
   afence->user_fence_cpu_address = user_fence_cpu_address;
   util_queue_fence_signal(&afence->submitted);
}

static void amdgpu_fence_signalled(struct pipe_fence_handle *fence)
{
   struct amdgpu_fence *afence = (struct amdgpu_fence*)fence;

   afence->signalled = true;
   util_queue_fence_signal(&afence->submitted);
}

bool amdgpu_fence_wait(struct pipe_fence_handle *fence, uint64_t timeout,
                       bool absolute)
{
   struct amdgpu_fence *afence = (struct amdgpu_fence*)fence;
   int64_t abs_timeout;
   uint64_t *user_fence_cpu;

   if (afence->signalled)
      return true;

   if (absolute)
      abs_timeout = timeout;
   else
      abs_timeout = os_time_get_absolute_timeout(timeout);

   /* The fence might not have a number assigned if its IB is being
    * submitted in the other thread right now. Wait until the submission
    * is done. */
   if (!util_queue_fence_wait_timeout(&afence->submitted, abs_timeout))
      return false;

   user_fence_cpu = afence->user_fence_cpu_address;
   if (user_fence_cpu) {
      if (*user_fence_cpu >= afence->seq_no) {
         afence->signalled = true;
         return true;
      }

      /* No timeout, just query: no need for the ioctl. */
      if (!absolute && !timeout)
         return false;
   }

   if ((uint64_t)abs_timeout == OS_TIMEOUT_INFINITE)
      abs_timeout = INT64_MAX;

   if (ac_drm_cs_syncobj_wait(afence->aws->fd, &afence->syncobj, 1,
                              abs_timeout, 0, NULL))
      return false;

   /* Check that guest-side syncobj agrees with the user fence. */
   if (user_fence_cpu && afence->aws->info.is_virtio)
      assert(afence->seq_no <= *user_fence_cpu);

   afence->signalled = true;
   return true;
}

static bool amdgpu_fence_wait_rel_timeout(struct radeon_winsys *rws,
                                          struct pipe_fence_handle *fence,
                                          uint64_t timeout)
{
   return amdgpu_fence_wait(fence, timeout, false);
}

static struct pipe_fence_handle *
amdgpu_cs_get_next_fence(struct radeon_cmdbuf *rcs)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct pipe_fence_handle *fence = NULL;

   if (acs->noop)
      return NULL;

   if (acs->next_fence) {
      amdgpu_fence_reference(&fence, acs->next_fence);
      return fence;
   }

   fence = amdgpu_fence_create(acs);
   if (!fence)
      return NULL;

   amdgpu_fence_reference(&acs->next_fence, fence);
   return fence;
}

/* CONTEXTS */

static uint32_t
radeon_to_amdgpu_priority(enum radeon_ctx_priority radeon_priority)
{
   switch (radeon_priority) {
   case RADEON_CTX_PRIORITY_REALTIME:
      return AMDGPU_CTX_PRIORITY_VERY_HIGH;
   case RADEON_CTX_PRIORITY_HIGH:
      return AMDGPU_CTX_PRIORITY_HIGH;
   case RADEON_CTX_PRIORITY_MEDIUM:
      return AMDGPU_CTX_PRIORITY_NORMAL;
   case RADEON_CTX_PRIORITY_LOW:
      return AMDGPU_CTX_PRIORITY_LOW;
   default:
      unreachable("Invalid context priority");
   }
}

static struct radeon_winsys_ctx *amdgpu_ctx_create(struct radeon_winsys *rws,
                                                   enum radeon_ctx_priority priority,
                                                   bool allow_context_lost)
{
   struct amdgpu_ctx *ctx = CALLOC_STRUCT(amdgpu_ctx);
   int r;
   struct amdgpu_bo_alloc_request alloc_buffer = {};
   uint32_t amdgpu_priority = radeon_to_amdgpu_priority(priority);
   ac_drm_device *dev;
   ac_drm_bo buf_handle;

   if (!ctx)
      return NULL;

   ctx->aws = amdgpu_winsys(rws);
   ctx->reference.count = 1;
   ctx->allow_context_lost = allow_context_lost;

   dev = ctx->aws->dev;

   r = ac_drm_cs_ctx_create2(dev, amdgpu_priority, &ctx->ctx_handle);
   if (r) {
      fprintf(stderr, "amdgpu: amdgpu_cs_ctx_create2 failed. (%i)\n", r);
      goto error_create;
   }

   alloc_buffer.alloc_size = ctx->aws->info.gart_page_size;
   alloc_buffer.phys_alignment = ctx->aws->info.gart_page_size;
   alloc_buffer.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

   r = ac_drm_bo_alloc(dev, &alloc_buffer, &buf_handle);
   if (r) {
      fprintf(stderr, "amdgpu: amdgpu_bo_alloc failed. (%i)\n", r);
      goto error_user_fence_alloc;
   }

   ctx->user_fence_cpu_address_base = NULL;
   r = ac_drm_bo_cpu_map(dev, buf_handle, (void**)&ctx->user_fence_cpu_address_base);
   if (r) {
      fprintf(stderr, "amdgpu: amdgpu_bo_cpu_map failed. (%i)\n", r);
      goto error_user_fence_map;
   }

   memset(ctx->user_fence_cpu_address_base, 0, alloc_buffer.alloc_size);
   ctx->user_fence_bo = buf_handle;
   ac_drm_bo_export(dev, buf_handle, amdgpu_bo_handle_type_kms, &ctx->user_fence_bo_kms_handle);

   return (struct radeon_winsys_ctx*)ctx;

error_user_fence_map:
   ac_drm_bo_free(dev, buf_handle);

error_user_fence_alloc:
   ac_drm_cs_ctx_free(dev, ctx->ctx_handle);
error_create:
   FREE(ctx);
   return NULL;
}

static void amdgpu_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct amdgpu_ctx *ctx = (struct amdgpu_ctx*)rwctx;

   amdgpu_ctx_reference(&ctx, NULL);
}

static void amdgpu_pad_gfx_compute_ib(struct amdgpu_winsys *aws, enum amd_ip_type ip_type,
                                      uint32_t *ib, uint32_t *num_dw, unsigned leave_dw_space)
{
   unsigned pad_dw_mask = aws->info.ip[ip_type].ib_pad_dw_mask;
   unsigned unaligned_dw = (*num_dw + leave_dw_space) & pad_dw_mask;

   if (unaligned_dw) {
      int remaining = pad_dw_mask + 1 - unaligned_dw;

      /* Only pad by 1 dword with the type-2 NOP if necessary. */
      if (remaining == 1 && aws->info.gfx_ib_pad_with_type2) {
         ib[(*num_dw)++] = PKT2_NOP_PAD;
      } else {
         /* Pad with a single NOP packet to minimize CP overhead because NOP is a variable-sized
          * packet. The size of the packet body after the header is always count + 1.
          * If count == -1, there is no packet body. NOP is the only packet that can have
          * count == -1, which is the definition of PKT3_NOP_PAD (count == 0x3fff means -1).
          */
         ib[(*num_dw)++] = PKT3(PKT3_NOP, remaining - 2, 0);
         *num_dw += remaining - 1;
      }
   }
   assert(((*num_dw + leave_dw_space) & pad_dw_mask) == 0);
}

static int amdgpu_submit_gfx_nop(struct amdgpu_ctx *ctx)
{
   struct amdgpu_bo_alloc_request request = {0};
   struct drm_amdgpu_bo_list_in bo_list_in;
   struct drm_amdgpu_cs_chunk_ib ib_in = {0};
   ac_drm_bo bo;
   amdgpu_va_handle va_handle = NULL;
   struct drm_amdgpu_cs_chunk chunks[2];
   struct drm_amdgpu_bo_list_entry list;
   unsigned noop_dw_size;
   void *cpu = NULL;
   uint64_t seq_no;
   uint64_t va;
   int r;

   /* Older amdgpu doesn't report if the reset is complete or not. Detect
    * it by submitting a no-op job. If it reports an error, then assume
    * that the reset is not complete.
    */
   uint32_t temp_ctx_handle;
   r = ac_drm_cs_ctx_create2(ctx->aws->dev, AMDGPU_CTX_PRIORITY_NORMAL, &temp_ctx_handle);
   if (r)
      return r;

   request.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
   request.alloc_size = 4096;
   request.phys_alignment = 4096;
   r = ac_drm_bo_alloc(ctx->aws->dev, &request, &bo);
   if (r)
      goto destroy_ctx;

   r = ac_drm_va_range_alloc(ctx->aws->dev, amdgpu_gpu_va_range_general,
                             request.alloc_size, request.phys_alignment,
                             0, &va, &va_handle,
                             AMDGPU_VA_RANGE_32_BIT | AMDGPU_VA_RANGE_HIGH);
   if (r)
      goto destroy_bo;

   uint32_t kms_handle;
   ac_drm_bo_export(ctx->aws->dev, bo, amdgpu_bo_handle_type_kms, &kms_handle);

   r = ac_drm_bo_va_op_raw(ctx->aws->dev, kms_handle, 0, request.alloc_size, va,
                           AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
                           AMDGPU_VA_OP_MAP);
   if (r)
      goto destroy_bo;

   r = ac_drm_bo_cpu_map(ctx->aws->dev, bo, &cpu);
   if (r)
      goto destroy_bo;

   noop_dw_size = ctx->aws->info.ip[AMD_IP_GFX].ib_pad_dw_mask + 1;
   ((uint32_t*)cpu)[0] = PKT3(PKT3_NOP, noop_dw_size - 2, 0);

   ac_drm_bo_cpu_unmap(ctx->aws->dev, bo);

   list.bo_handle = kms_handle;
   ac_drm_bo_export(ctx->aws->dev, bo, amdgpu_bo_handle_type_kms, &list.bo_handle);
   list.bo_priority = 0;

   bo_list_in.list_handle = ~0;
   bo_list_in.bo_number = 1;
   bo_list_in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
   bo_list_in.bo_info_ptr = (uint64_t)(uintptr_t)&list;

   ib_in.ip_type = AMD_IP_GFX;
   ib_in.ib_bytes = noop_dw_size * 4;
   ib_in.va_start = va;

   chunks[0].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
   chunks[0].length_dw = sizeof(struct drm_amdgpu_bo_list_in) / 4;
   chunks[0].chunk_data = (uintptr_t)&bo_list_in;

   chunks[1].chunk_id = AMDGPU_CHUNK_ID_IB;
   chunks[1].length_dw = sizeof(struct drm_amdgpu_cs_chunk_ib) / 4;
   chunks[1].chunk_data = (uintptr_t)&ib_in;

   r = ac_drm_cs_submit_raw2(ctx->aws->dev, temp_ctx_handle, 0, 2, chunks, &seq_no);

destroy_bo:
   if (va_handle)
      ac_drm_va_range_free(va_handle);
   ac_drm_bo_free(ctx->aws->dev, bo);
destroy_ctx:
   ac_drm_cs_ctx_free(ctx->aws->dev, temp_ctx_handle);

   return r;
}

static void
amdgpu_ctx_set_sw_reset_status(struct radeon_winsys_ctx *rwctx, enum pipe_reset_status status,
                               const char *format, ...)
{
   struct amdgpu_ctx *ctx = (struct amdgpu_ctx*)rwctx;

   /* Don't overwrite the last reset status. */
   if (ctx->sw_status != PIPE_NO_RESET)
      return;

   ctx->sw_status = status;

   if (!ctx->allow_context_lost) {
      va_list args;

      va_start(args, format);
      vfprintf(stderr, format, args);
      va_end(args);

      /* Non-robust contexts are allowed to terminate the process. The only alternative is
       * to skip command submission, which would look like a freeze because nothing is drawn,
       * which looks like a hang without any reset.
       */
      abort();
   }
}

static enum pipe_reset_status
amdgpu_ctx_query_reset_status(struct radeon_winsys_ctx *rwctx, bool full_reset_only,
                              bool *needs_reset, bool *reset_completed)
{
   struct amdgpu_ctx *ctx = (struct amdgpu_ctx*)rwctx;

   if (needs_reset)
      *needs_reset = false;
   if (reset_completed)
      *reset_completed = false;

   /* Return a failure due to a GPU hang. */
   uint64_t flags;

   if (full_reset_only && ctx->sw_status == PIPE_NO_RESET) {
      /* If the caller is only interested in full reset (= wants to ignore soft
       * recoveries), we can use the rejected cs count as a quick first check.
       */
      return PIPE_NO_RESET;
   }

   /*
    * ctx->sw_status is updated on alloc/ioctl failures.
    *
    * We only rely on amdgpu_cs_query_reset_state2 to tell us
    * that the context reset is complete.
    */
   if (ctx->sw_status != PIPE_NO_RESET) {
      int r = ac_drm_cs_query_reset_state2(ctx->aws->dev, ctx->ctx_handle, &flags);
      if (!r) {
         if (flags & AMDGPU_CTX_QUERY2_FLAGS_RESET) {
            if (reset_completed) {
               /* The ARB_robustness spec says:
               *
               *    If a reset status other than NO_ERROR is returned and subsequent
               *    calls return NO_ERROR, the context reset was encountered and
               *    completed. If a reset status is repeatedly returned, the context may
               *    be in the process of resetting.
               *
               * Starting with drm_minor >= 54 amdgpu reports if the reset is complete,
               * so don't do anything special. On older kernels, submit a no-op cs. If it
               * succeeds then assume the reset is complete.
               */
               if (!(flags & AMDGPU_CTX_QUERY2_FLAGS_RESET_IN_PROGRESS))
                  *reset_completed = true;

               if (ctx->aws->info.drm_minor < 54 && ctx->aws->info.has_graphics)
                  *reset_completed = amdgpu_submit_gfx_nop(ctx) == 0;
            }
         }
      } else {
         fprintf(stderr, "amdgpu: amdgpu_cs_query_reset_state2 failed. (%i)\n", r);
      }

      /* Return a failure due to SW issues. */
      if (needs_reset)
         *needs_reset = true;
      return ctx->sw_status;
   }

   if (needs_reset)
      *needs_reset = false;
   return PIPE_NO_RESET;
}

/* COMMAND SUBMISSION */

static bool amdgpu_cs_has_user_fence(struct amdgpu_cs *acs)
{
   return acs->ip_type == AMD_IP_GFX ||
          acs->ip_type == AMD_IP_COMPUTE ||
          acs->ip_type == AMD_IP_SDMA;
}

static inline unsigned amdgpu_cs_epilog_dws(struct amdgpu_cs *acs)
{
   if (acs->has_chaining)
      return 4; /* for chaining */

   return 0;
}

static struct amdgpu_cs_buffer *
amdgpu_lookup_buffer(struct amdgpu_cs_context *csc, struct amdgpu_winsys_bo *bo,
                     struct amdgpu_buffer_list *list)
{
   int num_buffers = list->num_buffers;
   struct amdgpu_cs_buffer *buffers = list->buffers;
   unsigned hash = bo->unique_id & (BUFFER_HASHLIST_SIZE-1);
   int i = csc->buffer_indices_hashlist[hash];

   /* not found or found */
   if (i < 0)
      return NULL;

   if (i < num_buffers && buffers[i].bo == bo)
      return &buffers[i];

   /* Hash collision, look for the BO in the list of buffers linearly. */
   for (int i = num_buffers - 1; i >= 0; i--) {
      if (buffers[i].bo == bo) {
         /* Put this buffer in the hash list.
          * This will prevent additional hash collisions if there are
          * several consecutive lookup_buffer calls for the same buffer.
          *
          * Example: Assuming buffers A,B,C collide in the hash list,
          * the following sequence of buffers:
          *         AAAAAAAAAAABBBBBBBBBBBBBBCCCCCCCC
          * will collide here: ^ and here:   ^,
          * meaning that we should get very few collisions in the end. */
         csc->buffer_indices_hashlist[hash] = i & 0x7fff;
         return &buffers[i];
      }
   }
   return NULL;
}

struct amdgpu_cs_buffer *
amdgpu_lookup_buffer_any_type(struct amdgpu_cs_context *csc, struct amdgpu_winsys_bo *bo)
{
   return amdgpu_lookup_buffer(csc, bo, &csc->buffer_lists[get_buf_list_idx(bo)]);
}

static struct amdgpu_cs_buffer *
amdgpu_do_add_buffer(struct amdgpu_cs_context *csc, struct amdgpu_winsys_bo *bo,
                     struct amdgpu_buffer_list *list, bool add_ref)
{
   /* New buffer, check if the backing array is large enough. */
   if (unlikely(list->num_buffers >= list->max_buffers)) {
      unsigned new_max =
         MAX2(list->max_buffers + 16, (unsigned)(list->max_buffers * 1.3));
      struct amdgpu_cs_buffer *new_buffers;

      new_buffers = (struct amdgpu_cs_buffer *)
                    REALLOC(list->buffers, list->max_buffers * sizeof(*new_buffers),
                            new_max * sizeof(*new_buffers));
      if (!new_buffers) {
         fprintf(stderr, "amdgpu_do_add_buffer: allocation failed\n");
         return NULL;
      }

      list->max_buffers = new_max;
      list->buffers = new_buffers;
   }

   unsigned idx = list->num_buffers++;
   struct amdgpu_cs_buffer *buffer = &list->buffers[idx];
   if (add_ref)
      p_atomic_inc(&bo->base.reference.count);
   buffer->bo = bo;
   buffer->usage = 0;

   unsigned hash = bo->unique_id & (BUFFER_HASHLIST_SIZE-1);
   csc->buffer_indices_hashlist[hash] = idx & 0x7fff;
   return buffer;
}

static struct amdgpu_cs_buffer *
amdgpu_lookup_or_add_buffer(struct amdgpu_cs_context *csc, struct amdgpu_winsys_bo *bo,
                            struct amdgpu_buffer_list *list, bool add_ref)
{
   struct amdgpu_cs_buffer *buffer = amdgpu_lookup_buffer(csc, bo, list);

   return buffer ? buffer : amdgpu_do_add_buffer(csc, bo, list, add_ref);
}

static unsigned amdgpu_cs_add_buffer(struct radeon_cmdbuf *rcs,
                                    struct pb_buffer_lean *buf,
                                    unsigned usage,
                                    enum radeon_bo_domain domains)
{
   /* Don't use the "domains" parameter. Amdgpu doesn't support changing
    * the buffer placement during command submission.
    */
   struct amdgpu_cs_context *csc = amdgpu_csc_get_current(amdgpu_cs(rcs));
   struct amdgpu_winsys_bo *bo = (struct amdgpu_winsys_bo*)buf;
   struct amdgpu_cs_buffer *buffer;

   /* Fast exit for no-op calls.
    * This is very effective with suballocators and linear uploaders that
    * are outside of the winsys.
    */
   if (bo == csc->last_added_bo &&
       (usage & csc->last_added_bo_usage) == usage)
      return 0;

   buffer = amdgpu_lookup_or_add_buffer(csc, bo, &csc->buffer_lists[get_buf_list_idx(bo)], true);
   if (!buffer)
      return 0;

   buffer->usage |= usage;

   csc->last_added_bo_usage = buffer->usage;
   csc->last_added_bo = bo;
   return 0;
}

static bool amdgpu_ib_new_buffer(struct amdgpu_winsys *aws,
                                 struct amdgpu_ib *main_ib,
                                 struct amdgpu_cs *acs)
{
   struct pb_buffer_lean *pb;
   uint8_t *mapped;
   unsigned buffer_size;

   /* Always create a buffer that is at least as large as the maximum seen IB size,
    * aligned to a power of two.
    */
   buffer_size = util_next_power_of_two(main_ib->max_ib_bytes);

   /* Multiply by 4 to reduce internal fragmentation if chaining is not available.*/
   if (!acs->has_chaining)
      buffer_size *= 4;

   const unsigned min_size = MAX2(main_ib->max_check_space_size, 32 * 1024);
   /* This is the maximum size that fits into the INDIRECT_BUFFER packet. */
   const unsigned max_size = 2 * 1024 * 1024;

   buffer_size = MIN2(buffer_size, max_size);
   buffer_size = MAX2(buffer_size, min_size); /* min_size is more important */

   /* Use cached GTT for command buffers. Writing to other heaps is very slow on the CPU.
    * The speed of writing to GTT WC is somewhere between no difference and very slow, while
    * VRAM being very slow a lot more often.
    *
    * Bypass GL2 because command buffers are read only once. Bypassing GL2 has better latency
    * and doesn't have to wait for cached GL2 requests to be processed.
    */
   enum radeon_bo_domain domain = RADEON_DOMAIN_GTT;
   unsigned flags = RADEON_FLAG_NO_INTERPROCESS_SHARING |
                    RADEON_FLAG_GL2_BYPASS;

   if (acs->ip_type == AMD_IP_GFX ||
       acs->ip_type == AMD_IP_COMPUTE ||
       acs->ip_type == AMD_IP_SDMA) {
      /* Avoids hangs with "rendercheck -t cacomposite -f a8r8g8b8" via glamor
       * on Navi 14
       */
      flags |= RADEON_FLAG_32BIT;
   }

   pb = amdgpu_bo_create(aws, buffer_size,
                         aws->info.gart_page_size,
                         domain, (radeon_bo_flag)flags);
   if (!pb)
      return false;

   mapped = (uint8_t*)amdgpu_bo_map(&aws->dummy_sws.base, pb, NULL, PIPE_MAP_WRITE);
   if (!mapped) {
      radeon_bo_reference(&aws->dummy_sws.base, &pb, NULL);
      return false;
   }

   radeon_bo_reference(&aws->dummy_sws.base, &main_ib->big_buffer, pb);
   radeon_bo_reference(&aws->dummy_sws.base, &pb, NULL);

   main_ib->gpu_address = amdgpu_bo_get_va(main_ib->big_buffer);
   main_ib->big_buffer_cpu_ptr = mapped;
   main_ib->used_ib_space = 0;

   return true;
}

static bool amdgpu_get_new_ib(struct amdgpu_winsys *aws,
                              struct radeon_cmdbuf *rcs,
                              struct amdgpu_ib *main_ib,
                              struct amdgpu_cs *acs)
{
   struct drm_amdgpu_cs_chunk_ib *chunk_ib = &amdgpu_csc_get_current(acs)->chunk_ib[IB_MAIN];
   /* This is the minimum size of a contiguous IB. */
   unsigned ib_size = 16 * 1024;

   /* Always allocate at least the size of the biggest cs_check_space call,
    * because precisely the last call might have requested this size.
    */
   ib_size = MAX2(ib_size, main_ib->max_check_space_size);

   if (!acs->has_chaining) {
      ib_size = MAX2(ib_size, MIN2(util_next_power_of_two(main_ib->max_ib_bytes),
                                   IB_MAX_SUBMIT_BYTES));
   }

   /* Decay the IB buffer size over time, so that memory usage decreases after
    * a temporary peak.
    */
   main_ib->max_ib_bytes = main_ib->max_ib_bytes - main_ib->max_ib_bytes / 32;

   rcs->prev_dw = 0;
   rcs->num_prev = 0;
   rcs->current.cdw = 0;
   rcs->current.buf = NULL;

   /* Allocate a new buffer for IBs if the current buffer is all used. */
   if (!main_ib->big_buffer ||
       main_ib->used_ib_space + ib_size > main_ib->big_buffer->size) {
      if (!amdgpu_ib_new_buffer(aws, main_ib, acs))
         return false;
   }

   chunk_ib->va_start = main_ib->gpu_address + main_ib->used_ib_space;
   chunk_ib->ib_bytes = 0;
   /* ib_bytes is in dwords and the conversion to bytes will be done before
    * the CS ioctl. */
   main_ib->ptr_ib_size = &chunk_ib->ib_bytes;
   main_ib->is_chained_ib = false;

   amdgpu_cs_add_buffer(rcs, main_ib->big_buffer,
                        (radeon_bo_flag)(RADEON_USAGE_READ | RADEON_PRIO_IB),
                        (radeon_bo_domain)0);

   rcs->current.buf = (uint32_t*)(main_ib->big_buffer_cpu_ptr + main_ib->used_ib_space);

   amdgpu_csc_get_current(acs)->ib_main_addr = rcs->current.buf;

   ib_size = main_ib->big_buffer->size - main_ib->used_ib_space;
   rcs->current.max_dw = ib_size / 4 - amdgpu_cs_epilog_dws(acs);
   return true;
}

static void amdgpu_set_ib_size(struct radeon_cmdbuf *rcs, struct amdgpu_ib *ib)
{
   if (ib->is_chained_ib) {
      *ib->ptr_ib_size = rcs->current.cdw |
                         S_3F2_CHAIN(1) | S_3F2_VALID(1) |
                         S_3F2_PRE_ENA(((struct amdgpu_cs*)ib)->preamble_ib_bo != NULL);
   } else {
      *ib->ptr_ib_size = rcs->current.cdw;
   }
}

static void amdgpu_ib_finalize(struct amdgpu_winsys *aws, struct radeon_cmdbuf *rcs,
                               struct amdgpu_ib *ib, enum amd_ip_type ip_type)
{
   amdgpu_set_ib_size(rcs, ib);
   ib->used_ib_space += rcs->current.cdw * 4;
   ib->used_ib_space = align(ib->used_ib_space, aws->info.ip[ip_type].ib_alignment);
   ib->max_ib_bytes = MAX2(ib->max_ib_bytes, (rcs->prev_dw + rcs->current.cdw) * 4);
}

static bool amdgpu_init_cs_context(struct amdgpu_winsys *aws,
                                   struct amdgpu_cs_context *csc,
                                   enum amd_ip_type ip_type)
{
   for (unsigned i = 0; i < ARRAY_SIZE(csc->chunk_ib); i++) {
      csc->chunk_ib[i].ip_type = ip_type;
      csc->chunk_ib[i].flags = 0;

      if (ip_type == AMD_IP_GFX || ip_type == AMD_IP_COMPUTE) {
         /* The kernel shouldn't invalidate L2 and vL1. The proper place for cache invalidation
          * is the beginning of IBs because completion of an IB doesn't care about the state of
          * GPU caches, only the beginning of an IB does. Draw calls from multiple IBs can be
          * executed in parallel, so draw calls from the current IB can finish after the next IB
          * starts drawing, and so the cache flush at the end of IBs is usually late and thus
          * useless.
          */
         csc->chunk_ib[i].flags |= AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE;
      }
   }

   csc->chunk_ib[IB_PREAMBLE].flags |= AMDGPU_IB_FLAG_PREAMBLE;
   csc->last_added_bo = NULL;
   return true;
}

static void cleanup_fence_list(struct amdgpu_fence_list *fences)
{
   for (unsigned i = 0; i < fences->num; i++)
      amdgpu_fence_drop_reference(fences->list[i]);
   fences->num = 0;
}

static void amdgpu_cs_context_cleanup_buffers(struct amdgpu_winsys *aws, struct amdgpu_cs_context *csc)
{
   for (unsigned i = 0; i < ARRAY_SIZE(csc->buffer_lists); i++) {
      struct amdgpu_cs_buffer *buffers = csc->buffer_lists[i].buffers;
      unsigned num_buffers = csc->buffer_lists[i].num_buffers;

      for (unsigned j = 0; j < num_buffers; j++)
         amdgpu_winsys_bo_drop_reference(aws, buffers[j].bo);

      csc->buffer_lists[i].num_buffers = 0;
   }
}

static void amdgpu_cs_context_cleanup(struct amdgpu_winsys *aws, struct amdgpu_cs_context *csc)
{
   csc->seq_no_dependencies.valid_fence_mask = 0;
   cleanup_fence_list(&csc->syncobj_dependencies);
   cleanup_fence_list(&csc->syncobj_to_signal);
   amdgpu_fence_reference(&csc->fence, NULL);
   csc->last_added_bo = NULL;
}

static void amdgpu_destroy_cs_context(struct amdgpu_winsys *aws, struct amdgpu_cs_context *csc)
{
   amdgpu_cs_context_cleanup_buffers(aws, csc);
   amdgpu_cs_context_cleanup(aws, csc);
   for (unsigned i = 0; i < ARRAY_SIZE(csc->buffer_lists); i++)
      FREE(csc->buffer_lists[i].buffers);
   FREE(csc->syncobj_dependencies.list);
   FREE(csc->syncobj_to_signal.list);
}


static enum amd_ip_type amdgpu_cs_get_ip_type(struct radeon_cmdbuf *rcs)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   return acs->ip_type;
}

static bool ip_uses_alt_fence(enum amd_ip_type ip_type)
{
   /* The alt_fence path can be tested thoroughly by enabling it for GFX here. */
   return ip_type == AMD_IP_VCN_DEC ||
          ip_type == AMD_IP_VCN_ENC ||
          ip_type == AMD_IP_VCN_JPEG;
}

static void amdgpu_cs_destroy(struct radeon_cmdbuf *rcs)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);

   if (!acs)
      return;

   amdgpu_cs_sync_flush(rcs);
   util_queue_fence_destroy(&acs->flush_completed);
   p_atomic_dec(&acs->aws->num_cs);
   radeon_bo_reference(&acs->aws->dummy_sws.base, &acs->preamble_ib_bo, NULL);
   radeon_bo_reference(&acs->aws->dummy_sws.base, &acs->main_ib.big_buffer, NULL);
   FREE(rcs->prev);
   for (unsigned i = 0; i < ARRAY_SIZE(acs->csc); i++)
      amdgpu_destroy_cs_context(acs->aws, &acs->csc[i]);
   amdgpu_fence_reference(&acs->next_fence, NULL);
   FREE(acs);
}

static bool
amdgpu_cs_create(struct radeon_cmdbuf *rcs,
                 struct radeon_winsys_ctx *rwctx,
                 enum amd_ip_type ip_type,
                 void (*flush)(void *ctx, unsigned flags,
                               struct pipe_fence_handle **fence),
                 void *flush_ctx)
{
   struct amdgpu_ctx *ctx = (struct amdgpu_ctx*)rwctx;
   struct amdgpu_cs *acs;

   acs = CALLOC_STRUCT(amdgpu_cs);
   if (!acs) {
      return false;
   }

   util_queue_fence_init(&acs->flush_completed);

   acs->aws = ctx->aws;
   acs->ctx = ctx;
   acs->flush_cs = flush;
   acs->flush_data = flush_ctx;
   acs->ip_type = ip_type;
   acs->noop = ctx->aws->noop_cs;
   acs->has_chaining = ctx->aws->info.gfx_level >= GFX7 &&
                       (ip_type == AMD_IP_GFX || ip_type == AMD_IP_COMPUTE);

   /* Compute the queue index by counting the IPs that have queues. */
   assert(ip_type < ARRAY_SIZE(ctx->aws->info.ip));
   assert(ctx->aws->info.ip[ip_type].num_queues);

   if (ip_uses_alt_fence(ip_type)) {
      acs->queue_index = INT_MAX;
      acs->uses_alt_fence = true;
   } else {
      acs->queue_index = 0;

      for (unsigned i = 0; i < ARRAY_SIZE(ctx->aws->info.ip); i++) {
         if (!ctx->aws->info.ip[i].num_queues || ip_uses_alt_fence((amd_ip_type)i))
            continue;

         if (i == ip_type)
            break;

         acs->queue_index++;
      }
      assert(acs->queue_index < AMDGPU_MAX_QUEUES);
   }

   ac_drm_cs_chunk_fence_info_to_data(acs->ctx->user_fence_bo_kms_handle, acs->ip_type * 4,
                                      (struct drm_amdgpu_cs_chunk_data*)&acs->fence_chunk);

   memset(acs->buffer_indices_hashlist, -1, sizeof(acs->buffer_indices_hashlist));

   for (unsigned i = 0; i < ARRAY_SIZE(acs->csc); i++) {
      if (!amdgpu_init_cs_context(ctx->aws, &acs->csc[i], ip_type)) {
         if (i)
            amdgpu_destroy_cs_context(ctx->aws, &acs->csc[0]);
         FREE(acs);
         return false;
      }

     /* only csc will use for buffer_indices_hashlist. */
      acs->csc[i].buffer_indices_hashlist = acs->buffer_indices_hashlist;
      acs->csc[i].aws = ctx->aws;
   }

   p_atomic_inc(&ctx->aws->num_cs);
   rcs->priv = acs;

   if (!amdgpu_get_new_ib(ctx->aws, rcs, &acs->main_ib, acs))
      goto fail;

   /* Currently only gfx, compute and sdma queues supports user queue. */
   if (acs->aws->info.use_userq && ip_type <= AMD_IP_SDMA) {
      if (!amdgpu_userq_init(acs->aws, &acs->aws->queues[acs->queue_index].userq, ip_type))
         goto fail;
   }

   return true;
fail:
   rcs->priv = NULL;
   amdgpu_cs_destroy(rcs);
   return false;
}

static bool
amdgpu_cs_setup_preemption(struct radeon_cmdbuf *rcs, const uint32_t *preamble_ib,
                           unsigned preamble_num_dw)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_winsys *aws = acs->aws;
   unsigned size = align(preamble_num_dw * 4, aws->info.ip[AMD_IP_GFX].ib_alignment);
   struct pb_buffer_lean *preamble_bo;
   uint32_t *map;

   /* Create the preamble IB buffer. */
   preamble_bo = amdgpu_bo_create(aws, size, aws->info.ip[AMD_IP_GFX].ib_alignment,
                                  RADEON_DOMAIN_VRAM,
                                  (radeon_bo_flag)
                                  (RADEON_FLAG_NO_INTERPROCESS_SHARING |
                                   RADEON_FLAG_GTT_WC));
   if (!preamble_bo)
      return false;

   map = (uint32_t*)amdgpu_bo_map(&aws->dummy_sws.base, preamble_bo, NULL,
                                  (pipe_map_flags)(PIPE_MAP_WRITE | RADEON_MAP_TEMPORARY));
   if (!map) {
      radeon_bo_reference(&aws->dummy_sws.base, &preamble_bo, NULL);
      return false;
   }

   /* Upload the preamble IB. */
   memcpy(map, preamble_ib, preamble_num_dw * 4);

   /* Pad the IB. */
   amdgpu_pad_gfx_compute_ib(aws, acs->ip_type, map, &preamble_num_dw, 0);
   amdgpu_bo_unmap(&aws->dummy_sws.base, preamble_bo);

   for (unsigned i = 0; i < ARRAY_SIZE(acs->csc); i++) {
      acs->csc[i].chunk_ib[IB_PREAMBLE].va_start = amdgpu_bo_get_va(preamble_bo);
      acs->csc[i].chunk_ib[IB_PREAMBLE].ib_bytes = preamble_num_dw * 4;

      acs->csc[i].chunk_ib[IB_MAIN].flags |= AMDGPU_IB_FLAG_PREEMPT;
   }

   assert(!acs->preamble_ib_bo);
   acs->preamble_ib_bo = preamble_bo;

   amdgpu_cs_add_buffer(rcs, acs->preamble_ib_bo,
                        RADEON_USAGE_READ | RADEON_PRIO_IB, (radeon_bo_domain)0);
   return true;
}

static bool amdgpu_cs_validate(struct radeon_cmdbuf *rcs)
{
   return true;
}

static bool amdgpu_cs_check_space(struct radeon_cmdbuf *rcs, unsigned dw)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_ib *main_ib = &acs->main_ib;

   if (rcs->current.cdw > rcs->current.max_dw)
      return false;

   unsigned projected_size_dw = rcs->prev_dw + rcs->current.cdw + dw;

   if (projected_size_dw * 4 > IB_MAX_SUBMIT_BYTES)
      return false;

   if (rcs->current.max_dw - rcs->current.cdw >= dw)
      return true;

   unsigned cs_epilog_dw = amdgpu_cs_epilog_dws(acs);
   unsigned need_byte_size = (dw + cs_epilog_dw) * 4;
   /* 125% of the size for IB epilog. */
   unsigned safe_byte_size = need_byte_size + need_byte_size / 4;
   main_ib->max_check_space_size = MAX2(main_ib->max_check_space_size, safe_byte_size);
   main_ib->max_ib_bytes = MAX2(main_ib->max_ib_bytes, projected_size_dw * 4);

   if (!acs->has_chaining)
      return false;

   /* Allocate a new chunk */
   if (rcs->num_prev >= rcs->max_prev) {
      unsigned new_max_prev = MAX2(1, 2 * rcs->max_prev);
      struct radeon_cmdbuf_chunk *new_prev;

      new_prev = (struct radeon_cmdbuf_chunk*)
                 REALLOC(rcs->prev, sizeof(*new_prev) * rcs->max_prev,
                         sizeof(*new_prev) * new_max_prev);
      if (!new_prev)
         return false;

      rcs->prev = new_prev;
      rcs->max_prev = new_max_prev;
   }

   if (!amdgpu_ib_new_buffer(acs->aws, main_ib, acs))
      return false;

   assert(main_ib->used_ib_space == 0);
   uint64_t va = main_ib->gpu_address;

   /* This space was originally reserved. */
   rcs->current.max_dw += cs_epilog_dw;

   /* Pad with NOPs but leave 4 dwords for INDIRECT_BUFFER. */
   amdgpu_pad_gfx_compute_ib(acs->aws, acs->ip_type, rcs->current.buf, &rcs->current.cdw, 4);

   radeon_emit(rcs, PKT3(PKT3_INDIRECT_BUFFER, 2, 0));
   radeon_emit(rcs, va);
   radeon_emit(rcs, va >> 32);
   uint32_t *new_ptr_ib_size = &rcs->current.buf[rcs->current.cdw++];

   assert((rcs->current.cdw & acs->aws->info.ip[acs->ip_type].ib_pad_dw_mask) == 0);
   assert(rcs->current.cdw <= rcs->current.max_dw);

   amdgpu_set_ib_size(rcs, main_ib);
   main_ib->ptr_ib_size = new_ptr_ib_size;
   main_ib->is_chained_ib = true;

   /* Hook up the new chunk */
   rcs->prev[rcs->num_prev].buf = rcs->current.buf;
   rcs->prev[rcs->num_prev].cdw = rcs->current.cdw;
   rcs->prev[rcs->num_prev].max_dw = rcs->current.cdw; /* no modifications */
   rcs->num_prev++;

   rcs->prev_dw += rcs->current.cdw;
   rcs->current.cdw = 0;

   rcs->current.buf = (uint32_t*)(main_ib->big_buffer_cpu_ptr + main_ib->used_ib_space);
   rcs->current.max_dw = main_ib->big_buffer->size / 4 - cs_epilog_dw;

   amdgpu_cs_add_buffer(rcs, main_ib->big_buffer,
                        RADEON_USAGE_READ | RADEON_PRIO_IB, (radeon_bo_domain)0);

   return true;
}

static void amdgpu_add_slab_backing_buffers(struct amdgpu_cs_context *csc)
{
   unsigned num_buffers = csc->buffer_lists[AMDGPU_BO_SLAB_ENTRY].num_buffers;
   struct amdgpu_cs_buffer *buffers = csc->buffer_lists[AMDGPU_BO_SLAB_ENTRY].buffers;

   for (unsigned i = 0; i < num_buffers; i++) {
      struct amdgpu_cs_buffer *slab_buffer = &buffers[i];
      struct amdgpu_cs_buffer *real_buffer =
         amdgpu_lookup_or_add_buffer(csc, &get_slab_entry_real_bo(slab_buffer->bo)->b,
                                     &csc->buffer_lists[AMDGPU_BO_REAL], true);

      /* We need to set the usage because it determines the BO priority.
       *
       * Mask out the SYNCHRONIZED flag because the backing buffer of slabs shouldn't add its
       * BO fences to fence dependencies. Only the slab entries should do that.
       */
      real_buffer->usage |= slab_buffer->usage & ~RADEON_USAGE_SYNCHRONIZED;
   }
}

static unsigned amdgpu_cs_get_buffer_list(struct radeon_cmdbuf *rcs,
                                          struct radeon_bo_list_item *list)
{
   struct amdgpu_cs_context *csc = amdgpu_csc_get_current(amdgpu_cs(rcs));

    /* We do this in the CS thread, but since we need to return the final usage of all buffers
     * here, do it here too. There is no harm in doing it again in the CS thread.
     */
    amdgpu_add_slab_backing_buffers(csc);

    struct amdgpu_buffer_list *real_buffers = &csc->buffer_lists[AMDGPU_BO_REAL];
    unsigned num_real_buffers = real_buffers->num_buffers;

#if HAVE_AMDGPU_VIRTIO
    assert(!csc->ws->info.is_virtio);
#endif

    if (list) {
        for (unsigned i = 0; i < num_real_buffers; i++) {
            list[i].bo_size = real_buffers->buffers[i].bo->base.size;
            list[i].vm_address =
               amdgpu_va_get_start_addr(get_real_bo(real_buffers->buffers[i].bo)->va_handle);
            list[i].priority_usage = real_buffers->buffers[i].usage;
        }
    }
    return num_real_buffers;
}

static void add_fence_to_list(struct amdgpu_fence_list *fences,
                              struct amdgpu_fence *fence)
{
   unsigned idx = fences->num++;

   if (idx >= fences->max) {
      unsigned size;
      const unsigned increment = 8;

      fences->max = idx + increment;
      size = fences->max * sizeof(fences->list[0]);
      fences->list = (struct pipe_fence_handle**)realloc(fences->list, size);
   }
   amdgpu_fence_set_reference(&fences->list[idx], (struct pipe_fence_handle*)fence);
}

static void amdgpu_cs_add_fence_dependency(struct radeon_cmdbuf *rcs,
                                           struct pipe_fence_handle *pfence)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_winsys *aws = acs->aws;
   struct amdgpu_cs_context *csc = amdgpu_csc_get_current(acs);
   struct amdgpu_fence *fence = (struct amdgpu_fence*)pfence;

   util_queue_fence_wait(&fence->submitted);

   if (!fence->imported) {
      if (!aws->info.use_userq || fence->ip_type != acs->ip_type || acs->ip_type > AMD_IP_SDMA) {
         /* Ignore idle fences. This will only check the user fence in memory. */
         if (!amdgpu_fence_wait((struct pipe_fence_handle *)fence, 0, false)) {
            add_seq_no_to_list(acs->aws, &csc->seq_no_dependencies, fence->queue_index,
                               fence->queue_seq_no);
         }
      }
   } else {
      add_fence_to_list(&csc->syncobj_dependencies, fence);
   }
}

static void amdgpu_add_fences_to_dependencies(struct amdgpu_winsys *ws,
                                              struct amdgpu_cs_context *csc,
                                              unsigned queue_index_bit,
                                              struct amdgpu_seq_no_fences *dependencies,
                                              struct amdgpu_winsys_bo *bo, unsigned usage)
{
   if (usage & RADEON_USAGE_SYNCHRONIZED) {
      /* Add BO fences from queues other than 'queue_index' to dependencies. */
      u_foreach_bit(other_queue_idx, bo->fences.valid_fence_mask & ~queue_index_bit) {
         add_seq_no_to_list(ws, dependencies, other_queue_idx,
                            bo->fences.seq_no[other_queue_idx]);
      }

      if (bo->alt_fence)
         add_fence_to_list(&csc->syncobj_dependencies, (struct amdgpu_fence*)bo->alt_fence);
   }
}

static void amdgpu_set_bo_seq_no(unsigned queue_index, struct amdgpu_winsys_bo *bo,
                                 uint_seq_no new_queue_seq_no)
{
   bo->fences.seq_no[queue_index] = new_queue_seq_no;
   bo->fences.valid_fence_mask |= BITFIELD_BIT(queue_index);
}

static void amdgpu_add_to_kernel_bo_list(struct drm_amdgpu_bo_list_entry *bo_entry,
                                         struct amdgpu_winsys_bo *bo, unsigned usage)
{
   bo_entry->bo_handle = get_real_bo(bo)->kms_handle;
   bo_entry->bo_priority = (util_last_bit(usage & RADEON_ALL_PRIORITIES) - 1) / 2;
}

static void amdgpu_cs_add_syncobj_signal(struct radeon_cmdbuf *rcs,
                                         struct pipe_fence_handle *fence)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_cs_context *csc = amdgpu_csc_get_current(acs);

   add_fence_to_list(&csc->syncobj_to_signal, (struct amdgpu_fence*)fence);
}

static int amdgpu_cs_submit_ib_kernelq(struct amdgpu_cs *acs,
                                       unsigned num_real_buffers,
                                       struct drm_amdgpu_bo_list_entry *bo_list_real,
                                       uint64_t *seq_no)
{
   struct amdgpu_winsys *aws = acs->aws;
   struct amdgpu_cs_context *csc = amdgpu_csc_get_submitted(acs);
   struct drm_amdgpu_bo_list_in bo_list_in;
   struct drm_amdgpu_cs_chunk chunks[8];
   unsigned num_chunks = 0;

   /* BO list */
   bo_list_in.operation = ~0;
   bo_list_in.list_handle = ~0;
   bo_list_in.bo_number = num_real_buffers;
   bo_list_in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
   bo_list_in.bo_info_ptr = (uint64_t)(uintptr_t)bo_list_real;

   chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
   chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_bo_list_in) / 4;
   chunks[num_chunks].chunk_data = (uintptr_t)&bo_list_in;
   num_chunks++;

   /* Syncobj dependencies. */
   unsigned num_syncobj_dependencies = csc->syncobj_dependencies.num;
   if (num_syncobj_dependencies) {
      struct drm_amdgpu_cs_chunk_sem *sem_chunk =
         (struct drm_amdgpu_cs_chunk_sem *)
         alloca(num_syncobj_dependencies * sizeof(sem_chunk[0]));

      for (unsigned i = 0; i < num_syncobj_dependencies; i++) {
         struct amdgpu_fence *fence =
            (struct amdgpu_fence*)csc->syncobj_dependencies.list[i];

         assert(util_queue_fence_is_signalled(&fence->submitted));
         sem_chunk[i].handle = fence->syncobj;
      }

      chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_IN;
      chunks[num_chunks].length_dw = sizeof(sem_chunk[0]) / 4 * num_syncobj_dependencies;
      chunks[num_chunks].chunk_data = (uintptr_t)sem_chunk;
      num_chunks++;
   }

   /* Syncobj signals. */
   unsigned num_syncobj_to_signal = 1 + csc->syncobj_to_signal.num;
   struct drm_amdgpu_cs_chunk_sem *sem_chunk =
      (struct drm_amdgpu_cs_chunk_sem *)
      alloca(num_syncobj_to_signal * sizeof(sem_chunk[0]));

   for (unsigned i = 0; i < num_syncobj_to_signal - 1; i++) {
      struct amdgpu_fence *fence =
         (struct amdgpu_fence*)csc->syncobj_to_signal.list[i];

      sem_chunk[i].handle = fence->syncobj;
   }
   sem_chunk[csc->syncobj_to_signal.num].handle = ((struct amdgpu_fence*)csc->fence)->syncobj;

   chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT;
   chunks[num_chunks].length_dw = sizeof(sem_chunk[0]) / 4 * num_syncobj_to_signal;
   chunks[num_chunks].chunk_data = (uintptr_t)sem_chunk;
   num_chunks++;

   if (aws->info.has_fw_based_shadowing && acs->mcbp_fw_shadow_chunk.shadow_va) {
      chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_CP_GFX_SHADOW;
      chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_cs_chunk_cp_gfx_shadow) / 4;
      chunks[num_chunks].chunk_data = (uintptr_t)&acs->mcbp_fw_shadow_chunk;
      num_chunks++;
   }

   /* Fence */
   if (amdgpu_cs_has_user_fence(acs)) {
      chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_FENCE;
      chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_cs_chunk_fence) / 4;
      chunks[num_chunks].chunk_data = (uintptr_t)&acs->fence_chunk;
      num_chunks++;
   }

   /* IB */
   if (csc->chunk_ib[IB_PREAMBLE].ib_bytes) {
      chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_IB;
      chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_cs_chunk_ib) / 4;
      chunks[num_chunks].chunk_data = (uintptr_t)&csc->chunk_ib[IB_PREAMBLE];
      num_chunks++;
   }

   /* IB */
   chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_IB;
   chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_cs_chunk_ib) / 4;
   chunks[num_chunks].chunk_data = (uintptr_t)&csc->chunk_ib[IB_MAIN];
   num_chunks++;

   if (csc->secure) {
      /* Secure submissions not supported for compute. */
      assert(acs->ip_type != AMD_IP_COMPUTE);

      csc->chunk_ib[IB_PREAMBLE].flags |= AMDGPU_IB_FLAGS_SECURE;
      csc->chunk_ib[IB_MAIN].flags |= AMDGPU_IB_FLAGS_SECURE;
   } else {
      csc->chunk_ib[IB_PREAMBLE].flags &= ~AMDGPU_IB_FLAGS_SECURE;
      csc->chunk_ib[IB_MAIN].flags &= ~AMDGPU_IB_FLAGS_SECURE;
   }

   assert(num_chunks <= 8);

   /* Submit the command buffer.
    *
    * The kernel returns -ENOMEM with many parallel processes using GDS such as test suites
    * quite often, but it eventually succeeds after enough attempts. This happens frequently
    * with dEQP using NGG streamout.
    */
   int r = 0;

   do {
      /* Wait 1 ms and try again. */
      if (r == -ENOMEM)
         os_time_sleep(1000);

      r = ac_drm_cs_submit_raw2(aws->dev, acs->ctx->ctx_handle, 0, num_chunks, chunks, seq_no);
   } while (r == -ENOMEM);

   return r;
}

static void amdgpu_cs_add_userq_packets(struct amdgpu_userq *userq,
                                        struct amdgpu_cs_context *csc,
                                        uint64_t num_fences,
                                        struct drm_amdgpu_userq_fence_info *fence_info)
{
   amdgpu_pkt_begin();

   if (userq->ip_type == AMD_IP_GFX || userq->ip_type == AMD_IP_COMPUTE) {
      if (num_fences) {
         unsigned num_fences_in_iter;
         /* FENCE_WAIT_MULTI packet supports max 32 fenes */
         for (unsigned i = 0; i < num_fences; i = i + 32) {
            num_fences_in_iter = (i + 32 > num_fences) ? num_fences - i : 32;
            amdgpu_pkt_add_dw(PKT3(PKT3_FENCE_WAIT_MULTI, num_fences_in_iter * 4, 0));
            amdgpu_pkt_add_dw(S_D10_ENGINE_SEL(1) | S_D10_POLL_INTERVAL(4) | S_D10_PREEMPTABLE(1));
            for (unsigned j = 0; j < num_fences_in_iter; j++) {
               amdgpu_pkt_add_dw(fence_info[i + j].va);
               amdgpu_pkt_add_dw(fence_info[i + j].va >> 32);
               amdgpu_pkt_add_dw(fence_info[i + j].value);
               amdgpu_pkt_add_dw(fence_info[i + j].value >> 32);
            }
         }
      }

      amdgpu_pkt_add_dw(PKT3(PKT3_HDP_FLUSH, 0, 0));
      amdgpu_pkt_add_dw(0x0);

      amdgpu_pkt_add_dw(PKT3(PKT3_INDIRECT_BUFFER, 2, 0));
      amdgpu_pkt_add_dw(csc->chunk_ib[IB_MAIN].va_start);
      amdgpu_pkt_add_dw(csc->chunk_ib[IB_MAIN].va_start >> 32);
      if (userq->ip_type == AMD_IP_GFX)
         amdgpu_pkt_add_dw((csc->chunk_ib[IB_MAIN].ib_bytes / 4) | S_3F3_INHERIT_VMID_MQD_GFX(1));
      else
         amdgpu_pkt_add_dw((csc->chunk_ib[IB_MAIN].ib_bytes / 4) | S_3F3_VALID_COMPUTE(1) |
                              S_3F3_INHERIT_VMID_MQD_COMPUTE(1));

      /* Add 8 for release mem packet and 2 for protected fence signal packet.
       * Calculcating userq_fence_seq_num this way to match with kernel fence that is
       * returned in userq_wait iotl.
       */
      userq->user_fence_seq_num = __next_wptr + 8 + 2;

      /* add release mem for user fence */
      amdgpu_pkt_add_dw(PKT3(PKT3_RELEASE_MEM, 6, 0));
      amdgpu_pkt_add_dw(S_490_EVENT_TYPE(V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT) |
                           S_490_EVENT_INDEX(5) | S_490_GLM_WB(1) | S_490_GLM_INV(1) |
                           S_490_GL2_WB(1) | S_490_SEQ(1) | S_490_CACHE_POLICY(3));
      amdgpu_pkt_add_dw(S_030358_DATA_SEL(2));
      amdgpu_pkt_add_dw(userq->user_fence_va);
      amdgpu_pkt_add_dw(userq->user_fence_va >> 32);
      amdgpu_pkt_add_dw(userq->user_fence_seq_num);
      amdgpu_pkt_add_dw(userq->user_fence_seq_num >> 32);
      amdgpu_pkt_add_dw(0);

      /* protected signal packet. This is trusted RELEASE_MEM packet. i.e. fence buffer
       * is only accessible from kernel through VMID 0.
       */
      amdgpu_pkt_add_dw(PKT3(PKT3_PROTECTED_FENCE_SIGNAL, 0, 0));
      amdgpu_pkt_add_dw(0);
   } else {
      fprintf(stderr, "amdgpu: unsupported userq ip submission = %d\n", userq->ip_type);
   }

   amdgpu_pkt_end();
}

static int amdgpu_cs_submit_ib_userq(struct amdgpu_userq *userq,
                                     struct amdgpu_cs *acs,
                                     uint32_t *shared_buf_kms_handles_write,
                                     unsigned num_shared_buf_write,
                                     uint32_t *shared_buf_kms_handles_read,
                                     unsigned num_shared_buf_read,
                                     uint64_t *seq_no,
                                     uint64_t vm_timeline_point)
{
   int r = 0;
   struct amdgpu_winsys *aws = acs->aws;
   struct amdgpu_cs_context *csc = amdgpu_csc_get_submitted(acs);

   /* Syncobj dependencies. */
   unsigned num_syncobj_dependencies = csc->syncobj_dependencies.num;
   uint32_t *syncobj_dependencies_list =
      (uint32_t*)alloca(num_syncobj_dependencies * sizeof(uint32_t));

   /* Currently only 1 vm timeline syncobj can be a dependency. */
   uint16_t num_syncobj_timeline_dependencies = 1;
   uint32_t syncobj_timeline_dependency;
   uint64_t syncobj_timeline_dependency_point;

   if (num_syncobj_dependencies) {
      for (unsigned i = 0; i < num_syncobj_dependencies; i++) {
         struct amdgpu_fence *fence =
            (struct amdgpu_fence*)csc->syncobj_dependencies.list[i];

         assert(util_queue_fence_is_signalled(&fence->submitted));
         syncobj_dependencies_list[i] = fence->syncobj;
      }
   }
   syncobj_timeline_dependency = aws->vm_timeline_syncobj;
   syncobj_timeline_dependency_point = vm_timeline_point;

   /* Syncobj signals. Adding 1 for cs submission fence. */
   unsigned num_syncobj_to_signal = csc->syncobj_to_signal.num + 1;
   uint32_t *syncobj_signal_list =
      (uint32_t*)alloca(num_syncobj_to_signal * sizeof(uint32_t));

   for (unsigned i = 0; i < csc->syncobj_to_signal.num; i++) {
      struct amdgpu_fence *fence =
         (struct amdgpu_fence*)csc->syncobj_to_signal.list[i];

      syncobj_signal_list[i] = fence->syncobj;
   }
   syncobj_signal_list[num_syncobj_to_signal - 1] = ((struct amdgpu_fence*)csc->fence)->syncobj;

   struct drm_amdgpu_userq_fence_info *fence_info;
   struct drm_amdgpu_userq_wait userq_wait_data = {
      .syncobj_handles = (uintptr_t)syncobj_dependencies_list,
      .syncobj_timeline_handles = (uintptr_t)&syncobj_timeline_dependency,
      .syncobj_timeline_points = (uintptr_t)&syncobj_timeline_dependency_point,
      .bo_read_handles = (uintptr_t)shared_buf_kms_handles_read,
      .bo_write_handles = (uintptr_t)shared_buf_kms_handles_write,
      .num_syncobj_timeline_handles = num_syncobj_timeline_dependencies,
      .num_fences = 0,
      .num_syncobj_handles = num_syncobj_dependencies,
      .num_bo_read_handles = num_shared_buf_read,
      .num_bo_write_handles = num_shared_buf_write,
      .out_fences = (uintptr_t)NULL,
   };

   /*
    * Buffers sharing synchronization follow these rules:
    *   - read-only buffers wait for all previous writes to complete
    *   - write-only(also read-write) buffers wait for all previous reads to complete
    * To implement this strategy, we use amdgpu_userq_wait() before submitting
    * a job, and amdgpu_userq_signal() after to indicate completion.
    */
   r = ac_drm_userq_wait(aws->dev, &userq_wait_data);
   if (r)
      fprintf(stderr, "amdgpu: getting wait num_fences failed\n");

   fence_info = (struct drm_amdgpu_userq_fence_info*)
      alloca(userq_wait_data.num_fences * sizeof(struct drm_amdgpu_userq_fence_info));
   userq_wait_data.out_fences = (uintptr_t)fence_info;

   r = ac_drm_userq_wait(aws->dev, &userq_wait_data);
   if (r)
      fprintf(stderr, "amdgpu: getting wait fences failed\n");

   simple_mtx_lock(&userq->lock);
   amdgpu_cs_add_userq_packets(userq, csc, userq_wait_data.num_fences, fence_info);
   struct drm_amdgpu_userq_signal userq_signal_data = {
      .queue_id = userq->userq_handle,
      .syncobj_handles = (uintptr_t)syncobj_signal_list,
      .num_syncobj_handles = num_syncobj_to_signal,
      .bo_read_handles = (uintptr_t)shared_buf_kms_handles_read,
      .bo_write_handles = (uintptr_t)shared_buf_kms_handles_write,
      .num_bo_read_handles = num_shared_buf_read,
      .num_bo_write_handles = num_shared_buf_write,
   };

#if DETECT_CC_GCC && (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
   asm volatile ("mfence" : : : "memory");
#endif
   /* Writing to *userq->wptr_bo_map is writing into mqd data. Before writing wptr into mqd
    * data, need to ensure that new packets added to user queue ring buffer are updated to.
    * memory. To ensure memory is updated, mfence is used.
    */
   *userq->wptr_bo_map = userq->next_wptr;
   /* Ringing the doorbell will have gpu execute new packets that were added in user queue
    * ring buffer. Before ringing the doorbell needed to ensure that mqd data is updated to
    * memory. To ensure memory is updated, mfence is used.
    */
#if DETECT_CC_GCC && (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
   asm volatile ("mfence" : : : "memory");
#endif
   userq->doorbell_bo_map[AMDGPU_USERQ_DOORBELL_INDEX] = userq->next_wptr;
   r = ac_drm_userq_signal(aws->dev, &userq_signal_data);

   *seq_no = userq->user_fence_seq_num;
   simple_mtx_unlock(&userq->lock);

   return r;
}

enum queue_type {
   KERNELQ,
   KERNELQ_ALT_FENCE,
   USERQ,
};

/* The template parameter determines whether the queue should skip code used by the default queue
 * system that's based on sequence numbers, and instead use and update amdgpu_winsys_bo::alt_fence
 * for all BOs.
 */
template<enum queue_type queue_type>
static void amdgpu_cs_submit_ib(void *job, void *gdata, int thread_index)
{
   struct amdgpu_cs *acs = (struct amdgpu_cs*)job;
   struct amdgpu_winsys *aws = acs->aws;
   struct amdgpu_cs_context *csc = amdgpu_csc_get_submitted(acs);
   int r;
   uint64_t seq_no = 0;
   bool has_user_fence = amdgpu_cs_has_user_fence(acs);
   /* The maximum timeline point of VM updates for all BOs used in this submit. */
   uint64_t vm_timeline_point = 0;

   simple_mtx_lock(&aws->bo_fence_lock);
   unsigned queue_index;
   struct amdgpu_queue *queue;
   uint_seq_no prev_seq_no, next_seq_no;

   if (queue_type != KERNELQ_ALT_FENCE) {
      queue_index = acs->queue_index;
      queue = &aws->queues[queue_index];
      prev_seq_no = queue->latest_seq_no;

      /* Generate a per queue sequence number. The logic is similar to the kernel side amdgpu seqno,
       * but the values aren't related.
       */
      next_seq_no = prev_seq_no + 1;

      /* Wait for the oldest fence to signal. This should always check the user fence, then wait
       * via the ioctl. We have to do this because we are going to release the oldest fence and
       * replace it with the latest fence in the ring.
       */
      struct pipe_fence_handle **oldest_fence =
         &queue->fences[next_seq_no % AMDGPU_FENCE_RING_SIZE];

      if (*oldest_fence) {
         if (!amdgpu_fence_wait(*oldest_fence, 0, false)) {
            /* Take the reference because the fence can be released by other threads after we
             * unlock the mutex.
             */
            struct pipe_fence_handle *tmp_fence = NULL;
            amdgpu_fence_reference(&tmp_fence, *oldest_fence);

            /* Unlock the mutex before waiting. */
            simple_mtx_unlock(&aws->bo_fence_lock);
            amdgpu_fence_wait(tmp_fence, OS_TIMEOUT_INFINITE, false);
            amdgpu_fence_reference(&tmp_fence, NULL);
            simple_mtx_lock(&aws->bo_fence_lock);
         }

         /* Remove the idle fence from the ring. */
         amdgpu_fence_reference(oldest_fence, NULL);
      }
   }

   /* We'll accumulate sequence numbers in this structure. It automatically keeps only the latest
    * sequence number per queue and removes all older ones.
    */
   struct amdgpu_seq_no_fences seq_no_dependencies;
   memcpy(&seq_no_dependencies, &csc->seq_no_dependencies, sizeof(seq_no_dependencies));

   if (queue_type == KERNELQ) {
      /* Add a fence dependency on the previous IB if the IP has multiple physical queues to
       * make it appear as if it had only 1 queue, or if the previous IB comes from a different
       * context. The reasons are:
       * - Our BO fence tracking only supports 1 queue per IP.
       * - IBs from different contexts must wait for each other and can't execute in a random order.
       */
      struct amdgpu_fence *prev_fence =
         (struct amdgpu_fence*)queue->fences[prev_seq_no % AMDGPU_FENCE_RING_SIZE];

      /* Add a dependency on a previous fence, unless we can determine that
       * it's useless because the execution order is guaranteed.
       */
      if (prev_fence) {
         bool same_ctx = queue->last_ctx == acs->ctx;
         bool same_queue = aws->info.ip[acs->ip_type].num_queues == 1;

         if (!same_ctx || !same_queue)
            add_seq_no_to_list(aws, &seq_no_dependencies, queue_index, prev_seq_no);
      }
   }

   /* Since the kernel driver doesn't synchronize execution between different
    * rings automatically, we have to add fence dependencies manually. This gathers sequence
    * numbers from BOs and sets the next sequence number in the BOs.
    */

   /* Slab entry BOs: Add fence dependencies, update seq_no in BOs, add real buffers. */
   struct amdgpu_cs_buffer *slab_entry_buffers = csc->buffer_lists[AMDGPU_BO_SLAB_ENTRY].buffers;
   unsigned num_slab_entry_buffers = csc->buffer_lists[AMDGPU_BO_SLAB_ENTRY].num_buffers;
   unsigned initial_num_real_buffers = csc->buffer_lists[AMDGPU_BO_REAL].num_buffers;
   unsigned queue_index_bit = (queue_type == KERNELQ_ALT_FENCE) ?
      0 : BITFIELD_BIT(queue_index);

   for (unsigned i = 0; i < num_slab_entry_buffers; i++) {
      struct amdgpu_cs_buffer *buffer = &slab_entry_buffers[i];
      struct amdgpu_winsys_bo *bo = buffer->bo;

      amdgpu_add_fences_to_dependencies(aws, csc, queue_index_bit, &seq_no_dependencies, bo,
                                        buffer->usage);
      if (queue_type == KERNELQ_ALT_FENCE)
         amdgpu_fence_reference(&bo->alt_fence, csc->fence);
      else
         amdgpu_set_bo_seq_no(queue_index, bo, next_seq_no);

      /* We didn't add any slab entries into the real buffer list that will be submitted
       * to the kernel. Do it now.
       */
      struct amdgpu_cs_buffer *real_buffer =
         amdgpu_lookup_or_add_buffer(csc, &get_slab_entry_real_bo(buffer->bo)->b,
                                     &csc->buffer_lists[AMDGPU_BO_REAL], false);

      /* We need to set the usage because it determines the BO priority. */
      real_buffer->usage |= buffer->usage;
   }

   /* Sparse BOs: Add fence dependencies, update seq_no in BOs, add real buffers. */
   unsigned num_real_buffers_except_sparse = csc->buffer_lists[AMDGPU_BO_REAL].num_buffers;
   struct amdgpu_cs_buffer *sparse_buffers = csc->buffer_lists[AMDGPU_BO_SPARSE].buffers;
   unsigned num_sparse_buffers = csc->buffer_lists[AMDGPU_BO_SPARSE].num_buffers;
   bool out_of_memory = false;

   for (unsigned i = 0; i < num_sparse_buffers; i++) {
      struct amdgpu_cs_buffer *buffer = &sparse_buffers[i];
      struct amdgpu_winsys_bo *bo = buffer->bo;

      amdgpu_add_fences_to_dependencies(aws, csc, queue_index_bit, &seq_no_dependencies, bo,
                                        buffer->usage);
      if (queue_type == KERNELQ_ALT_FENCE)
         amdgpu_fence_reference(&bo->alt_fence, csc->fence);
      else
         amdgpu_set_bo_seq_no(queue_index, bo, next_seq_no);

      /* Add backing buffers of sparse buffers to the buffer list.
       *
       * This is done late, during submission, to keep the buffer list short before
       * submit, and to avoid managing fences for the backing buffers.
       */
      struct amdgpu_bo_sparse *sparse_bo = get_sparse_bo(buffer->bo);

      if (queue_type == USERQ) {
         uint64_t bo_vm_point = p_atomic_read(&sparse_bo->vm_timeline_point);
         vm_timeline_point = MAX2(vm_timeline_point, bo_vm_point);
      }

      simple_mtx_lock(&sparse_bo->commit_lock);
      list_for_each_entry(struct amdgpu_sparse_backing, backing, &sparse_bo->backing, list) {
         /* We can directly add the buffer here, because we know that each
          * backing buffer occurs only once.
          */
         struct amdgpu_cs_buffer *real_buffer =
            amdgpu_do_add_buffer(csc, &backing->bo->b, &csc->buffer_lists[AMDGPU_BO_REAL], true);
         if (!real_buffer) {
            fprintf(stderr, "%s: failed to add sparse backing buffer\n", __func__);
            simple_mtx_unlock(&sparse_bo->commit_lock);
            r = -ENOMEM;
            out_of_memory = true;
         }

         real_buffer->usage = buffer->usage;
      }
      simple_mtx_unlock(&sparse_bo->commit_lock);
   }

   /* Real BOs: Add fence dependencies, update seq_no in BOs except sparse backing BOs. */
   unsigned num_real_buffers = csc->buffer_lists[AMDGPU_BO_REAL].num_buffers;
   struct amdgpu_cs_buffer *real_buffers = csc->buffer_lists[AMDGPU_BO_REAL].buffers;
   struct drm_amdgpu_bo_list_entry *bo_list;
   /* BO dependency management depends on the queue mode:
    * - kernel queue: BO used by the submit are passed to the kernel in a
    *   drm_amdgpu_bo_list_entry list. The inter-process synchronization is handled
    *   automatically by the kernel; intra-process sync is handled by Mesa.
    * - user queue: intra-process sync is similar. Inter-process sync is handled
    *   using timeline points, amdgpu_userq_wait (before a submit) and
    *   amdgpu_userq_signal (after a submit).
    */
   unsigned num_shared_buf_write;
   unsigned num_shared_buf_read;
   /* Store write handles in the begining and read handles at the end in shared_buf_kms_handles.
    * If usage is read and write then store the handle in write list.
    */
   uint32_t *shared_buf_kms_handles;
   if (queue_type != USERQ) {
      bo_list = (struct drm_amdgpu_bo_list_entry *)
         alloca(num_real_buffers * sizeof(struct drm_amdgpu_bo_list_entry));
   } else {
      num_shared_buf_write = 0;
      num_shared_buf_read = 0;
      shared_buf_kms_handles = (uint32_t*)alloca(num_real_buffers * sizeof(uint32_t));
   }
   unsigned i;

   for (i = 0; i < initial_num_real_buffers; i++) {
      struct amdgpu_cs_buffer *buffer = &real_buffers[i];
      struct amdgpu_winsys_bo *bo = buffer->bo;

      amdgpu_add_fences_to_dependencies(aws, csc, queue_index_bit, &seq_no_dependencies, bo,
                                        buffer->usage);
      if (queue_type == KERNELQ_ALT_FENCE)
         amdgpu_fence_reference(&bo->alt_fence, csc->fence);
      else
         amdgpu_set_bo_seq_no(queue_index, bo, next_seq_no);

      if (queue_type != USERQ) {
         amdgpu_add_to_kernel_bo_list(&bo_list[i], bo, buffer->usage);
      } else {
         vm_timeline_point = MAX2(vm_timeline_point, get_real_bo(bo)->vm_timeline_point);

         if (!get_real_bo(bo)->is_shared)
            continue;

         if (buffer->usage & RADEON_USAGE_WRITE) {
            shared_buf_kms_handles[num_shared_buf_write] = get_real_bo(bo)->kms_handle;
            num_shared_buf_write++;
         } else {
            num_shared_buf_read++;
            shared_buf_kms_handles[num_real_buffers - num_shared_buf_read] =
               get_real_bo(bo)->kms_handle;
         }
      }
   }

   /* These are backing buffers of slab entries. Don't add their fence dependencies. */
   for (; i < num_real_buffers_except_sparse; i++) {
      struct amdgpu_cs_buffer *buffer = &real_buffers[i];
      struct amdgpu_winsys_bo *bo = buffer->bo;

      if (queue_type == KERNELQ_ALT_FENCE)
         get_real_bo_reusable_slab(bo)->b.b.slab_has_busy_alt_fences = true;
      else
         amdgpu_set_bo_seq_no(queue_index, bo, next_seq_no);

      if (queue_type != USERQ) {
         amdgpu_add_to_kernel_bo_list(&bo_list[i], bo, buffer->usage);
      } else {
         vm_timeline_point = MAX2(vm_timeline_point, get_real_bo(bo)->vm_timeline_point);

         if (!get_real_bo(bo)->is_shared)
            continue;

         if (buffer->usage & RADEON_USAGE_WRITE) {
            shared_buf_kms_handles[num_shared_buf_write] = get_real_bo(bo)->kms_handle;
            num_shared_buf_write++;
         } else {
            num_shared_buf_read++;
            shared_buf_kms_handles[num_real_buffers - num_shared_buf_read] =
               get_real_bo(bo)->kms_handle;
         }
      }
   }

   /* Sparse backing BOs are last. Don't update their fences because we don't use them. */
   for (; i < num_real_buffers; ++i) {
      struct amdgpu_cs_buffer *buffer = &real_buffers[i];

      if (queue_type != USERQ) {
         amdgpu_add_to_kernel_bo_list(&bo_list[i], buffer->bo, buffer->usage);
      } else {
         if (!get_real_bo(buffer->bo)->is_shared)
            continue;
         if (buffer->usage & RADEON_USAGE_WRITE) {
            shared_buf_kms_handles[num_shared_buf_write] =
               get_real_bo(buffer->bo)->kms_handle;
            num_shared_buf_write++;
         } else {
            num_shared_buf_read++;
            shared_buf_kms_handles[num_real_buffers - num_shared_buf_read] =
               get_real_bo(buffer->bo)->kms_handle;
         }
      }
   }

#if 0 /* Debug code. */
   printf("submit queue=%u, seq_no=%u\n", acs->queue_index, next_seq_no);

   /* Wait for all previous fences. This can be used when BO fence tracking doesn't work. */
   for (unsigned i = 0; i < AMDGPU_MAX_QUEUES; i++) {
      if (i == acs->queue_index)
         continue;

      struct pipe_fence_handle *fence = queue->fences[ws->queues[i].latest_seq_no % AMDGPU_FENCE_RING_SIZE];
      if (!fence) {
         if (i <= 1)
            printf("      queue %u doesn't have any fence at seq_no %u\n", i, ws->queues[i].latest_seq_no);
         continue;
      }

      bool valid = seq_no_dependencies.valid_fence_mask & BITFIELD_BIT(i);
      uint_seq_no old = seq_no_dependencies.seq_no[i];
      add_seq_no_to_list(aws, &seq_no_dependencies, i, aws->queues[i].latest_seq_no);
      uint_seq_no new = seq_no_dependencies.seq_no[i];

      if (!valid)
         printf("   missing dependency on queue=%u, seq_no=%u\n", i, new);
      else if (old != new)
         printf("   too old dependency on queue=%u, old=%u, new=%u\n", i, old, new);
      else
         printf("   has dependency on queue=%u, seq_no=%u\n", i, old);
   }
#endif

   /* Convert the sequence numbers we gathered to fence dependencies. */
   u_foreach_bit(i, seq_no_dependencies.valid_fence_mask) {
      struct pipe_fence_handle **fence = get_fence_from_ring(aws, &seq_no_dependencies, i);

      if (fence) {
         /* If it's idle, don't add it to the list of dependencies. */
         if (amdgpu_fence_wait(*fence, 0, false))
            amdgpu_fence_reference(fence, NULL);
         else
            add_fence_to_list(&csc->syncobj_dependencies, (struct amdgpu_fence*)*fence);
      }
   }

   if (queue_type != KERNELQ_ALT_FENCE) {
      /* Finally, add the IB fence into the fence ring of the queue. */
      amdgpu_fence_reference(&queue->fences[next_seq_no % AMDGPU_FENCE_RING_SIZE], csc->fence);
      queue->latest_seq_no = next_seq_no;
      ((struct amdgpu_fence*)csc->fence)->queue_seq_no = next_seq_no;

      /* Update the last used context in the queue. */
      amdgpu_ctx_reference(&queue->last_ctx, acs->ctx);
   }
   simple_mtx_unlock(&aws->bo_fence_lock);

#if MESA_DEBUG
   /* Prepare the buffer list. */
   if (aws->debug_all_bos) {
      /* The buffer list contains all buffers. This is a slow path that
       * ensures that no buffer is missing in the BO list.
       */
      simple_mtx_lock(&aws->global_bo_list_lock);
      if (queue_type != USERQ) {
         bo_list = (struct drm_amdgpu_bo_list_entry *)
                   alloca(aws->num_buffers * sizeof(struct drm_amdgpu_bo_list_entry));
         num_real_buffers = 0;
         list_for_each_entry(struct amdgpu_bo_real, bo, &aws->global_bo_list, global_list_item) {
            bo_list[num_real_buffers].bo_handle = bo->kms_handle;
            bo_list[num_real_buffers].bo_priority = 0;
            ++num_real_buffers;
         }
      } else {
         shared_buf_kms_handles = (uint32_t*)alloca(aws->num_buffers * sizeof(uint32_t));
         num_shared_buf_write = 0;
         num_shared_buf_read = 0;
         list_for_each_entry(struct amdgpu_bo_real, bo, &aws->global_bo_list, global_list_item) {
            shared_buf_kms_handles[num_shared_buf_write] = bo->kms_handle;
            num_shared_buf_write++;
         }
      }
      simple_mtx_unlock(&aws->global_bo_list_lock);
   }
#endif

   if (acs->ip_type == AMD_IP_GFX)
      aws->gfx_bo_list_counter += num_real_buffers;

   if (out_of_memory) {
      r = -ENOMEM;
   } else if (unlikely(acs->ctx->sw_status != PIPE_NO_RESET)) {
      r = -ECANCELED;
   } else if (unlikely(acs->noop) && acs->ip_type != AMD_IP_GFX) {
      r = 0;
   } else {
      if (queue_type != USERQ) {
         /* Submit the command buffer.
          *
          * The kernel returns -ENOMEM with many parallel processes using GDS such as test suites
          * quite often, but it eventually succeeds after enough attempts. This happens frequently
          * with dEQP using NGG streamout.
          */
         r = 0;

         do {
            /* Wait 1 ms and try again. */
            if (r == -ENOMEM)
               os_time_sleep(1000);

            r = amdgpu_cs_submit_ib_kernelq(acs, num_real_buffers, bo_list, &seq_no);
         } while (r == -ENOMEM);

         if (!r) {
            /* Success. */
            uint64_t *user_fence = NULL;

            /* Need to reserve 4 QWORD for user fence:
             *   QWORD[0]: completed fence
             *   QWORD[1]: preempted fence
             *   QWORD[2]: reset fence
             *   QWORD[3]: preempted then reset
             */
            if (has_user_fence)
               user_fence = acs->ctx->user_fence_cpu_address_base + acs->ip_type * 4;
            amdgpu_fence_submitted(csc->fence, seq_no, user_fence);
         }
      } else {
         struct amdgpu_userq *userq = &queue->userq;
         r = amdgpu_cs_submit_ib_userq(userq, acs, shared_buf_kms_handles, num_shared_buf_write,
                                       &shared_buf_kms_handles[num_real_buffers - num_shared_buf_read],
                                       num_shared_buf_read, &seq_no, vm_timeline_point);
         if (!r) {
            /* Success. */
            amdgpu_fence_submitted(csc->fence, seq_no, userq->user_fence_ptr);
         }
      }
   }

   if (unlikely(r)) {
      if (r == -ECANCELED) {
         amdgpu_ctx_set_sw_reset_status((struct radeon_winsys_ctx*)acs->ctx, PIPE_INNOCENT_CONTEXT_RESET,
                                        "amdgpu: The CS has cancelled because the context is lost. This context is innocent.\n");
      } else if (r == -ENODATA) {
         amdgpu_ctx_set_sw_reset_status((struct radeon_winsys_ctx*)acs->ctx, PIPE_GUILTY_CONTEXT_RESET,
                                        "amdgpu: The CS has cancelled because the context is lost. This context is guilty of a soft recovery.\n");
      } else if (r == -ETIME) {
         amdgpu_ctx_set_sw_reset_status((struct radeon_winsys_ctx*)acs->ctx, PIPE_GUILTY_CONTEXT_RESET,
                                        "amdgpu: The CS has cancelled because the context is lost. This context is guilty of a hard recovery.\n");
      } else {
         amdgpu_ctx_set_sw_reset_status((struct radeon_winsys_ctx*)acs->ctx,
                                        PIPE_UNKNOWN_CONTEXT_RESET,
                                        "amdgpu: The CS has been rejected, "
                                        "see dmesg for more information (%i).\n",
                                        r);
      }
   }

   /* If there was an error, signal the fence, because it won't be signalled
    * by the hardware. */
   if (r || (unlikely(acs->noop) && acs->ip_type != AMD_IP_GFX))
      amdgpu_fence_signalled(csc->fence);

   if (unlikely(aws->info.has_fw_based_shadowing && acs->mcbp_fw_shadow_chunk.flags && r == 0))
      acs->mcbp_fw_shadow_chunk.flags = 0;

   csc->error_code = r;

   /* Clear the buffer lists. */
   for (unsigned list = 0; list < ARRAY_SIZE(csc->buffer_lists); list++) {
      struct amdgpu_cs_buffer *buffers = csc->buffer_lists[list].buffers;
      unsigned num_buffers = csc->buffer_lists[list].num_buffers;

      if (list == AMDGPU_BO_REAL) {
         /* Only decrement num_active_ioctls and unref where we incremented them.
          * We did both for regular real BOs. We only incremented the refcount for sparse
          * backing BOs.
          */
         /* Regular real BOs. */
         for (unsigned i = 0; i < initial_num_real_buffers; i++) {
            p_atomic_dec(&buffers[i].bo->num_active_ioctls);
            amdgpu_winsys_bo_drop_reference(aws, buffers[i].bo);
         }

         /* Do nothing for slab BOs. */

         /* Sparse backing BOs. */
         for (unsigned i = num_real_buffers_except_sparse; i < num_buffers; i++)
            amdgpu_winsys_bo_drop_reference(aws, buffers[i].bo);
      } else {
         for (unsigned i = 0; i < num_buffers; i++) {
            p_atomic_dec(&buffers[i].bo->num_active_ioctls);
            amdgpu_winsys_bo_drop_reference(aws, buffers[i].bo);
         }
      }

      csc->buffer_lists[list].num_buffers = 0;
   }

   amdgpu_cs_context_cleanup(aws, csc);
}

/* Make sure the previous submission is completed. */
void amdgpu_cs_sync_flush(struct radeon_cmdbuf *rcs)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);

   /* Wait for any pending ioctl of this CS to complete. */
   util_queue_fence_wait(&acs->flush_completed);
}

static int amdgpu_cs_flush(struct radeon_cmdbuf *rcs,
                           unsigned flags,
                           struct pipe_fence_handle **fence)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_winsys *aws = acs->aws;
   struct amdgpu_cs_context *csc_current = amdgpu_csc_get_current(acs);
   int error_code = 0;
   uint32_t ib_pad_dw_mask = aws->info.ip[acs->ip_type].ib_pad_dw_mask;

   rcs->current.max_dw += amdgpu_cs_epilog_dws(acs);

   /* Pad the IB according to the mask. */
   switch (acs->ip_type) {
   case AMD_IP_SDMA:
      if (aws->info.gfx_level <= GFX6) {
         while (rcs->current.cdw & ib_pad_dw_mask)
            radeon_emit(rcs, 0xf0000000); /* NOP packet */
      } else {
         while (rcs->current.cdw & ib_pad_dw_mask)
            radeon_emit(rcs, SDMA_NOP_PAD);
      }
      break;
   case AMD_IP_GFX:
   case AMD_IP_COMPUTE:
      amdgpu_pad_gfx_compute_ib(aws, acs->ip_type, rcs->current.buf, &rcs->current.cdw, 0);
      if (acs->ip_type == AMD_IP_GFX)
         aws->gfx_ib_size_counter += (rcs->prev_dw + rcs->current.cdw) * 4;
      break;
   case AMD_IP_UVD:
   case AMD_IP_UVD_ENC:
      while (rcs->current.cdw & ib_pad_dw_mask)
         radeon_emit(rcs, 0x80000000); /* type2 nop packet */
      break;
   case AMD_IP_VCN_JPEG:
      if (rcs->current.cdw % 2)
         assert(0);
      while (rcs->current.cdw & ib_pad_dw_mask) {
         radeon_emit(rcs, 0x60000000); /* nop packet */
         radeon_emit(rcs, 0x00000000);
      }
      break;
   case AMD_IP_VCN_DEC:
      while (rcs->current.cdw & ib_pad_dw_mask)
         radeon_emit(rcs, 0x81ff); /* nop packet */
      break;
   default:
      break;
   }

   if (rcs->current.cdw > rcs->current.max_dw) {
      amdgpu_ctx_set_sw_reset_status(
         (struct radeon_winsys_ctx*)acs->ctx, PIPE_UNKNOWN_CONTEXT_RESET,
         "amdgpu: command stream overflowed (current: %d, max: %d)\n",
         rcs->current.cdw, rcs->current.max_dw);
      return -1;
   }

   /* If the CS is not empty or overflowed.... */
   if (likely(radeon_emitted(rcs, 0) &&
       rcs->current.cdw <= rcs->current.max_dw &&
       !(flags & RADEON_FLUSH_NOOP))) {

      /* Set IB sizes. */
      amdgpu_ib_finalize(aws, rcs, &acs->main_ib, acs->ip_type);

      /* Create a fence. */
      amdgpu_fence_reference(&csc_current->fence, NULL);
      if (acs->next_fence) {
         /* just move the reference */
         csc_current->fence = acs->next_fence;
         acs->next_fence = NULL;
      } else {
         csc_current->fence = amdgpu_fence_create(acs);
      }
      if (fence)
         amdgpu_fence_reference(fence, csc_current->fence);

      for (unsigned i = 0; i < ARRAY_SIZE(csc_current->buffer_lists); i++) {
         unsigned num_buffers = csc_current->buffer_lists[i].num_buffers;
         struct amdgpu_cs_buffer *buffers = csc_current->buffer_lists[i].buffers;

         for (unsigned j = 0; j < num_buffers; j++)
            p_atomic_inc(&buffers[j].bo->num_active_ioctls);
      }

      amdgpu_cs_sync_flush(rcs);

      csc_current->chunk_ib[IB_MAIN].ib_bytes *= 4; /* Convert from dwords to bytes. */
      if (acs->noop && acs->ip_type == AMD_IP_GFX) {
         /* Reduce the IB size and fill it with NOP to make it like an empty IB. */
         unsigned noop_dw_size = aws->info.ip[AMD_IP_GFX].ib_pad_dw_mask + 1;
         assert(csc_current->chunk_ib[IB_MAIN].ib_bytes / 4 >= noop_dw_size);

         csc_current->ib_main_addr[0] = PKT3(PKT3_NOP, noop_dw_size - 2, 0);
         csc_current->chunk_ib[IB_MAIN].ib_bytes = noop_dw_size * 4;
      }

      amdgpu_csc_swap(acs);
      csc_current = amdgpu_csc_get_current(acs);
      struct amdgpu_cs_context *csc_submitted = amdgpu_csc_get_submitted(acs);

      /* only gfx, compute and sdma queues are supported in userqueues. */
      if (aws->info.use_userq && acs->ip_type <= AMD_IP_SDMA) {
         util_queue_add_job(&aws->cs_queue, acs, &acs->flush_completed,
                            amdgpu_cs_submit_ib<USERQ>, NULL, 0);
      } else {
         util_queue_add_job(&aws->cs_queue, acs, &acs->flush_completed,
                            acs->uses_alt_fence ?
                               amdgpu_cs_submit_ib<KERNELQ_ALT_FENCE>
                               : amdgpu_cs_submit_ib<KERNELQ>,
                            NULL, 0);
      }

      if (flags & RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION)
         csc_current->secure = !csc_submitted->secure;
      else
         csc_current->secure = csc_submitted->secure;

      if (!(flags & PIPE_FLUSH_ASYNC)) {
         amdgpu_cs_sync_flush(rcs);
         error_code = csc_submitted->error_code;
      }
   } else {
      if (flags & RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION)
         csc_current->secure = !csc_current->secure;

      amdgpu_cs_context_cleanup_buffers(aws, csc_current);
      amdgpu_cs_context_cleanup(aws, csc_current);
   }

   memset(csc_current->buffer_indices_hashlist, -1, sizeof(acs->buffer_indices_hashlist));

   amdgpu_get_new_ib(aws, rcs, &acs->main_ib, acs);

   if (acs->preamble_ib_bo) {
      amdgpu_cs_add_buffer(rcs, acs->preamble_ib_bo,
                           RADEON_USAGE_READ | RADEON_PRIO_IB, (radeon_bo_domain)0);
   }

   if (acs->ip_type == AMD_IP_GFX)
      aws->num_gfx_IBs++;
   else if (acs->ip_type == AMD_IP_SDMA)
      aws->num_sdma_IBs++;

   return error_code;
}

static bool amdgpu_bo_is_referenced(struct radeon_cmdbuf *rcs,
                                    struct pb_buffer_lean *_buf,
                                    unsigned usage)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   struct amdgpu_winsys_bo *bo = (struct amdgpu_winsys_bo*)_buf;

   return amdgpu_bo_is_referenced_by_cs_with_usage(acs, bo, usage);
}

static void amdgpu_cs_set_mcbp_reg_shadowing_va(struct radeon_cmdbuf *rcs,uint64_t regs_va,
                                                                   uint64_t csa_va)
{
   struct amdgpu_cs *acs = amdgpu_cs(rcs);
   acs->mcbp_fw_shadow_chunk.shadow_va = regs_va;
   acs->mcbp_fw_shadow_chunk.csa_va = csa_va;
   acs->mcbp_fw_shadow_chunk.gds_va = 0;
   acs->mcbp_fw_shadow_chunk.flags = AMDGPU_CS_CHUNK_CP_GFX_SHADOW_FLAGS_INIT_SHADOW;
}

static void amdgpu_winsys_fence_reference(struct radeon_winsys *rws,
                                          struct pipe_fence_handle **dst,
                                          struct pipe_fence_handle *src)
{
   amdgpu_fence_reference(dst, src);
}

void amdgpu_cs_init_functions(struct amdgpu_screen_winsys *sws)
{
   sws->base.ctx_create = amdgpu_ctx_create;
   sws->base.ctx_destroy = amdgpu_ctx_destroy;
   sws->base.ctx_set_sw_reset_status = amdgpu_ctx_set_sw_reset_status;
   sws->base.ctx_query_reset_status = amdgpu_ctx_query_reset_status;
   sws->base.cs_create = amdgpu_cs_create;
   sws->base.cs_setup_preemption = amdgpu_cs_setup_preemption;
   sws->base.cs_destroy = amdgpu_cs_destroy;
   sws->base.cs_add_buffer = amdgpu_cs_add_buffer;
   sws->base.cs_validate = amdgpu_cs_validate;
   sws->base.cs_check_space = amdgpu_cs_check_space;
   sws->base.cs_get_buffer_list = amdgpu_cs_get_buffer_list;
   sws->base.cs_flush = amdgpu_cs_flush;
   sws->base.cs_get_next_fence = amdgpu_cs_get_next_fence;
   sws->base.cs_is_buffer_referenced = amdgpu_bo_is_referenced;
   sws->base.cs_sync_flush = amdgpu_cs_sync_flush;
   sws->base.cs_add_fence_dependency = amdgpu_cs_add_fence_dependency;
   sws->base.cs_add_syncobj_signal = amdgpu_cs_add_syncobj_signal;
   sws->base.cs_get_ip_type = amdgpu_cs_get_ip_type;
   sws->base.fence_wait = amdgpu_fence_wait_rel_timeout;
   sws->base.fence_reference = amdgpu_winsys_fence_reference;
   sws->base.fence_import_syncobj = amdgpu_fence_import_syncobj;
   sws->base.fence_import_sync_file = amdgpu_fence_import_sync_file;
   sws->base.fence_export_sync_file = amdgpu_fence_export_sync_file;
   sws->base.export_signalled_sync_file = amdgpu_export_signalled_sync_file;

   if (sws->aws->info.has_fw_based_shadowing)
      sws->base.cs_set_mcbp_reg_shadowing_va = amdgpu_cs_set_mcbp_reg_shadowing_va;
}
