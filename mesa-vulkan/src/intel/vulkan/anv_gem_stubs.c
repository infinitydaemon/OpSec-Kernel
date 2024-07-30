/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <sys/mman.h>
#include <sys/syscall.h>

#include "util/anon_file.h"
#include "anv_private.h"

static void
stub_gem_close(struct anv_device *device, struct anv_bo *bo)
{
   close(bo->gem_handle);
}

static uint32_t
stub_gem_create(struct anv_device *device,
                const struct intel_memory_class_instance **regions,
                uint16_t num_regions, uint64_t size,
                enum anv_bo_alloc_flags alloc_flags,
                uint64_t *actual_size)
{
   int fd = os_create_anonymous_file(size, "fake bo");
   if (fd == -1)
      return 0;

   assert(fd != 0);

   *actual_size = size;
   return fd;
}

static void *
stub_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,
              uint64_t size, void *placed_addr)
{
   return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->gem_handle,
               offset);
}

static VkResult
stub_queue_exec_locked(struct anv_queue *queue,
                       uint32_t wait_count,
                       const struct vk_sync_wait *waits,
                       uint32_t cmd_buffer_count,
                       struct anv_cmd_buffer **cmd_buffers,
                       uint32_t signal_count,
                       const struct vk_sync_signal *signals,
                       struct anv_query_pool *perf_query_pool,
                       uint32_t perf_query_pass,
                       struct anv_utrace_submit *utrace_submit)
{
   return VK_ERROR_UNKNOWN;
}

static VkResult
stub_queue_exec_async(struct anv_async_submit *submit,
                      uint32_t wait_count,
                      const struct vk_sync_wait *waits,
                      uint32_t signal_count,
                      const struct vk_sync_signal *signals)
{
   return VK_ERROR_UNKNOWN;
}

static uint32_t
stub_bo_alloc_flags_to_bo_flags(struct anv_device *device,
                                enum anv_bo_alloc_flags alloc_flags)
{
   return 0;
}

static uint32_t
stub_gem_create_userptr(struct anv_device *device, void *mem, uint64_t size)
{
   int fd = os_create_anonymous_file(size, "fake bo");
   if (fd == -1)
      return 0;

   assert(fd != 0);

   return fd;
}

int
anv_gem_wait(struct anv_device *device, uint32_t gem_handle, int64_t *timeout_ns)
{
   return 0;
}

int
anv_gem_set_tiling(struct anv_device *device,
                   uint32_t gem_handle, uint32_t stride, uint32_t tiling)
{
   return 0;
}

int
anv_gem_get_tiling(struct anv_device *device, uint32_t gem_handle)
{
   return 0;
}

int
anv_gem_handle_to_fd(struct anv_device *device, uint32_t gem_handle)
{
   unreachable("Unused");
}

uint32_t
anv_gem_fd_to_handle(struct anv_device *device, int fd)
{
   unreachable("Unused");
}

VkResult
anv_gem_import_bo_alloc_flags_to_bo_flags(struct anv_device *device,
                                          struct anv_bo *bo,
                                          enum anv_bo_alloc_flags alloc_flags,
                                          uint32_t *bo_flags)
{
   return VK_SUCCESS;
}

static VkResult
stub_vm_bind(struct anv_device *device, struct anv_sparse_submission *submit,
             enum anv_vm_bind_flags flags)
{
   return VK_SUCCESS;
}

static VkResult
stub_vm_bind_bo(struct anv_device *device, struct anv_bo *bo)
{
   return VK_SUCCESS;
}

const struct anv_kmd_backend *anv_stub_kmd_backend_get(void)
{
   static const struct anv_kmd_backend stub_backend = {
      .gem_create = stub_gem_create,
      .gem_create_userptr = stub_gem_create_userptr,
      .gem_close = stub_gem_close,
      .gem_mmap = stub_gem_mmap,
      .vm_bind = stub_vm_bind,
      .vm_bind_bo = stub_vm_bind_bo,
      .vm_unbind_bo = stub_vm_bind_bo,
      .queue_exec_locked = stub_queue_exec_locked,
      .queue_exec_async = stub_queue_exec_async,
      .bo_alloc_flags_to_bo_flags = stub_bo_alloc_flags_to_bo_flags,
   };
   return &stub_backend;
}
