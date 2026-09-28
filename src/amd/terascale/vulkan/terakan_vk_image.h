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

#include "vk_image.h"

#ifdef __cplusplus
extern "C" {
#endif

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
