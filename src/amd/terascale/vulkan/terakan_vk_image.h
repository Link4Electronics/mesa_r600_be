/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_VK_IMAGE_H
#define TERAKAN_VK_IMAGE_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_image.h"

#include "util/macros.h"
#include "vk_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Should be used instead of filling `terakan_image_descriptor_subresource_range` directly to avoid
 * integer overflow for #MemoryIntegrity, as `terakan_image_descriptor_subresource_range` may have
 * different widths of the fields.
 *
 * `max_level_count` can be `VK_REMAINING_MIP_LEVELS`.
 * `max_depth_or_layer_count` can be `VK_REMAINING_ARRAY_LAYERS`.
 */
static inline struct terakan_image_descriptor_subresource_range
terakan_vk_image_descriptor_subresource_range(uint32_t const base_mip_level,
                                              uint32_t const max_level_count,
                                              uint32_t const base_z_or_array_layer,
                                              uint32_t const max_depth_or_layer_count)
{
   /* `VK_REMAINING_MIP_LEVELS` and `VK_REMAINING_ARRAY_LAYERS` are `UINT32_MAX`, and thus are just
    * handled as maximum values that are never smaller than the actual respective counts in the
    * image.
    *
    * Clamping the bases to the exclusive upper bounds, so out-of-bounds bases fail sanitization.
    */
   return (struct terakan_image_descriptor_subresource_range){
      .base_mip_level =
         (uint8_t)MIN2(base_mip_level, (uint32_t)(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_LOG2 + 1u)),
      .max_level_count =
         (uint8_t)MIN2(max_level_count, (uint32_t)(TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_LOG2 + 1u)),
      .base_z_or_array_layer =
         (uint16_t)MIN2(base_z_or_array_layer, (uint32_t)TERAKAN_IMAGE_MAX_SLICES),
      .max_depth_or_layer_count =
         (uint16_t)MIN2(max_depth_or_layer_count, (uint32_t)TERAKAN_IMAGE_MAX_SLICES),
   };
}

struct terakan_vk_image_view {
   struct vk_image_view vk;

   struct terakan_bo const * bo;

   struct terakan_srv_descriptor srv;

   /* Used for both RTVs and UAVs, so `INFO.RAT` is not specifically defined. */
   struct terakan_color_descriptor color;
   struct terakan_color_meta_descriptor color_meta;

   struct terakan_depth_stencil_descriptor depth_stencil;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_vk_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_IMAGE_H */
