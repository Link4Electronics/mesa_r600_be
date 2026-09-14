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

#ifndef TERAKAN_VK_BUFFER_H
#define TERAKAN_VK_BUFFER_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"

#include "util/macros.h"
#include "vk_buffer.h"
#include "vk_buffer_view.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_vk_buffer {
   struct vk_buffer vk;

   struct terakan_bo const * bo;
   uint64_t va;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_vk_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

/* For #MemoryIntegrity, and also handling `VK_WHOLE_SIZE`.
 *
 * Returns the element count in the range. Alignment is not handled by this function.
 *
 * For Vulkan commands accepting the size in bytes, pass 1 as `element_bytes`. If the range in bytes
 * further needs to be converted to elements, divide the result by the element size externally.
 *
 * For Vulkan commands accepting the size in elements (such as buffer view creation), this function,
 * to avoid potential overflow when converting from elements to bytes via multiplication, allows the
 * actual size of an element to be provided as `element_bytes`.
 */
static inline VkDeviceSize
terakan_vk_buffer_clamp_range(struct terakan_vk_buffer const * const buffer,
                              VkDeviceSize const offset_bytes, VkDeviceSize const size_elements,
                              VkDeviceSize const element_bytes)
{
   assert(element_bytes != 0);
   /* Also safely treat buffers not bound to device memory as null buffers. */
   if (buffer == NULL || unlikely(buffer->bo == NULL || offset_bytes > buffer->vk.size)) {
      return 0;
   }
   return MIN2(size_elements, (buffer->vk.size - offset_bytes) / element_bytes);
}

struct terakan_vk_buffer_view {
   struct vk_buffer_view vk;

   struct terakan_bo const * bo;

   struct terakan_srv_descriptor srv;

   struct terakan_color_descriptor color;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_vk_buffer_view, vk.base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_BUFFER_H */
