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

#include "terakan_vk_image.h"

#include "terakan_device.h"
#include "terakan_format.h"
#include "terakan_vk_entrypoints.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/compiler.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

VKAPI_ATTR void VKAPI_CALL
terakan_vk_DestroyImageView(VkDevice const deviceHandle, VkImageView const imageView,
                            VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_vk_image_view * const image_view = terakan_vk_image_view_from_handle(imageView);
   if (image_view == NULL) {
      return;
   }

   vk_image_view_finish(&image_view->vk);

   vk_free2(&terakan_device_from_handle(deviceHandle)->vk.alloc, pAllocator, image_view);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreateImageView(VkDevice const deviceHandle,
                           VkImageViewCreateInfo const * const pCreateInfo,
                           VkAllocationCallbacks const * const pAllocator,
                           VkImageView * const pView)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_format_info view_format_info;
   if (!terakan_format_info_get(pCreateInfo->format, &view_format_info)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "Image view format %s is not supported",
                       vk_Format_to_str(pCreateInfo->format));
   }

   struct terakan_vk_image_view * const image_view =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_vk_image_view),
                alignof(struct terakan_vk_image_view), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image_view == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_image_view_init(&device->vk, &image_view->vk, pCreateInfo);

   struct terakan_image const * const image = terakan_image_from_handle(pCreateInfo->image);

   image_view->bo = image->bo;

   struct terakan_image_descriptor_create_info descriptor_create_info = {
      .image = image,
      .subresource_range =
         {
            .base_mip_level = pCreateInfo->subresourceRange.baseMipLevel,
            .max_level_count = pCreateInfo->subresourceRange.levelCount,
            .base_z_or_array_layer = pCreateInfo->subresourceRange.baseArrayLayer,
            .max_depth_or_layer_count = 1,
         },
   };

   /* Using a view with both depth and stencil aspects for sampled or storage image descriptors is
    * not valid according to the `VkImageSubresourceRange` documentation, but very easy to produce,
    * so create depth-only SRV and color descriptors unless only stencil is needed.
    */
   unsigned view_format_main_aspect_index = 0, image_format_main_aspect_index = 0;
   if ((pCreateInfo->subresourceRange.aspectMask &
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == VK_IMAGE_ASPECT_STENCIL_BIT) {
      view_format_main_aspect_index =
         terakan_format_aspect_index(view_format_info.aspect_map, VK_IMAGE_ASPECT_STENCIL_BIT, 0);
      image_format_main_aspect_index =
         terakan_format_aspect_index(image->format_info.aspect_map, VK_IMAGE_ASPECT_STENCIL_BIT, 0);
   }
   /* TODO(Triang3l): For multi-planar images, select the correct plane for the main aspect, and
    * also create SRV descriptors for all planes if requested (the aspect mask is COLOR).
    */

   descriptor_create_info.view_format =
      view_format_info.aspect_formats[view_format_main_aspect_index];
   descriptor_create_info.image_aspect_index = image_format_main_aspect_index;

   /* Always using array hardware view dimensionalities for non-array Vulkan view types with the
    * array layer zeroed in shaders, because the hardware doesn't support non-array 1D and 2D views
    * for `VkImage`s with multiple slices.
    * Also, with `VK_IMAGE_CREATE_ALIAS_SINGLE_LAYER_DESCRIPTOR_BIT_KHR`, whether the shader binding
    * is arrayed must override whether the view is arrayed, and layer count queries (returning 0 for
    * non-array dimensionalities in the hardware) and access robustness must work as expected by the
    * shader binding type.
    */
   uint32_t srv_dimensionality = V_030000_SQ_TEX_DIM_2D_ARRAY;
   uint32_t color_resource_type = V_028C70_TEXTURE2DARRAY;
   switch (pCreateInfo->viewType) {
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      FALLTHROUGH;
   case VK_IMAGE_VIEW_TYPE_1D:
      srv_dimensionality = V_030000_SQ_TEX_DIM_1D_ARRAY;
      color_resource_type = V_028C70_TEXTURE1DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      FALLTHROUGH;
   case VK_IMAGE_VIEW_TYPE_2D:
      srv_dimensionality = image->extent.samples_log2 != 0u ? V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA
                                                            : V_030000_SQ_TEX_DIM_2D_ARRAY;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_3D:
      descriptor_create_info.subresource_range.base_z_or_array_layer = 0;
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         u_minify(image->extent.slices, pCreateInfo->subresourceRange.baseMipLevel);
      srv_dimensionality = V_030000_SQ_TEX_DIM_3D;
      color_resource_type = V_028C70_TEXTURE3D;
      break;
   case VK_IMAGE_VIEW_TYPE_CUBE:
      descriptor_create_info.subresource_range.max_depth_or_layer_count = 6;
      srv_dimensionality = V_030000_SQ_TEX_DIM_CUBEMAP;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      srv_dimensionality = V_030000_SQ_TEX_DIM_CUBEMAP;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   default:
      assert(!"Unsupported image view type");
   }

   /* TODO(Triang3l): Verify sanitization for cubemaps. */
   if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
          image, &descriptor_create_info.subresource_range,
          srv_dimensionality == V_030000_SQ_TEX_DIM_CUBEMAP))) {
      image_view->srv = (struct terakan_srv_descriptor){};
      image_view->color = (struct terakan_color_descriptor){};
      image_view->color_meta = (struct terakan_color_meta_descriptor){};
      image_view->depth_stencil = (struct terakan_depth_stencil_descriptor){};
   } else {
      if (!terakan_image_create_srv_descriptor(&descriptor_create_info, srv_dimensionality,
                                               &pCreateInfo->components, &image_view->srv)) {
         image_view->srv = (struct terakan_srv_descriptor){};
      }
      if (!descriptor_create_info.view_format.supports_cb_color ||
          terakan_image_create_color_descriptor(&descriptor_create_info, color_resource_type,
                                                &image_view->color, &image_view->color_meta) == 0) {
         image_view->color = (struct terakan_color_descriptor){};
         image_view->color_meta = (struct terakan_color_meta_descriptor){};
      }
      /* According to the `VkFramebufferCreateInfo` and `VkRenderingInfo` reference, the image view
       * `aspectMask` is ignored for depth / stencil attachments, so toggle depth and stencil based
       * purely on the format.
       */
      enum terascale_r8xx_depth_format view_depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
      bool view_has_stencil = false;
      if (!terascale_get_r8xx_depth_stencil_format(vk_format_to_pipe_format(pCreateInfo->format),
                                                   &view_depth_format, &view_has_stencil) ||
          !terakan_image_create_depth_stencil_descriptor(image, view_depth_format, view_has_stencil,
                                                         &descriptor_create_info.subresource_range,
                                                         &image_view->depth_stencil)) {
         image_view->depth_stencil = (struct terakan_depth_stencil_descriptor){};
      }
   }

   *pView = terakan_vk_image_view_to_handle(image_view);
   return VK_SUCCESS;
}
