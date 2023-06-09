/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#include "pvr_bo.h"
#include "pvr_debug.h"
#include "pvr_dump.h"
#include "pvr_private.h"
#include "pvr_types.h"
#include "pvr_util.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "util/rb_tree.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "vk_alloc.h"
#include "vk_log.h"

struct pvr_bo_store {
   struct rb_tree tree;
   simple_mtx_t mutex;
   uint32_t size;
};

struct pvr_bo_store_entry {
   struct rb_node node;
   struct pvr_bo bo;
};

#define entry_from_node(node_) \
   container_of(node_, struct pvr_bo_store_entry, node)
#define entry_from_bo(bo_) container_of(bo_, struct pvr_bo_store_entry, bo)

static inline int pvr_dev_addr_cmp(const pvr_dev_addr_t a,
                                   const pvr_dev_addr_t b)
{
   const uint64_t addr_a = a.addr;
   const uint64_t addr_b = b.addr;

   if (addr_a < addr_b)
      return 1;
   else if (addr_a > addr_b)
      return -1;
   else
      return 0;
}

/* Borrowed from pandecode. Using this comparator allows us to lookup intervals
 * in the RB-tree without storing extra information.
 */
static inline int pvr_bo_store_entry_cmp_key(const struct rb_node *node,
                                             const void *const key)
{
   const struct pvr_winsys_vma *const vma = entry_from_node(node)->bo.vma;
   const pvr_dev_addr_t addr = *(const pvr_dev_addr_t *)key;

   if (addr.addr >= vma->dev_addr.addr &&
       addr.addr < (vma->dev_addr.addr + vma->size)) {
      return 0;
   }

   return pvr_dev_addr_cmp(vma->dev_addr, addr);
}

static inline int pvr_bo_store_entry_cmp(const struct rb_node *const a,
                                         const struct rb_node *const b)
{
   return pvr_dev_addr_cmp(entry_from_node(a)->bo.vma->dev_addr,
                           entry_from_node(b)->bo.vma->dev_addr);
}

VkResult pvr_bo_store_create(struct pvr_device *device)
{
   struct pvr_bo_store *store;

   if (!PVR_IS_DEBUG_SET(TRACK_BOS)) {
      device->bo_store = NULL;
      return VK_SUCCESS;
   }

   store = vk_alloc(&device->vk.alloc,
                    sizeof(*store),
                    8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!store)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   rb_tree_init(&store->tree);
   store->size = 0;
   simple_mtx_init(&store->mutex, mtx_plain);

   device->bo_store = store;

   return VK_SUCCESS;
}

void pvr_bo_store_destroy(struct pvr_device *device)
{
   struct pvr_bo_store *store = device->bo_store;

   if (likely(!store))
      return;

   if (unlikely(!rb_tree_is_empty(&store->tree))) {
      debug_warning("Non-empty BO store destroyed; dump follows");
      pvr_bo_store_dump(device);
   }

   simple_mtx_destroy(&store->mutex);

   vk_free(&device->vk.alloc, store);

   device->bo_store = NULL;
}

static void pvr_bo_store_insert(struct pvr_bo_store *const store,
                                struct pvr_bo *const bo)
{
   if (likely(!store))
      return;

   simple_mtx_lock(&store->mutex);
   rb_tree_insert(&store->tree,
                  &entry_from_bo(bo)->node,
                  pvr_bo_store_entry_cmp);
   store->size++;
   simple_mtx_unlock(&store->mutex);
}

static void pvr_bo_store_remove(struct pvr_bo_store *const store,
                                struct pvr_bo *const bo)
{
   if (likely(!store))
      return;

   simple_mtx_lock(&store->mutex);
   rb_tree_remove(&store->tree, &entry_from_bo(bo)->node);
   store->size--;
   simple_mtx_unlock(&store->mutex);
}

struct pvr_bo *pvr_bo_store_lookup(struct pvr_device *const device,
                                   const pvr_dev_addr_t addr)
{
   struct pvr_bo_store *const store = device->bo_store;
   struct rb_node *node;

   if (unlikely(!store))
      return NULL;

   simple_mtx_lock(&store->mutex);
   node = rb_tree_search(&store->tree, &addr, pvr_bo_store_entry_cmp_key);
   simple_mtx_unlock(&store->mutex);

   if (!node)
      return NULL;

   return &entry_from_node(node)->bo;
}

static void pvr_bo_dump_line(struct pvr_dump_ctx *const ctx,
                             const struct pvr_bo *bo,
                             const uint32_t index,
                             const uint32_t nr_bos_log10)
{
   static const char *const pretty_sizes[64 + 1] = {
      "",        "1 B",     "2 B",     "4 B",     "8 B",     "16 B",
      "32 B",    "64 B",    "128 B",   "256 B",   "512 B",   "1 KiB",
      "2 KiB",   "4 KiB",   "8 KiB",   "16 KiB",  "32 KiB",  "64 KiB",
      "128 KiB", "256 KiB", "512 KiB", "1 MiB",   "2 MiB",   "4 MiB",
      "8 MiB",   "16 MiB",  "32 MiB",  "64 MiB",  "128 MiB", "256 MiB",
      "512 MiB", "1 GiB",   "2 GiB",   "4 GiB",   "8 GiB",   "16 GiB",
      "32 GiB",  "64 GiB",  "128 GiB", "256 GiB", "512 GiB", "1 TiB",
      "2 TiB",   "4 TiB",   "8 TiB",   "16 TiB",  "32 TiB",  "64 TiB",
      "128 TiB", "256 TiB", "512 TiB", "1 PiB",   "2 PiB",   "4 PiB",
      "8 PiB",   "16 PiB",  "32 PiB",  "64 PiB",  "128 PiB", "256 PiB",
      "512 PiB", "1 EiB",   "2 EiB",   "4 EiB",   "8 EiB",
   };

   const uint64_t size = bo->vma->size;
   const uint32_t size_log2 =
      util_is_power_of_two_or_zero64(size) ? util_last_bit(size) : 0;

   pvr_dump_println(ctx,
                    "[%0*" PRIu32 "] " PVR_DEV_ADDR_FMT " -> %*p "
                    "(%s%s0x%" PRIx64 " bytes)",
                    nr_bos_log10,
                    index,
                    bo->vma->dev_addr.addr,
                    (int)sizeof(void *) * 2 + 2, /* nr hex digits + 0x prefix */
                    bo->bo->map,
                    pretty_sizes[size_log2],
                    size_log2 ? ", " : "",
                    size);
}

bool pvr_bo_store_dump(struct pvr_device *const device)
{
   struct pvr_bo_store *const store = device->bo_store;
   const uint32_t nr_bos = store->size;
   const uint32_t nr_bos_log10 = u32_dec_digits(nr_bos);
   struct pvr_dump_ctx ctx;
   uint32_t bo_idx = 0;

   if (unlikely(!store)) {
      debug_warning("Requested BO store dump, but no BO store is present.");
      return false;
   }

   pvr_dump_begin(&ctx, stderr, "BO STORE", 1);

   pvr_dump_println(&ctx, "Dumping %" PRIu32 " BO store entries...", nr_bos);

   pvr_dump_indent(&ctx);
   rb_tree_foreach_safe (struct pvr_bo_store_entry, entry, &store->tree, node) {
      pvr_bo_dump_line(&ctx, &entry->bo, bo_idx++, nr_bos_log10);
   }
   pvr_dump_dedent(&ctx);

   return pvr_dump_end(&ctx);
}

void pvr_bo_list_dump(struct pvr_dump_ctx *const ctx,
                      const struct list_head *const bo_list,
                      const uint32_t nr_bos)
{
   const uint32_t real_nr_bos = nr_bos ? nr_bos : list_length(bo_list);
   const uint32_t nr_bos_log10 = u32_dec_digits(real_nr_bos);
   uint32_t bo_idx = 0;

   list_for_each_entry (struct pvr_bo, bo, bo_list, link) {
      pvr_bo_dump_line(ctx, bo, bo_idx++, nr_bos_log10);
   }
}

static uint32_t pvr_bo_alloc_to_winsys_flags(uint64_t flags)
{
   uint32_t ws_flags = 0;

   if (flags & (PVR_BO_ALLOC_FLAG_CPU_ACCESS | PVR_BO_ALLOC_FLAG_CPU_MAPPED))
      ws_flags |= PVR_WINSYS_BO_FLAG_CPU_ACCESS;

   if (flags & PVR_BO_ALLOC_FLAG_GPU_UNCACHED)
      ws_flags |= PVR_WINSYS_BO_FLAG_GPU_UNCACHED;

   if (flags & PVR_BO_ALLOC_FLAG_PM_FW_PROTECT)
      ws_flags |= PVR_WINSYS_BO_FLAG_PM_FW_PROTECT;

   if (flags & PVR_BO_ALLOC_FLAG_ZERO_ON_ALLOC)
      ws_flags |= PVR_WINSYS_BO_FLAG_ZERO_ON_ALLOC;

   return ws_flags;
}

static inline struct pvr_bo *
pvr_bo_alloc_bo(const struct pvr_device *const device)
{
   size_t size;
   void *ptr;

   if (unlikely(device->bo_store))
      size = sizeof(struct pvr_bo_store_entry);
   else
      size = sizeof(struct pvr_bo);

   ptr =
      vk_alloc(&device->vk.alloc, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (unlikely(!ptr))
      return NULL;

   if (unlikely(device->bo_store))
      return &((struct pvr_bo_store_entry *)ptr)->bo;
   else
      return (struct pvr_bo *)ptr;
}

static inline void pvr_bo_free_bo(const struct pvr_device *const device,
                                  struct pvr_bo *const bo)
{
   void *ptr;

   if (unlikely(device->bo_store))
      ptr = entry_from_bo(bo);
   else
      ptr = bo;

   vk_free(&device->vk.alloc, ptr);
}

/**
 * \brief Helper interface to allocate a GPU buffer and map it to both host and
 * device virtual memory. Host mapping is conditional and is controlled by
 * flags.
 *
 * \param[in] device      Logical device pointer.
 * \param[in] heap        Heap to allocate device virtual address from.
 * \param[in] size        Size of buffer to allocate.
 * \param[in] alignment   Required alignment of the allocation. Must be a power
 *                        of two.
 * \param[in] flags       Controls allocation, CPU and GPU mapping behavior
 *                        using PVR_BO_ALLOC_FLAG_*.
 * \param[out] pvr_bo_out On success output buffer is returned in this pointer.
 * \return VK_SUCCESS on success, or error code otherwise.
 *
 * \sa #pvr_bo_free()
 */
VkResult pvr_bo_alloc(struct pvr_device *device,
                      struct pvr_winsys_heap *heap,
                      uint64_t size,
                      uint64_t alignment,
                      uint64_t flags,
                      struct pvr_bo **const pvr_bo_out)
{
   const uint32_t ws_flags = pvr_bo_alloc_to_winsys_flags(flags);
   struct pvr_bo *pvr_bo;
   pvr_dev_addr_t addr;
   VkResult result;

   pvr_bo = pvr_bo_alloc_bo(device);
   if (!pvr_bo)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = device->ws->ops->buffer_create(device->ws,
                                           size,
                                           alignment,
                                           PVR_WINSYS_BO_TYPE_GPU,
                                           ws_flags,
                                           &pvr_bo->bo);
   if (result != VK_SUCCESS)
      goto err_free_bo;

   if (flags & PVR_BO_ALLOC_FLAG_CPU_MAPPED) {
      void *map = device->ws->ops->buffer_map(pvr_bo->bo);
      if (!map) {
         result = VK_ERROR_MEMORY_MAP_FAILED;
         goto err_buffer_destroy;
      }

      if (flags & PVR_BO_ALLOC_FLAG_ZERO_ON_ALLOC)
         VG(VALGRIND_MAKE_MEM_DEFINED(map, pvr_bo->bo->size));
   }

   pvr_bo->vma = device->ws->ops->heap_alloc(heap, size, alignment);
   if (!pvr_bo->vma) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto err_buffer_unmap;
   }

   addr = device->ws->ops->vma_map(pvr_bo->vma, pvr_bo->bo, 0, size);
   if (!addr.addr) {
      result = VK_ERROR_MEMORY_MAP_FAILED;
      goto err_heap_free;
   }

   pvr_bo_store_insert(device->bo_store, pvr_bo);
   *pvr_bo_out = pvr_bo;

   return VK_SUCCESS;

err_heap_free:
   device->ws->ops->heap_free(pvr_bo->vma);

err_buffer_unmap:
   if (flags & PVR_BO_ALLOC_FLAG_CPU_MAPPED)
      device->ws->ops->buffer_unmap(pvr_bo->bo);

err_buffer_destroy:
   device->ws->ops->buffer_destroy(pvr_bo->bo);

err_free_bo:
   pvr_bo_free_bo(device, pvr_bo);

   return result;
}

/**
 * \brief Interface to map the buffer into host virtual address space.
 *
 * Buffer should have been created with the #PVR_BO_ALLOC_FLAG_CPU_ACCESS
 * flag. It should also not already be mapped or it should have been unmapped
 * using #pvr_bo_cpu_unmap() before mapping again.
 *
 * \param[in] device Logical device pointer.
 * \param[in] pvr_bo Buffer to map.
 * \return Valid host virtual address on success, or NULL otherwise.
 *
 * \sa #pvr_bo_alloc(), #PVR_BO_ALLOC_FLAG_CPU_MAPPED
 */
void *pvr_bo_cpu_map(struct pvr_device *device, struct pvr_bo *pvr_bo)
{
   assert(!pvr_bo->bo->map);

   return device->ws->ops->buffer_map(pvr_bo->bo);
}

/**
 * \brief Interface to unmap the buffer from host virtual address space.
 *
 * Buffer should have a valid mapping, created either using #pvr_bo_cpu_map() or
 * by passing #PVR_BO_ALLOC_FLAG_CPU_MAPPED flag to #pvr_bo_alloc() at
 * allocation time.
 *
 * Buffer can be remapped using #pvr_bo_cpu_map().
 *
 * \param[in] device Logical device pointer.
 * \param[in] pvr_bo Buffer to unmap.
 */
void pvr_bo_cpu_unmap(struct pvr_device *device, struct pvr_bo *pvr_bo)
{
   struct pvr_winsys_bo *bo = pvr_bo->bo;

   assert(bo->map);

#if defined(HAVE_VALGRIND)
   if (!bo->vbits)
      bo->vbits = vk_alloc(&device->vk.alloc,
                           bo->size,
                           8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (bo->vbits)
      VALGRIND_GET_VBITS(bo->map, bo->vbits, bo->size);
   else
      mesa_loge("Failed to alloc vbits storage; expect bad valgrind results.");
#endif /* defined(HAVE_VALGRIND) */

   device->ws->ops->buffer_unmap(bo);
}

/**
 * \brief Interface to free the buffer object.
 *
 * \param[in] device Logical device pointer.
 * \param[in] pvr_bo Buffer to free.
 *
 * \sa #pvr_bo_alloc()
 */
void pvr_bo_free(struct pvr_device *device, struct pvr_bo *pvr_bo)
{
   if (!pvr_bo)
      return;

#if defined(HAVE_VALGRIND)
   vk_free(&device->vk.alloc, pvr_bo->bo->vbits);
#endif /* defined(HAVE_VALGRIND) */

   pvr_bo_store_remove(device->bo_store, pvr_bo);

   device->ws->ops->vma_unmap(pvr_bo->vma);
   device->ws->ops->heap_free(pvr_bo->vma);

   if (pvr_bo->bo->map)
      device->ws->ops->buffer_unmap(pvr_bo->bo);

   device->ws->ops->buffer_destroy(pvr_bo->bo);

   pvr_bo_free_bo(device, pvr_bo);
}

#if defined(HAVE_VALGRIND)
void *pvr_bo_cpu_map_unchanged(struct pvr_device *device, struct pvr_bo *pvr_bo)
{
   void *ret = pvr_bo_cpu_map(device, pvr_bo);
   if (ret)
      VALGRIND_SET_VBITS(pvr_bo->bo->map, pvr_bo->bo->vbits, pvr_bo->bo->size);

   return ret;
}
#endif /* defined(HAVE_VALGRIND) */
