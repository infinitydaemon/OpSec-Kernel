/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_descriptor_table.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"

#include <sys/mman.h>

static VkResult
nvk_descriptor_table_grow_locked(struct nvk_device *dev,
                                 struct nvk_descriptor_table *table,
                                 uint32_t new_alloc)
{
   struct nvkmd_mem *new_mem;
   uint32_t *new_free_table;
   VkResult result;

   assert(new_alloc > table->alloc && new_alloc <= table->max_alloc);

   const uint32_t new_mem_size = new_alloc * table->desc_size;
   result = nvkmd_dev_alloc_mapped_mem(dev->nvkmd, &dev->vk.base,
                                       new_mem_size, 256,
                                       NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_WR,
                                       &new_mem);
   if (result != VK_SUCCESS)
      return result;

   if (table->mem) {
      assert(new_mem_size >= table->mem->size_B);
      memcpy(new_mem->map, table->mem->map, table->mem->size_B);
      nvkmd_mem_unref(table->mem);
   }
   table->mem = new_mem;

   const size_t new_free_table_size = new_alloc * sizeof(uint32_t);
   new_free_table = vk_realloc(&dev->vk.alloc, table->free_table,
                               new_free_table_size, 4,
                               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (new_free_table == NULL) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "Failed to allocate image descriptor free table");
   }
   table->free_table = new_free_table;

   table->alloc = new_alloc;

   return VK_SUCCESS;
}

VkResult
nvk_descriptor_table_init(struct nvk_device *dev,
                          struct nvk_descriptor_table *table,
                          uint32_t descriptor_size,
                          uint32_t min_descriptor_count,
                          uint32_t max_descriptor_count)
{
   memset(table, 0, sizeof(*table));
   VkResult result;

   simple_mtx_init(&table->mutex, mtx_plain);

   assert(util_is_power_of_two_nonzero(min_descriptor_count));
   assert(util_is_power_of_two_nonzero(max_descriptor_count));

   table->desc_size = descriptor_size;
   table->alloc = 0;
   table->max_alloc = max_descriptor_count;
   table->next_desc = 0;
   table->free_count = 0;

   result = nvk_descriptor_table_grow_locked(dev, table, min_descriptor_count);
   if (result != VK_SUCCESS) {
      nvk_descriptor_table_finish(dev, table);
      return result;
   }

   return VK_SUCCESS;
}

void
nvk_descriptor_table_finish(struct nvk_device *dev,
                            struct nvk_descriptor_table *table)
{
   if (table->mem != NULL)
      nvkmd_mem_unref(table->mem);
   vk_free(&dev->vk.alloc, table->free_table);
   simple_mtx_destroy(&table->mutex);
}

#define NVK_IMAGE_DESC_INVALID

static VkResult
nvk_descriptor_table_alloc_locked(struct nvk_device *dev,
                                  struct nvk_descriptor_table *table,
                                  uint32_t *index_out)
{
   VkResult result;

   if (table->free_count > 0) {
      *index_out = table->free_table[--table->free_count];
      return VK_SUCCESS;
   }

   if (table->next_desc < table->alloc) {
      *index_out = table->next_desc++;
      return VK_SUCCESS;
   }

   if (table->next_desc >= table->max_alloc) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "Descriptor table not large enough");
   }

   result = nvk_descriptor_table_grow_locked(dev, table, table->alloc * 2);
   if (result != VK_SUCCESS)
      return result;

   assert(table->next_desc < table->alloc);
   *index_out = table->next_desc++;

   return VK_SUCCESS;
}

static VkResult
nvk_descriptor_table_add_locked(struct nvk_device *dev,
                                struct nvk_descriptor_table *table,
                                const void *desc_data, size_t desc_size,
                                uint32_t *index_out)
{
   VkResult result = nvk_descriptor_table_alloc_locked(dev, table, index_out);
   if (result != VK_SUCCESS)
      return result;

   void *map = (char *)table->mem->map + (*index_out * table->desc_size);

   assert(desc_size == table->desc_size);
   memcpy(map, desc_data, table->desc_size);

   return VK_SUCCESS;
}


VkResult
nvk_descriptor_table_add(struct nvk_device *dev,
                         struct nvk_descriptor_table *table,
                         const void *desc_data, size_t desc_size,
                         uint32_t *index_out)
{
   simple_mtx_lock(&table->mutex);
   VkResult result = nvk_descriptor_table_add_locked(dev, table, desc_data,
                                                     desc_size, index_out);
   simple_mtx_unlock(&table->mutex);

   return result;
}

void
nvk_descriptor_table_remove(struct nvk_device *dev,
                            struct nvk_descriptor_table *table,
                            uint32_t index)
{
   simple_mtx_lock(&table->mutex);

   void *map = (char *)table->mem->map + (index * table->desc_size);
   memset(map, 0, table->desc_size);

   /* Sanity check for double-free */
   assert(table->free_count < table->alloc);
   for (uint32_t i = 0; i < table->free_count; i++)
      assert(table->free_table[i] != index);

   table->free_table[table->free_count++] = index;

   simple_mtx_unlock(&table->mutex);
}
