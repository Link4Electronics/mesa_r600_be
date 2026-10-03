/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_DEVICE_H
#define TERAKAN_DEVICE_H

#include "meta/terakan_meta.h"
#include "terakan_bo.h"
#include "terakan_device_enums.h"
#include "terakan_physical_device.h"
#include "terakan_queue.h"
#include "terakan_shader.h"

#include "c11/threads.h"
#include "util/log.h"
#include "util/macros.h"
#include "amd_family.h"
#include "vk_device.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_device_platform_fn {
   struct terakan_bo_platform_fn const * bo;

   struct terakan_queue_platform_fn const * queue;

   void (*destroy)(struct terakan_device * device);
};

/* Partially implemented by the platform specialization. */
struct terakan_device {
   struct vk_device vk;

   struct vk_device_dispatch_table command_dispatch_table;

   struct terakan_device_platform_fn const * platform_fn;

   size_t bo_reference_size;
   size_t bo_reference_alignment;

   uint32_t last_bo_creation_number;

   /* TODO(Triang3l): Don't actually allocate BOs for placeholders. Currently, however, the actual
    * ring placeholder BO will be used if there are ring base setting packets in the submission's
    * command buffers, but the command buffers don't specify that they actually need a nonzero
    * amount of ring buffer memory.
    */
   struct terakan_bo * reference_placeholder_bos[TERAKAN_QUEUE_BO_REFERENCE_PLACEHOLDER_INDEX_COUNT];

   /* BO for data that needs to be discarded on the graphics queue:
    * - EVENT_WRITE_EOP fence (4 bytes).
    * - CP DMA size misalignment (TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT * 2).
    */
   struct terakan_bo * gfx_discard_bo;

   /* `CB_IMMED` buffers of `terakan_physical_device_chip_info::uav_immediate_size_elements`
    * elements each for all possible UAV element sizes, because CB multiplies the invocation index
    * by the element size, so using the same buffer with different element sizes would cause
    * collisions.
    * `uav_immediate_va_shr8` index is log2 of the element size in bytes.
    * Because the same buffer is used for all UAVs (of a given element size), one invocation must
    * never have multiple UAV operations returning a value in flight.
    */
   struct terakan_bo * uav_immediate_bo;
   uint32_t uav_immediate_va_shr8[4 + 1];

   /* Buffer holding the intermediate result in accumulation of query results from multiple indirect
    * buffers.
    */
   struct terakan_bo * query_accumulator_bo;

   struct terakan_bo * meta_shaders_bo;
   uint32_t meta_shaders_empty_fetch_va_shr8;
   struct terakan_shader_static meta_shaders[TERAKAN_META_SHADER_COUNT];
   struct terakan_shader_sqk_usage meta_shader_sqk_usage[TERAKAN_META_SHADER_COUNT];

   /* Mutex and condition variable for terakan_sync_completion timeline semaphore value updates and
    * controlling submission completion waits.
    */
   mtx_t completion_mutex;
   cnd_t completion_condition;

   /* Whether awaiting submission completion isn't possible anymore, as a result of device
    * destruction or submission-related device loss.
    * Protected by completion_mutex, broadcast completion_condition when setting.
    */
   bool completion_lost;

   /* Optimal command buffer submission sizes clamped to the maximum sizes for queues in their
    * families and aligned to the size alignment requirements.
    */
   struct terakan_queue_submission_size command_buffer_submission_size_gfx;

   /* Each queue is optional, if requested by the application. */
   struct terakan_queue * queue_graphics;
};

VK_DEFINE_HANDLE_CASTS(terakan_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static inline struct terakan_physical_device *
terakan_device_physical_device(struct terakan_device const * const device)
{
   return container_of(device->vk.physical, struct terakan_physical_device, vk);
}

#define TERAKAN_DEVICE_DEFINE_OBJECT_SHORTCUTS(object, get_device)                                 \
   static inline struct terakan_device * terakan_##object##_device(                                \
      struct terakan_##object const * const object)                                                \
   {                                                                                               \
      return (get_device);                                                                         \
   }                                                                                               \
                                                                                                   \
   static inline struct terakan_physical_device * terakan_##object##_physical_device(              \
      struct terakan_##object const * const object)                                                \
   {                                                                                               \
      return terakan_device_physical_device(terakan_##object##_device(object));                    \
   }

/* Various parts of the driver core may allocate (and subsequently free) memory with a NULL client
 * allocator context pointer, which is expected to be treated by the implementation as allocating in
 * the client allocator context of the device itself.
 *
 * May be called during device initialization.
 */
void * terakan_device_host_allocate(struct terakan_device const * device,
                                    void const * client_allocator_context, size_t size,
                                    size_t alignment, enum terakan_device_allocation_scope scope);

/* The memory pointer may be NULL, in which case the function must do nothing.
 *
 * The client allocator context must be usable for freeing allocations created in the client
 * allocator context used for the command that allocated the memory (like how in Vulkan, the
 * `VkAllocationCallbacks` for `vkDestroy...` must be usable for freeing allocations made by the
 * `vkCreate...` for the object).
 *
 * May be called during device initialization.
 */
void terakan_device_host_free(struct terakan_device const * device,
                              void const * client_allocator_context, void * memory);

/* Object that can be passed to VK_LOG_OBJS during or after the creation of the device. */
static inline struct vk_object_base const *
terakan_device_log_obj(struct terakan_device const * const device)
{
   if (device->vk.base.client_visible) {
      return &device->vk.base;
   }
   return &terakan_device_physical_device(device)->vk.base.instance->base;
}

/* The logging functions may be called during device initialization and may allocate host memory via
 * or similarly to `terakan_device_host_allocate`.
 */
void terakan_device_log(struct terakan_device const * device, enum mesa_log_level level,
                        char const * file, int line, char const * message);
#define TERAKAN_DEVICE_LOG(device, level, message)                                                 \
   terakan_device_log(device, level, __FILE__, __LINE__, message)
/* Uses `alloca` for `%m` and `strerror` reentrancy, avoid logging potentially long messages. */
void terakan_device_logv(struct terakan_device const * device, enum mesa_log_level level,
                         char const * file, int line, char const * format, va_list arguments);
void terakan_device_logf(struct terakan_device const * device, enum mesa_log_level level,
                         char const * file, int line, char const * format, ...);
#define TERAKAN_DEVICE_LOGF(device, level, format, ...)                                            \
   terakan_device_logf(device, level, __FILE__, __LINE__, format, ##__VA_ARGS__)

void terakan_device_finish(struct terakan_device * device);

VkResult terakan_device_init(struct terakan_device * device,
                             struct terakan_physical_device * physical_device,
                             VkDeviceCreateInfo const * create_info,
                             VkAllocationCallbacks const * allocator,
                             struct terakan_device_platform_fn const * platform_fn_static,
                             size_t bo_reference_size, size_t bo_reference_alignment);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_DEVICE_H */
