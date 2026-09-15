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

#ifndef TERAKAN_VK_DESCRIPTOR_SET_H
#define TERAKAN_VK_DESCRIPTOR_SET_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_vk_descriptor_set_layout.h"
#include "terakan_vk_sampler.h"

#include "util/macros.h"
#include "util/u_math.h"
#include "util/vma.h"
#include "vk_object.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_VK_DESCRIPTOR_SET_SRV_DYNAMIC_OFFSET_BUFFER_SIZE_LO_WORD                           \
   (5 + (TERAKAN_SRV_BUFFER_PRIORITY_WORD == 5))

struct terakan_vk_descriptor_set_srv {
   /* If the BO is NULL, the descriptor contents should be ignored. */
   struct terakan_bo const * bo;

   /* For consistency of out-of-bounds access behavior with kcache and UAV access, for uniform and
    * storage buffers, the addressable size (but not the size for `OpArrayLength`) stored in the
    * descriptor is rounded up to `robustUniform/StorageBufferAccessSizeAlignment`.
    * It's safe to round the size up from the submission validation perspective, because BO size is
    * rounded up when allocating device memory to take this into account.
    * The base address is aligned to `minUniform/StorageBufferOffsetAlignment`:
    * - Without dynamic offset: when writing;
    * - With dynamic offset: when binding (stored with byte granularity in the descriptor set).
    *
    * For #MemoryIntegrity clamping (without dereferencing the buffer object, which might have
    * already been destroyed, if it's not known that the descriptor will be statically used, when
    * binding the set), bindings with dynamic offset have the remaining `VkBuffer` size since the
    * base in bytes in:
    * [31: 0] - srv[TERAKAN_VK_DESCRIPTOR_SET_SRV_DYNAMIC_OFFSET_BUFFER_SIZE_LO_WORD]
    * [39:32] - srv[7][7:0]
    */
   struct terakan_srv_descriptor srv;
};

struct terakan_vk_descriptor_set_sampler {
   /* The sampler constant itself, not a pointer to the sampler object, is stored, because
    * descriptors in bindings not statically used by the pipeline can be undefined according to the
    * "Allocation of Descriptor Sets" section of the Vulkan specification, and thus descriptors of
    * unused samplers may point to samplers that have already been destroyed.
    */
   struct terakan_sampler_descriptor sampler;
   bool unnormalized_coordinates;
};

static inline void
terakan_vk_descriptor_set_sampler_init(
   struct terakan_vk_descriptor_set_sampler * const sampler_descriptor,
   struct terakan_vk_sampler const * const sampler)
{
   sampler_descriptor->sampler = sampler->sampler;
   sampler_descriptor->unnormalized_coordinates = sampler->unnormalized_coordinates;
}

struct terakan_vk_descriptor_set_uav {
   /* If the BO is NULL, the descriptor contents should be ignored. */
   struct terakan_bo const * bo;

   /* For storage buffers, the size stored in the descriptor is rounded up to
    * `robustStorageBufferAccessSizeAlignment`, as it's specified in 32-bit elements.
    * It's safe to round the size up from the submission validation perspective, because BO size is
    * rounded up when allocating device memory to take this into account.
    * The base address is aligned to `minStorageBufferOffsetAlignment`:
    * - Without dynamic offset: when writing;
    * - With dynamic offset: when binding (stored with byte granularity in `base` and `view` in the
    *   descriptor set).
    *
    * For #MemoryIntegrity clamping (without dereferencing the buffer object, which might have
    * already been destroyed, if it's not known that the descriptor will be statically used, when
    * binding the set), bindings with dynamic offset have the remaining `VkBuffer` size since the
    * base in bytes in:
    * [31: 0] - pitch
    * [39:32] - slice
    */
   struct terakan_color_descriptor color;
};

#define TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT                                             \
   MAX3(alignof(struct terakan_vk_descriptor_set_srv),                                             \
        alignof(struct terakan_vk_descriptor_set_sampler),                                         \
        alignof(struct terakan_vk_descriptor_set_uav))

static_assert(
   (sizeof(struct terakan_vk_descriptor_set_srv) %
    TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT) == 0 &&
      (sizeof(struct terakan_vk_descriptor_set_sampler) %
       TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT) == 0 &&
      (sizeof(struct terakan_vk_descriptor_set_uav) %
       TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT) == 0,
   "Assuming that descriptors of different types can be tightly packed in descriptor sets "
   "arbitrarily, and placing descriptors of any different types next to each other won't cause "
   "them to become misaligned. If this expectation stops being true, add padding to the "
   "underaligned structures using `alignas`, or wrap all descriptor structures in "
   "`#pragma pack(push, 4)`.");

struct terakan_vk_descriptor_set {
   struct vk_object_base base;

   /* The part readable by writing and binding. */

   struct terakan_vk_descriptor_set_layout * layout;

   /* Note that descriptor memory is zeroed on allocation in the driver, so undefined descriptors,
    * as long as Vulkan valid usage rules are respected, should still be valid
    * `terakan_vk_descriptor_set_*` structures of the hardware descriptor types expected at the
    * given locations in the descriptor set memory, representing either null descriptors (including
    * ones produced by zero initialization) or non-null descriptors.
    *
    * This means that assumptions (such as in assertions, though preferably not those that can
    * potentially result in bypassing #MemoryIntegrity sanitization) about the consistency of the
    * register values can still be made in the driver logic, except for anything involving
    * dereferencing the BO pointer (as it must not be dereferenced until it's known that the
    * descriptor is statically used in the current draw or dispatch, because the BO for an undefined
    * descriptor may have already been destroyed).
    *
    * With mutable descriptor types, however, the memory of undefined SRV and UAV descriptors may
    * still contain SRV or UAV (respectively) descriptors of other `VkDescriptorType`s (except for
    * types with a dynamic offset, which can't be represented by a mutable descriptor type), so the
    * memory of an undefined buffer descriptor may still store a texture descriptor, or vice versa.
    */
   char * descriptors;

   /* The part accessed exclusively by allocation or freeing. */

   uint32_t pool_allocated_prev;
   uint32_t pool_allocated_or_freed_next;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_vk_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct terakan_vk_descriptor_pool {
   struct vk_object_base base;

   size_t descriptor_memory_size;
   char * descriptor_memory;
   struct util_vma_heap descriptor_memory_heap;
   size_t descriptor_memory_unallocated;

   struct terakan_vk_descriptor_set * sets;
   uint32_t max_sets;

   /* Lists are terminated with UINT32_MAX. */

   uint32_t sets_allocated;
   uint32_t allocated_sets_head;

   uint32_t sets_freed;
   uint32_t freed_sets_head;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_vk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_DESCRIPTOR_SET_H */
