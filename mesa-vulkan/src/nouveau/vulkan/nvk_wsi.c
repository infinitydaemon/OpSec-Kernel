/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_wsi.h"
#include "nvk_instance.h"
#include "nvkmd/nvkmd.h"
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

VkResult
nvk_init_wsi(struct nvk_physical_device *pdev)
{
   VkResult result;

   struct wsi_device_options wsi_options = {
      .sw_device = false
   };
   result = wsi_device_init(&pdev->wsi_device,
                            nvk_physical_device_to_handle(pdev),
                            nvk_wsi_proc_addr, &pdev->vk.instance->alloc,
                            nvkmd_pdev_get_drm_primary_fd(pdev->nvkmd),
                            &nvk_physical_device_instance(pdev)->dri_options,
                            &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   pdev->wsi_device.supports_scanout = false;
   pdev->wsi_device.supports_modifiers =
      pdev->vk.supported_extensions.table.EXT_image_drm_format_modifier;

   pdev->vk.wsi_device = &pdev->wsi_device;

   return result;
}

void
nvk_finish_wsi(struct nvk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}
