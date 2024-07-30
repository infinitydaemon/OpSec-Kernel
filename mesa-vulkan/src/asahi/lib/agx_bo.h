/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2019 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "util/list.h"

struct agx_device;

enum agx_alloc_type {
   AGX_ALLOC_REGULAR = 0,
   AGX_ALLOC_MEMMAP = 1,
   AGX_ALLOC_CMDBUF = 2,
   AGX_NUM_ALLOC,
};

enum agx_bo_flags {
   /* BO is shared across processes (imported or exported) and therefore cannot
    * be cached locally
    */
   AGX_BO_SHARED = 1 << 0,

   /* BO must be allocated in the low 32-bits of VA space */
   AGX_BO_LOW_VA = 1 << 1,

   /* BO is executable */
   AGX_BO_EXEC = 1 << 2,

   /* BO should be mapped write-back on the CPU (else, write combine) */
   AGX_BO_WRITEBACK = 1 << 3,

   /* BO could potentially be shared (imported or exported) and therefore cannot
    * be allocated as private
    */
   AGX_BO_SHAREABLE = 1 << 4,

   /* BO is read-only from the GPU side
    */
   AGX_BO_READONLY = 1 << 5,
};

struct agx_ptr {
   /* If CPU mapped, CPU address. NULL if not mapped */
   void *cpu;

   /* If type REGULAR, mapped GPU address */
   uint64_t gpu;
};

struct agx_bo {
   /* Must be first for casting */
   struct list_head bucket_link;

   /* Used to link the BO to the BO cache LRU list. */
   struct list_head lru_link;

   /* The time this BO was used last, so we can evict stale BOs. */
   time_t last_used;

   enum agx_alloc_type type;

   /* Creation attributes */
   enum agx_bo_flags flags;
   size_t size;
   size_t align;

   /* Mapping */
   struct agx_ptr ptr;

   /* Index unique only up to type, process-local */
   uint32_t handle;

   /* DMA-BUF fd clone for adding fences to imports/exports */
   int prime_fd;

   /* Current writer, if any (queue in upper 32 bits, syncobj in lower 32 bits) */
   uint64_t writer;

   /* Owner */
   struct agx_device *dev;

   /* Update atomically */
   int32_t refcnt;

   /* Used while decoding, marked read-only */
   bool ro;

   /* Used while decoding, mapped */
   bool mapped;

   /* For debugging */
   const char *label;

   /* virtio blob_id */
   uint32_t blob_id;
   uint32_t vbo_res_id;
};

static inline uint32_t
agx_bo_writer_syncobj(uint64_t writer)
{
   return writer;
}

static inline uint32_t
agx_bo_writer_queue(uint64_t writer)
{
   return writer >> 32;
}

static inline uint64_t
agx_bo_writer(uint32_t queue, uint32_t syncobj)
{
   return (((uint64_t)queue) << 32) | syncobj;
}

struct agx_bo *agx_bo_create_aligned(struct agx_device *dev, unsigned size,
                                     unsigned align, enum agx_bo_flags flags,
                                     const char *label);
static inline struct agx_bo *
agx_bo_create(struct agx_device *dev, unsigned size, enum agx_bo_flags flags,
              const char *label)
{
   return agx_bo_create_aligned(dev, size, 0, flags, label);
}

void agx_bo_reference(struct agx_bo *bo);
void agx_bo_unreference(struct agx_bo *bo);
struct agx_bo *agx_bo_import(struct agx_device *dev, int fd);
int agx_bo_export(struct agx_bo *bo);

void agx_bo_free(struct agx_device *dev, struct agx_bo *bo);
struct agx_bo *agx_bo_cache_fetch(struct agx_device *dev, size_t size,
                                  size_t align, uint32_t flags,
                                  const bool dontwait);
void agx_bo_cache_evict_all(struct agx_device *dev);
