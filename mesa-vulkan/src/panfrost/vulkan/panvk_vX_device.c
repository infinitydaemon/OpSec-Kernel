/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_image.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

#include "panvk_cmd_buffer.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_instance.h"
#include "panvk_macros.h"
#include "panvk_physical_device.h"
#include "panvk_priv_bo.h"
#include "panvk_queue.h"

#include "genxml/decode.h"
#include "genxml/gen_macros.h"

#include "kmod/pan_kmod.h"
#include "pan_props.h"
#include "pan_samples.h"

static void *
panvk_kmod_zalloc(const struct pan_kmod_allocator *allocator, size_t size,
                  bool transient)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   void *obj = vk_zalloc(vkalloc, size, 8,
                         transient ? VK_SYSTEM_ALLOCATION_SCOPE_COMMAND
                                   : VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   /* We force errno to -ENOMEM on host allocation failures so we can properly
    * report it back as VK_ERROR_OUT_OF_HOST_MEMORY. */
   errno = obj ? 0 : -ENOMEM;

   return obj;
}

static void
panvk_kmod_free(const struct pan_kmod_allocator *allocator, void *data)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   return vk_free(vkalloc, data);
}

static void
panvk_device_init_mempools(struct panvk_device *dev)
{
   struct panvk_pool_properties rw_pool_props = {
      .create_flags = 0,
      .slab_size = 16 * 1024,
      .label = "Device RW memory pool",
      .owns_bos = false,
      .needs_locking = true,
      .prealloc = false,
   };

   panvk_pool_init(&dev->mempools.rw, dev, NULL, &rw_pool_props);

   struct panvk_pool_properties exec_pool_props = {
      .create_flags = PAN_KMOD_BO_FLAG_EXECUTABLE,
      .slab_size = 16 * 1024,
      .label = "Device executable memory pool (shaders)",
      .owns_bos = false,
      .needs_locking = true,
      .prealloc = false,
   };

   panvk_pool_init(&dev->mempools.exec, dev, NULL, &exec_pool_props);
}

static void
panvk_device_cleanup_mempools(struct panvk_device *dev)
{
   panvk_pool_cleanup(&dev->mempools.rw);
   panvk_pool_cleanup(&dev->mempools.exec);
}

/* Always reserve the lower 32MB. */
#define PANVK_VA_RESERVE_BOTTOM 0x2000000ull

VkResult
panvk_per_arch(create_device)(struct panvk_physical_device *physical_device,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice)
{
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   VkResult result;
   struct panvk_device *device;

   device = vk_zalloc2(&instance->vk.alloc, pAllocator, sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;

   /* For secondary command buffer support, overwrite any command entrypoints
    * in the main device-level dispatch table with
    * vk_cmd_enqueue_unless_primary_Cmd*.
    */
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);

   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &panvk_per_arch(device_entrypoints), false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &wsi_device_entrypoints, false);

   /* Populate our primary cmd_dispatch table. */
   vk_device_dispatch_table_from_entrypoints(
      &device->cmd_dispatch, &panvk_per_arch(device_entrypoints), true);
   vk_device_dispatch_table_from_entrypoints(&device->cmd_dispatch,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &device->cmd_dispatch, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   /* Must be done after vk_device_init() because this function memset(0) the
    * whole struct.
    */
   device->vk.command_dispatch_table = &device->cmd_dispatch;
   device->vk.command_buffer_ops = &panvk_per_arch(cmd_buffer_ops);
   device->vk.shader_ops = &panvk_per_arch(device_shader_ops);

   device->kmod.allocator = (struct pan_kmod_allocator){
      .zalloc = panvk_kmod_zalloc,
      .free = panvk_kmod_free,
      .priv = &device->vk.alloc,
   };
   device->kmod.dev =
      pan_kmod_dev_create(dup(physical_device->kmod.dev->fd),
                          PAN_KMOD_DEV_FLAG_OWNS_FD, &device->kmod.allocator);

   if (!device->kmod.dev) {
      result = vk_errorf(instance, panvk_errno_to_vk_error(), "cannot create device");
      goto fail_dev;
   }

   if (instance->debug_flags &
       (PANVK_DEBUG_TRACE | PANVK_DEBUG_SYNC | PANVK_DEBUG_DUMP))
      device->debug.decode_ctx = pandecode_create_context(false);

   /* 32bit address space, with the lower 32MB reserved. We clamp
    * things so it matches kmod VA range limitations.
    */
   uint64_t user_va_start = panfrost_clamp_to_usable_va_range(
      device->kmod.dev, PANVK_VA_RESERVE_BOTTOM);
   uint64_t user_va_end =
      panfrost_clamp_to_usable_va_range(device->kmod.dev, 1ull << 32);

   device->kmod.vm =
      pan_kmod_vm_create(device->kmod.dev, PAN_KMOD_VM_FLAG_AUTO_VA,
                         user_va_start, user_va_end - user_va_start);

   if (!device->kmod.vm) {
      result = vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_vm;
   }

   panvk_device_init_mempools(device);

   device->tiler_heap = panvk_priv_bo_create(
      device, 128 * 1024 * 1024,
      PAN_KMOD_BO_FLAG_NO_MMAP | PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (!device->tiler_heap) {
      result = vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_priv_bo;
   }

   device->sample_positions =
      panvk_priv_bo_create(device, panfrost_sample_positions_buffer_size(), 0,
                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (!device->sample_positions) {
      result = vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_priv_bo;
   }

   panfrost_upload_sample_positions(device->sample_positions->addr.host);

   vk_device_set_drm_fd(&device->vk, device->kmod.dev->fd);

   result = panvk_per_arch(blend_shader_cache_init)(device);

   if (result != VK_SUCCESS)
      goto fail_priv_bo;

   panvk_per_arch(meta_init)(device);

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] =
         vk_alloc(&device->vk.alloc,
                  queue_create->queueCount * sizeof(struct panvk_queue), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct panvk_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result = panvk_per_arch(queue_init)(device, &device->queues[qfi][q], q,
                                             queue_create);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   *pDevice = panvk_device_to_handle(device);
   return VK_SUCCESS;

fail:
   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_per_arch(meta_cleanup)(device);
   panvk_per_arch(blend_shader_cache_cleanup)(device);

fail_priv_bo:
   panvk_priv_bo_unref(device->sample_positions);
   panvk_priv_bo_unref(device->tiler_heap);
   panvk_device_cleanup_mempools(device);
   pan_kmod_vm_destroy(device->kmod.vm);

fail_vm:
   pan_kmod_dev_destroy(device->kmod.dev);

fail_dev:
   vk_free(&device->vk.alloc, device);
   return result;
}

void
panvk_per_arch(destroy_device)(struct panvk_device *device,
                               const VkAllocationCallbacks *pAllocator)
{
   if (!device)
      return;

   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_per_arch(meta_cleanup)(device);
   panvk_per_arch(blend_shader_cache_cleanup)(device);
   panvk_priv_bo_unref(device->tiler_heap);
   panvk_priv_bo_unref(device->sample_positions);
   panvk_device_cleanup_mempools(device);
   pan_kmod_vm_destroy(device->kmod.vm);

   if (device->debug.decode_ctx)
      pandecode_destroy_context(device->debug.decode_ctx);

   pan_kmod_dev_destroy(device->kmod.dev);
   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(GetRenderAreaGranularity)(VkDevice device,
                                         VkRenderPass renderPass,
                                         VkExtent2D *pGranularity)
{
   *pGranularity = (VkExtent2D){32, 32};
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(GetRenderingAreaGranularityKHR)(
   VkDevice _device, const VkRenderingAreaInfoKHR *pRenderingAreaInfo,
   VkExtent2D *pGranularity)
{
   *pGranularity = (VkExtent2D){32, 32};
}
