/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "util/xmlconfig.h"
#include "hk_private.h"
#include "vk_instance.h"

struct hk_instance {
   struct vk_instance vk;

   struct driOptionCache dri_options;
   struct driOptionCache available_dri_options;

   uint8_t driver_build_sha[20];
   uint32_t force_vk_vendor;
};

VK_DEFINE_HANDLE_CASTS(hk_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)
