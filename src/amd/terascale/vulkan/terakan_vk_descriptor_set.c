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

#include "terakan_vk_descriptor_set.h"

#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_format.h"
#include "terakan_vk_buffer.h"
#include "terakan_vk_entrypoints.h"
#include "terakan_vk_image.h"
#include "terakan_vk_sampler.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/compiler.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Returns the BO of the buffer if the descriptor data for a valid non-empty range was written.
 * If NULL is returned, no descriptor data is written.
 */
static struct terakan_bo const *
terakan_vk_descriptor_set_write_uniform_buffer(VkDescriptorBufferInfo const * const buffer_info,
                                               struct terakan_srv_descriptor * const srv_out)
{
   /* If the behavior is changed, make sure writing and binding of descriptors with a dynamic offset
    * is done consistently with the changes to descriptor creation without a dynamic offset.
    */

   struct terakan_vk_buffer const * const buffer =
      terakan_vk_buffer_from_handle(buffer_info->buffer);
   VkDeviceSize const buffer_range =
      terakan_vk_buffer_clamp_range(buffer, buffer_info->offset, buffer_info->range, 1);
   if (buffer_range == 0) {
      return NULL;
   }

   /* Enforcing SRV size and base alignment to make sure out-of-bounds access and invalid alignment
    * behavior is consistent with kcache access.
    * See the `terakan_vk_descriptor_set_srv` description.
    */
   uint32_t const va_kcache_lines =
      (uint32_t)((buffer->va + buffer_info->offset) >> TERAKAN_KCACHE_HW_LINE_BYTES_LOG2);

   *srv_out = (struct terakan_srv_descriptor){
      .srv = {
         [0] = va_kcache_lines << TERAKAN_KCACHE_HW_LINE_BYTES_LOG2,
         [1] = (uint32_t)MIN2(buffer_range - 1u, UINT32_MAX) | (TERAKAN_KCACHE_HW_LINE_BYTES - 1u),
         [2] =
            S_030008_BASE_ADDRESS_HI(va_kcache_lines >> (32 - TERAKAN_KCACHE_HW_LINE_BYTES_LOG2)) |
            S_030008_STRIDE(1),
         [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER,
      }};

   return buffer->bo;
}

/* Returns the BO of the buffer if the descriptor data for a valid non-empty range was written.
 * If NULL is returned, no descriptor data is written.
 */
static struct terakan_bo const *
terakan_vk_descriptor_set_write_storage_buffer(VkDescriptorBufferInfo const * const buffer_info,
                                               struct terakan_srv_descriptor * const srv_out,
                                               struct terakan_color_descriptor * const uav_out)
{
   /* If the behavior is changed, make sure writing and binding of descriptors with a dynamic offset
    * is done consistently with the changes to descriptor creation without a dynamic offset.
    */

   struct terakan_vk_buffer const * const buffer =
      terakan_vk_buffer_from_handle(buffer_info->buffer);
   VkDeviceSize const buffer_range =
      terakan_vk_buffer_clamp_range(buffer, buffer_info->offset, buffer_info->range, 1);
   if (buffer_range == 0) {
      return NULL;
   }

   /* Enforcing SRV size and base alignment, and clamping the range to `maxStorageBufferRange` with
    * the UAV pipe interleave alignment offset taken into account, to make sure out-of-bounds access
    * and alignment behavior is consistent with UAV access.
    * See the `terakan_vk_descriptor_set_srv` and `terakan_vk_descriptor_set_uav` descriptions.
    */
   uint64_t const va = (buffer->va + buffer_info->offset) & ~(uint64_t)(sizeof(uint32_t) - 1u);
   struct terakan_physical_device const * const physical_device =
      container_of(buffer->vk.base.device->physical, struct terakan_physical_device const, vk);
   uint32_t const buffer_bytes_clamped =
      (uint32_t)MIN2(buffer_range, physical_device->vk.properties.maxStorageBufferRange);

   *srv_out = (struct terakan_srv_descriptor){
      .srv = {
         [0] = (uint32_t)va,
         [1] = (buffer_bytes_clamped - 1u) | (uint32_t)(sizeof(uint32_t) - 1u),
         [2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(1),
         [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
         [4] = buffer_bytes_clamped,
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER,
      }};

   *uav_out = (struct terakan_color_descriptor){
      .info = S_028C70_ENDIAN(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
              S_028C70_FORMAT(TERASCALE_FORMAT_INDEX_32) |
              S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
              TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS,
      .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
   };
   terakan_color_descriptor_calculate_buffer_base_view_dim(
      uav_out, va, (buffer_bytes_clamped - 1u) / sizeof(uint32_t) + 1u, sizeof(uint32_t),
      physical_device->tiling_info.pipe_interleave_bytes_log2, false);

   return buffer->bo;
}

VKAPI_ATTR void VKAPI_CALL
terakan_vk_UpdateDescriptorSets(VkDevice const device, uint32_t const descriptorWriteCount,
                                VkWriteDescriptorSet const * const pDescriptorWrites,
                                uint32_t const descriptorCopyCount,
                                VkCopyDescriptorSet const * const pDescriptorCopies)
{
   for (uint32_t descriptor_write_index = 0; descriptor_write_index < descriptorWriteCount;
        ++descriptor_write_index) {
      VkWriteDescriptorSet const * const descriptor_write =
         &pDescriptorWrites[descriptor_write_index];

      struct terakan_vk_descriptor_set const * const dst_set =
         terakan_vk_descriptor_set_from_handle(descriptor_write->dstSet);
      struct terakan_vk_descriptor_set_layout_binding const * const dst_binding =
         &dst_set->layout->bindings[descriptor_write->dstBinding];
      /* VUID-VkWriteDescriptorSet-dstBinding-00316 "dstBinding must be a binding with a non-zero
       * descriptorCount", no need to skip bindings with zero descriptors, which are uninitialized
       * in descriptor set layouts in the driver except for the descriptor count, to get to the
       * first actually updated binding.
       */
      assert(dst_binding->descriptor_count != 0);

      uint32_t const descriptor_count = descriptor_write->descriptorCount;

      struct terakan_vk_descriptor_set_srv * const dst_srvs =
         (struct terakan_vk_descriptor_set_srv *)dst_set->descriptors + dst_binding->first_set_srv +
         descriptor_write->dstArrayElement;
      struct terakan_vk_descriptor_set_sampler * const dst_samplers =
         (struct terakan_vk_descriptor_set_sampler *)(dst_set->descriptors +
                                                      dst_set->layout
                                                         ->pool_first_sampler_offset_bytes) +
         dst_binding->first_set_sampler + descriptor_write->dstArrayElement;
      struct terakan_vk_descriptor_set_uav * const dst_uavs =
         (struct terakan_vk_descriptor_set_uav *)(dst_set->descriptors +
                                                  dst_set->layout->pool_first_uav_offset_bytes) +
         dst_binding->first_set_uav + descriptor_write->dstArrayElement;

      switch (descriptor_write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            struct terakan_vk_image_view const * const image_view =
               terakan_vk_image_view_from_handle(
                  descriptor_write->pImageInfo[descriptor_index].imageView);
            if (image_view != NULL &&
                G_028C70_FORMAT(image_view->color.info) != TERASCALE_FORMAT_INDEX_INVALID) {
               dst_uav->bo = image_view->bo;
               dst_uav->color = image_view->color;
               dst_uav->color.info |= S_028C70_RAT(true);
            } else {
               dst_uav->bo = NULL;
            }
         }
      }
         FALLTHROUGH;
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
         if ((descriptor_write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
              descriptor_write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
             dst_binding->first_immutable_sampler_or_dynamic_offset == UINT16_MAX) {
            for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
                 ++descriptor_index) {
               terakan_vk_descriptor_set_sampler_init(
                  &dst_samplers[descriptor_index],
                  terakan_vk_sampler_from_handle(
                     descriptor_write->pImageInfo[descriptor_index].sampler));
            }
         }
         if (descriptor_write->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
            for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
                 ++descriptor_index) {
               struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
               struct terakan_vk_image_view const * const image_view =
                  terakan_vk_image_view_from_handle(
                     descriptor_write->pImageInfo[descriptor_index].imageView);
               if (image_view != NULL &&
                   G_03001C_TYPE(image_view->srv.srv[7]) == V_03001C_SQ_TEX_VTX_VALID_TEXTURE) {
                  dst_srv->bo = image_view->bo;
                  dst_srv->srv = image_view->srv;
               } else {
                  dst_srv->bo = NULL;
               }
            }
         }
      } break;

      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            struct terakan_vk_buffer_view const * const buffer_view =
               terakan_vk_buffer_view_from_handle(
                  descriptor_write->pTexelBufferView[descriptor_index]);
            if (buffer_view != NULL &&
                G_028C70_FORMAT(buffer_view->color.info) != TERASCALE_FORMAT_INDEX_INVALID) {
               dst_uav->bo = buffer_view->bo;
               memcpy(&dst_uav->color, &buffer_view->color,
                      sizeof(struct terakan_color_descriptor));
            } else {
               dst_uav->bo = NULL;
            }
         }
      }
         FALLTHROUGH;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
            struct terakan_vk_buffer_view const * const buffer_view =
               terakan_vk_buffer_view_from_handle(
                  descriptor_write->pTexelBufferView[descriptor_index]);
            if (buffer_view != NULL &&
                G_03001C_TYPE(buffer_view->srv.srv[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER) {
               dst_srv->bo = buffer_view->bo;
               dst_srv->srv = buffer_view->srv;
            } else {
               dst_srv->bo = NULL;
            }
         }
      } break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
            dst_srv->bo = terakan_vk_descriptor_set_write_uniform_buffer(
               &descriptor_write->pBufferInfo[descriptor_index], &dst_srv->srv);
         }
      } break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
            VkDescriptorBufferInfo const * const buffer_info =
               &descriptor_write->pBufferInfo[descriptor_index];
            struct terakan_vk_buffer const * const buffer =
               terakan_vk_buffer_from_handle(buffer_info->buffer);
            VkDeviceSize const buffer_range =
               terakan_vk_buffer_clamp_range(buffer, buffer_info->offset, buffer_info->range, 1);
            if (buffer_range != 0) {
               /* Aligning the SRV size to make sure out-of-bounds access behavior is consistent
                * with kcache access.
                * Base alignment will be enforced when the dynamic offset is applied.
                * Also storing the remaining amount of data in the buffer for #MemoryIntegrity
                * enforcement when applying the dynamic offset.
                * See the `terakan_vk_descriptor_set_srv` description.
                */
               uint64_t const va = buffer->va + buffer_info->offset;
               VkDeviceSize const buffer_remaining_size = buffer->vk.size - buffer_info->offset;
               dst_srv->bo = buffer->bo;
               dst_srv->srv = (struct terakan_srv_descriptor){
                  .srv = {
                     [0] = (uint32_t)va,
                     [1] = (uint32_t)MIN2(buffer_range - 1u, UINT32_MAX) |
                           (TERAKAN_KCACHE_HW_LINE_BYTES - 1u),
                     [2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(1),
                     [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
                     [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER) |
                           ((buffer_remaining_size >> 32) & 0xFFu),
                     [TERAKAN_VK_DESCRIPTOR_SET_SRV_DYNAMIC_OFFSET_BUFFER_SIZE_LO_WORD] =
                        (uint32_t)buffer_remaining_size,
                     [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER,
                  }};
            } else {
               dst_srv->bo = NULL;
            }
         }
      } break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
            struct terakan_vk_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            struct terakan_bo const * const bo = terakan_vk_descriptor_set_write_storage_buffer(
               &descriptor_write->pBufferInfo[descriptor_index], &dst_srv->srv, &dst_uav->color);
            dst_srv->bo = bo;
            dst_uav->bo = bo;
         }
      } break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
         for (uint32_t descriptor_index = 0; descriptor_index < descriptor_count;
              ++descriptor_index) {
            struct terakan_vk_descriptor_set_srv * const dst_srv = &dst_srvs[descriptor_index];
            struct terakan_vk_descriptor_set_uav * const dst_uav = &dst_uavs[descriptor_index];
            VkDescriptorBufferInfo const * const buffer_info =
               &descriptor_write->pBufferInfo[descriptor_index];
            struct terakan_vk_buffer const * const buffer =
               terakan_vk_buffer_from_handle(buffer_info->buffer);
            VkDeviceSize const buffer_range =
               terakan_vk_buffer_clamp_range(buffer, buffer_info->offset, buffer_info->range, 1);
            if (buffer_range != 0) {
               /* Aligning the SRV size, and clamping the range to `maxStorageBufferRange` with the
                * UAV pipe interleave alignment offset taken into account, to make sure
                * out-of-bounds access behavior is consistent with UAV access.
                * Base alignment will be performed when the dynamic offset is applied.
                * Also storing the remaining amount of data in the buffer for #MemoryIntegrity
                * enforcement when applying the dynamic offset.
                * See the `terakan_vk_descriptor_set_srv` and `terakan_vk_descriptor_set_uav`
                * descriptions.
                */
               uint64_t const va = buffer->va + buffer_info->offset;
               struct terakan_physical_device const * const physical_device =
                  terakan_device_physical_device(terakan_device_from_handle(device));
               uint32_t const buffer_bytes_clamped = (uint32_t)MIN2(
                  buffer_range, physical_device->vk.properties.maxStorageBufferRange);
               VkDeviceSize const buffer_remaining_size = buffer->vk.size - buffer_info->offset;

               dst_srv->bo = buffer->bo;
               dst_srv->srv = (struct terakan_srv_descriptor){
                  .srv = {
                     [0] = (uint32_t)va,
                     [1] = (buffer_bytes_clamped - 1u) | (uint32_t)(sizeof(uint32_t) - 1u),
                     [2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(1),
                     [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
                     [4] = buffer_bytes_clamped,
                     [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER) |
                           ((buffer_remaining_size >> 32) & 0xFFu),
                     [TERAKAN_VK_DESCRIPTOR_SET_SRV_DYNAMIC_OFFSET_BUFFER_SIZE_LO_WORD] =
                        (uint32_t)buffer_remaining_size,
                     [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER,
                  }};

               dst_uav->bo = buffer->bo;
               dst_uav->color = (struct terakan_color_descriptor){
                  .base = (uint32_t)(va >> 8),
                  .pitch = (uint32_t)buffer_remaining_size,
                  .slice = (uint32_t)(buffer_remaining_size >> 32),
                  .view = va & 0xFFu,
                  .info = S_028C70_ENDIAN(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                               : TERASCALE_ENDIAN_SWAP_NONE) |
                          S_028C70_FORMAT(TERASCALE_FORMAT_INDEX_32) |
                          S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
                          TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS,
                  .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
                  /* Rounding the size up by truncating the inclusive last byte address to the
                   * inclusive last dword address.
                   */
                  .dim = (buffer_bytes_clamped - 1u) / sizeof(uint32_t),
               };
            } else {
               dst_srv->bo = NULL;
               dst_uav->bo = NULL;
            }
         }
      } break;

      default:
         assert(!"Unsupported descriptor type");
      }
   }

   for (uint32_t descriptor_copy_index = 0; descriptor_copy_index < descriptorCopyCount;
        ++descriptor_copy_index) {
      VkCopyDescriptorSet const * const descriptor_copy = &pDescriptorCopies[descriptor_copy_index];
      if (unlikely(descriptor_copy->descriptorCount == 0)) {
         /* There doesn't seem to be a VU rule about descriptorCount in VkCopyDescriptorSet being
          * nonzero, consider it valid, but don't bother trying to locate the first non-empty
          * binding.
          */
         continue;
      }

      /* VkCopyDescriptorSet's valid usage doesn't require that srcBinding and dstBinding themselves
       * are non-empty. However, empty bindings are completely skipped and not initialized by the
       * driver during descriptor set layout creation, so take the type and the offsets from the
       * first non-empty binding starting from srcBinding or dstBinding.
       */
      struct terakan_vk_descriptor_set const * const src_set =
         terakan_vk_descriptor_set_from_handle(descriptor_copy->srcSet);
      size_t src_binding_index = descriptor_copy->srcBinding;
      while (src_binding_index < src_set->layout->binding_count &&
             src_set->layout->bindings[src_binding_index].descriptor_count == 0) {
         ++src_binding_index;
      }
      assert(src_binding_index < src_set->layout->binding_count);
      if (unlikely(src_binding_index >= src_set->layout->binding_count)) {
         continue;
      }
      struct terakan_vk_descriptor_set_layout_binding const * const src_binding =
         &src_set->layout->bindings[src_binding_index];

      VkDescriptorType const descriptor_type = src_binding->descriptor_type;

      struct terakan_vk_descriptor_set const * const dst_set =
         terakan_vk_descriptor_set_from_handle(descriptor_copy->dstSet);
      size_t dst_binding_index = descriptor_copy->dstBinding;
      while (dst_binding_index < dst_set->layout->binding_count &&
             dst_set->layout->bindings[dst_binding_index].descriptor_count == 0) {
         ++dst_binding_index;
      }
      assert(dst_binding_index < dst_set->layout->binding_count);
      if (unlikely(dst_binding_index >= dst_set->layout->binding_count)) {
         continue;
      }
      struct terakan_vk_descriptor_set_layout_binding const * const dst_binding =
         &dst_set->layout->bindings[dst_binding_index];

      if (terakan_vk_descriptor_type_has_srv(descriptor_type)) {
         assert(terakan_vk_descriptor_type_has_srv(dst_binding->descriptor_type));
         memcpy(dst_set->descriptors +
                   sizeof(struct terakan_vk_descriptor_set_srv) *
                      (dst_binding->first_set_srv + descriptor_copy->dstArrayElement),
                src_set->descriptors +
                   sizeof(struct terakan_vk_descriptor_set_srv) *
                      (src_binding->first_set_srv + descriptor_copy->srcArrayElement),
                sizeof(struct terakan_vk_descriptor_set_srv) * descriptor_copy->descriptorCount);

         if (terakan_vk_descriptor_type_has_uav(descriptor_type)) {
            assert(terakan_vk_descriptor_type_has_uav(dst_binding->descriptor_type));
            memcpy(dst_set->descriptors + dst_set->layout->pool_first_uav_offset_bytes +
                      sizeof(struct terakan_vk_descriptor_set_uav) *
                         (dst_binding->first_set_uav + descriptor_copy->dstArrayElement),
                   src_set->descriptors + src_set->layout->pool_first_uav_offset_bytes +
                      sizeof(struct terakan_vk_descriptor_set_uav) *
                         (src_binding->first_set_uav + descriptor_copy->srcArrayElement),
                   sizeof(struct terakan_vk_descriptor_set_uav) * descriptor_copy->descriptorCount);
         }
      }

      if (terakan_vk_descriptor_type_has_sampler(descriptor_type)) {
         assert(terakan_vk_descriptor_type_has_sampler(dst_binding->descriptor_type));
         memcpy(
            dst_set->descriptors + dst_set->layout->pool_first_sampler_offset_bytes +
               sizeof(struct terakan_vk_descriptor_set_sampler) *
                  (dst_binding->first_set_sampler + descriptor_copy->dstArrayElement),
            src_set->descriptors + src_set->layout->pool_first_sampler_offset_bytes +
               sizeof(struct terakan_vk_descriptor_set_sampler) *
                  (src_binding->first_set_sampler + descriptor_copy->srcArrayElement),
            sizeof(struct terakan_vk_descriptor_set_sampler) * descriptor_copy->descriptorCount);
      }
   }
}

static void
terakan_vk_descriptor_set_free_descriptors_and_finish(
   struct terakan_vk_descriptor_pool * const pool, uint32_t const set_index)
{
   struct terakan_vk_descriptor_set * const set = &pool->sets[set_index];

   uint32_t const descriptors_size = set->layout->pool_size_bytes;
   if (descriptors_size != 0) {
      assert(pool->descriptor_memory_size - pool->descriptor_memory_unallocated >=
             descriptors_size);
      pool->descriptor_memory_unallocated += descriptors_size;
      util_vma_heap_free(&pool->descriptor_memory_heap, (uint64_t)set->descriptors,
                         descriptors_size);
   }

   vk_descriptor_set_layout_unref(set->base.device, &set->layout->vk);

   vk_object_base_finish(&set->base);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_FreeDescriptorSets(UNUSED VkDevice const device, VkDescriptorPool const descriptorPool,
                              uint32_t const descriptorSetCount,
                              VkDescriptorSet const * const pDescriptorSets)
{
   struct terakan_vk_descriptor_pool * const pool =
      terakan_vk_descriptor_pool_from_handle(descriptorPool);

   pool->sets_allocated -= descriptorSetCount;

   for (uint32_t array_set_index = 0; array_set_index < descriptorSetCount; ++array_set_index) {
      struct terakan_vk_descriptor_set * const set =
         terakan_vk_descriptor_set_from_handle(pDescriptorSets[array_set_index]);

      uint32_t const set_index = set - pool->sets;
      assert(set_index < pool->max_sets);

      terakan_vk_descriptor_set_free_descriptors_and_finish(pool, set_index);

      if (set->pool_allocated_prev != UINT32_MAX) {
         struct terakan_vk_descriptor_set * const set_prev = &pool->sets[set->pool_allocated_prev];
         assert(set_prev->pool_allocated_or_freed_next == set_index);
         set_prev->pool_allocated_or_freed_next = set->pool_allocated_or_freed_next;
      } else {
         assert(pool->allocated_sets_head == set_index);
         pool->allocated_sets_head = set->pool_allocated_or_freed_next;
      }
      if (set->pool_allocated_or_freed_next != UINT32_MAX) {
         struct terakan_vk_descriptor_set * const set_next =
            &pool->sets[set->pool_allocated_or_freed_next];
         assert(set_next->pool_allocated_prev == set_index);
         set_next->pool_allocated_prev = set->pool_allocated_prev;
      }

      set->pool_allocated_or_freed_next = pool->freed_sets_head;
      pool->freed_sets_head = set_index;
   }

   pool->sets_freed += descriptorSetCount;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_AllocateDescriptorSets(VkDevice const deviceHandle,
                                  VkDescriptorSetAllocateInfo const * const pAllocateInfo,
                                  VkDescriptorSet * const pDescriptorSets)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_vk_descriptor_pool * const pool =
      terakan_vk_descriptor_pool_from_handle(pAllocateInfo->descriptorPool);
   uint32_t const set_count = pAllocateInfo->descriptorSetCount;

   if (pool->max_sets - pool->sets_allocated < set_count) {
      for (uint32_t null_set_index = 0; null_set_index < set_count; ++null_set_index) {
         pDescriptorSets[null_set_index] = VK_NULL_HANDLE;
      }
      return vk_error(device, VK_ERROR_OUT_OF_POOL_MEMORY);
   }

   for (uint32_t array_set_index = 0; array_set_index < set_count; ++array_set_index) {
      struct terakan_vk_descriptor_set_layout * const set_layout =
         terakan_vk_descriptor_set_layout_from_handle(pAllocateInfo->pSetLayouts[array_set_index]);
      size_t const set_size = set_layout->pool_size_bytes;

      char * set_descriptors = NULL;
      if (set_size != 0) {
         VkResult set_descriptors_allocate_error = VK_ERROR_OUT_OF_POOL_MEMORY;
         /* Distinguish between trying to allocate too much memory and fragmentation. */
         if (pool->descriptor_memory_unallocated >= set_size) {
            set_descriptors_allocate_error = VK_ERROR_FRAGMENTED_POOL;
            set_descriptors =
               (char *)util_vma_heap_alloc(&pool->descriptor_memory_heap, set_size,
                                           TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT);
         }
         if (set_descriptors == NULL) {
            terakan_vk_FreeDescriptorSets(deviceHandle, pAllocateInfo->descriptorPool,
                                          array_set_index, pDescriptorSets);
            for (uint32_t null_set_index = 0; null_set_index < set_count; ++null_set_index) {
               pDescriptorSets[null_set_index] = VK_NULL_HANDLE;
            }
            return vk_error(device, set_descriptors_allocate_error);
         }
         assert(pool->descriptor_memory_unallocated >= set_size);
         pool->descriptor_memory_unallocated -= set_size;
      }

      uint32_t set_index = pool->freed_sets_head;
      if (set_index != UINT32_MAX) {
         assert(pool->sets_freed != 0);
         --pool->sets_freed;
         pool->freed_sets_head = pool->sets[set_index].pool_allocated_or_freed_next;
      } else {
         set_index = pool->sets_allocated + pool->sets_freed;
         assert(set_index < pool->max_sets);
      }
      ++pool->sets_allocated;
      struct terakan_vk_descriptor_set * const set = &pool->sets[set_index];

      set->pool_allocated_prev = UINT32_MAX;
      set->pool_allocated_or_freed_next = pool->allocated_sets_head;
      if (pool->allocated_sets_head != UINT32_MAX) {
         struct terakan_vk_descriptor_set * const allocated_head_set =
            &pool->sets[pool->allocated_sets_head];
         assert(allocated_head_set->pool_allocated_prev == UINT32_MAX);
         allocated_head_set->pool_allocated_prev = set_index;
      }
      pool->allocated_sets_head = set_index;

      vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

      vk_descriptor_set_layout_ref(&set_layout->vk);
      set->layout = set_layout;

      set->descriptors = set_descriptors;

      if (set_size != 0) {
         /* Section 14.2.3. "Allocation of Descriptor Sets" of the Vulkan 1.3.275 specification
          * says:
          *
          *     "Entries that are not used by a pipeline can have undefined descriptors."
          *
          * Make sure the internal logic of the driver can assume that undefined descriptors are
          * still well-formed descriptors (except for the BO potentially having been destroyed) of
          * the hardware types corresponding to them, even if never written, by initializing the
          * descriptor set memory to null descriptors (for samplers).
          */
         memset(set->descriptors, 0, set_size);

         /* Write immutable samplers. */
         struct terakan_vk_descriptor_set_sampler * const set_samplers =
            (struct terakan_vk_descriptor_set_sampler *)(set->descriptors +
                                                         set_layout
                                                            ->pool_first_sampler_offset_bytes);
         for (uint8_t immutable_sampler_index = 0;
              immutable_sampler_index < set_layout->immutable_sampler_count;
              ++immutable_sampler_index) {
            terakan_vk_descriptor_set_sampler_init(
               &set_samplers[set_layout->immutable_sampler_indices_in_set[immutable_sampler_index]],
               set_layout->immutable_samplers[immutable_sampler_index]);
         }
      }

      pDescriptorSets[array_set_index] = terakan_vk_descriptor_set_to_handle(set);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_ResetDescriptorPool(UNUSED VkDevice const device, VkDescriptorPool const descriptorPool,
                               UNUSED VkDescriptorPoolResetFlags const flags)
{
   struct terakan_vk_descriptor_pool * const pool =
      terakan_vk_descriptor_pool_from_handle(descriptorPool);

   for (uint32_t set_index = pool->allocated_sets_head; set_index != UINT32_MAX;
        set_index = pool->sets[set_index].pool_allocated_or_freed_next) {
      terakan_vk_descriptor_set_free_descriptors_and_finish(pool, set_index);
   }

   pool->sets_allocated = 0;
   pool->allocated_sets_head = UINT32_MAX;

   pool->sets_freed = 0;
   pool->freed_sets_head = UINT32_MAX;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_vk_DestroyDescriptorPool(VkDevice const deviceHandle, VkDescriptorPool const descriptorPool,
                                 VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_vk_descriptor_pool * const pool =
      terakan_vk_descriptor_pool_from_handle(descriptorPool);

   if (pool == NULL) {
      return;
   }

   for (uint32_t set_index = pool->allocated_sets_head; set_index != UINT32_MAX;
        set_index = pool->sets[set_index].pool_allocated_or_freed_next) {
      terakan_vk_descriptor_set_free_descriptors_and_finish(pool, set_index);
   }

   if (pool->descriptor_memory_size != 0) {
      util_vma_heap_finish(&pool->descriptor_memory_heap);
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_object_base_finish(&pool->base);

   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreateDescriptorPool(VkDevice const deviceHandle,
                                VkDescriptorPoolCreateInfo const * const pCreateInfo,
                                VkAllocationCallbacks const * const pAllocator,
                                VkDescriptorPool * const pDescriptorPool)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   size_t srv_count = 0, sampler_count = 0, uav_count = 0;
   for (uint32_t pool_size_index = 0; pool_size_index < pCreateInfo->poolSizeCount;
        ++pool_size_index) {
      VkDescriptorPoolSize const pool_size = pCreateInfo->pPoolSizes[pool_size_index];
      if (terakan_vk_descriptor_type_has_srv(pool_size.type)) {
         srv_count += pool_size.descriptorCount;
         if (terakan_vk_descriptor_type_has_uav(pool_size.type)) {
            uav_count += pool_size.descriptorCount;
         }
      }
      if (terakan_vk_descriptor_type_has_sampler(pool_size.type)) {
         sampler_count += pool_size.descriptorCount;
      }
   }
   size_t const descriptor_memory_size =
      sizeof(struct terakan_vk_descriptor_set_srv) * srv_count +
      sizeof(struct terakan_vk_descriptor_set_sampler) * sampler_count +
      sizeof(struct terakan_vk_descriptor_set_uav) * uav_count;

   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_vk_descriptor_pool, pool, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_vk_descriptor_set, sets, pCreateInfo->maxSets);
   void * descriptor_memory;
   vk_multialloc_add_size_align(&multialloc, &descriptor_memory, descriptor_memory_size,
                                TERAKAN_VK_DESCRIPTOR_SET_DESCRIPTOR_ALIGNMENT);
   if (vk_multialloc_alloc2(&multialloc, &device->vk.alloc, pAllocator,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL);

   pool->descriptor_memory_size = descriptor_memory_size;
   pool->descriptor_memory = descriptor_memory;
   if (descriptor_memory_size != 0) {
      static_assert(
         sizeof(descriptor_memory) <= sizeof(uint64_t),
         "Using VMA directly for CPU pointers, expecting offsets in it to be large enough to store "
         "one.");
      /* Use the descriptor memory pointer directly as the start because VMA expects the start
       * address to be nonzero since it uses 0 to report allocation errors.
       */
      util_vma_heap_init(&pool->descriptor_memory_heap, (uint64_t)descriptor_memory,
                         descriptor_memory_size);
   }
   pool->descriptor_memory_unallocated = descriptor_memory_size;

   pool->sets = sets;
   pool->max_sets = pCreateInfo->maxSets;

   pool->sets_allocated = 0;
   pool->allocated_sets_head = UINT32_MAX;

   pool->sets_freed = 0;
   pool->freed_sets_head = UINT32_MAX;

   *pDescriptorPool = terakan_vk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}
