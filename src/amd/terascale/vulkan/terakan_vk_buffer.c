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

#include "terakan_vk_buffer.h"

#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_format.h"
#include "terakan_physical_device.h"
#include "terakan_vk_device_memory.h"
#include "terakan_vk_entrypoints.h"

#include "util/macros.h"
#include "util/u_endian.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

VKAPI_ATTR void VKAPI_CALL
terakan_vk_GetPhysicalDeviceExternalBufferProperties(
   UNUSED VkPhysicalDevice const physicalDevice,
   VkPhysicalDeviceExternalBufferInfo const * const pExternalBufferInfo,
   VkExternalBufferProperties * const pExternalBufferProperties)
{
   pExternalBufferProperties->externalMemoryProperties.externalMemoryFeatures = 0;
   pExternalBufferProperties->externalMemoryProperties.exportFromImportedHandleTypes = 0;
   pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes = 0;

   struct terakan_physical_device const * const device =
      terakan_physical_device_from_handle(physicalDevice);

   VkExternalMemoryHandleTypeFlags const supported_handle_types =
      terakan_physical_device_supported_external_memory_types(device);

   if (supported_handle_types & pExternalBufferInfo->handleType) {
      switch (pExternalBufferInfo->handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT: {
         pExternalBufferProperties->externalMemoryProperties.externalMemoryFeatures =
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
         VkExternalMemoryHandleTypeFlags const supported_fd_types =
            supported_handle_types & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
         pExternalBufferProperties->externalMemoryProperties.exportFromImportedHandleTypes =
            supported_fd_types;
         pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes =
            supported_fd_types;
      } break;

      default:
         break;
      }
   }

   /* From the VkExternalMemoryProperties specification:
    *
    *    compatibleHandleTypes must include at least handleType.
    */
   pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes |=
      pExternalBufferInfo->handleType;
}

VKAPI_ATTR void VKAPI_CALL
terakan_vk_GetDeviceBufferMemoryRequirements(VkDevice const deviceHandle,
                                             VkDeviceBufferMemoryRequirements const * const pInfo,
                                             VkMemoryRequirements2 * const pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements.size = pInfo->pCreateInfo->size;

   VkDeviceSize alignment;
   VkBufferUsageFlags const usage = pInfo->pCreateInfo->usage;
   /* From the largest to the smallest alignment. */
   if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
      alignment = TERAKAN_KCACHE_HW_LINE_BYTES;
   } else if (usage &
              (VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
      /* Largest random access target element (R32G32B32A32) alignment, including for image to
       * buffer transfers. Also query result copying may require an alignment of up to 8 bytes.
       */
      alignment = sizeof(uint32_t) * 4;
   } else {
      /* Required in many places: largest in vertex fetch instructions, largest possible vertex
       * index size, a storage buffer vertex fetch and UAV element, indirect buffer alignment.
       * Additionally, because storage buffers use 32-bit UAVs, and with `VK_EXT_robustness2` their
       * ranges are rounded up (not down) to `robustStorageBufferAccessSizeAlignment`, which is
       * `sizeof(uint32_t)` because of that, aligning all buffers and images to `sizeof(uint32_t)`
       * prevents writes to the end of storage buffers with an unaligned size from affecting the
       * resource placed next to them.
       */
      alignment = sizeof(uint32_t);
   }
   pMemoryRequirements->memoryRequirements.alignment = alignment;

   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(terakan_device_from_handle(deviceHandle));
   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;

   vk_foreach_struct (ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements * const dedicated_requirements =
            (VkMemoryDedicatedRequirements *)ext;
         dedicated_requirements->requiresDedicatedAllocation = VK_FALSE;
         dedicated_requirements->prefersDedicatedAllocation = VK_FALSE;
      } break;

      default:
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_BindBufferMemory2(VkDevice const device, uint32_t const bindInfoCount,
                             VkBindBufferMemoryInfo const * const pBindInfos)
{
   /* #MemoryIntegrity. */
   for (uint32_t bind_info_index = 0; bind_info_index < bindInfoCount; ++bind_info_index) {
      VkBindBufferMemoryInfo const * const bind_info = &pBindInfos[bind_info_index];
      struct terakan_vk_buffer const * const buffer =
         terakan_vk_buffer_from_handle(bind_info->buffer);
      struct terakan_vk_device_memory const * const memory =
         terakan_vk_device_memory_from_handle(bind_info->memory);
      /* Currently `vkGetDeviceBufferMemoryRequirements` never pads the size, so checking the size
       * of the buffer itself, not the size in the memory requirements, is sufficient.
       *
       * Note that alignment isn't validated, because in some cases it's the sum of the buffer
       * device memory binding offset and the offset provided to the specific command that needs to
       * be aligned to a certain boundary.
       * VUID-vkCmdBindIndexBuffer2-offset-08783:
       *     "The sum of `offset` and the base address of the range of `VkDeviceMemory` object that
       *     is backing `buffer`, must be a multiple of the size of the type indicated by
       *     `indexType`"
       * Although that's not always the case in Vulkan valid usage wordings.
       * VUID-VkWriteDescriptorSet-descriptorType-00327:
       *     "If `descriptorType` is `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` or
       *     `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`, the `offset` member of each element of
       *     `pBufferInfo` must be a multiple of
       *     `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment`"
       */
      if (unlikely(bind_info->memoryOffset > memory->vk.size ||
                   memory->vk.size - bind_info->memoryOffset < buffer->vk.size)) {
         return vk_errorf(
            terakan_device_from_handle(device), VK_ERROR_VALIDATION_FAILED_EXT,
            "Device memory allocation with a size of %" PRIu64 " bytes is too small to bind a "
            "buffer with a size of %" PRIu64 " bytes at an offset of %" PRIu64 " bytes to it",
            memory->vk.size, buffer->vk.size, bind_info->memoryOffset);
      }
   }

   for (uint32_t bind_info_index = 0; bind_info_index < bindInfoCount; ++bind_info_index) {
      VkBindBufferMemoryInfo const * const bind_info = &pBindInfos[bind_info_index];
      struct terakan_vk_buffer * const buffer = terakan_vk_buffer_from_handle(bind_info->buffer);
      buffer->bo = terakan_vk_device_memory_from_handle(bind_info->memory)->bo;
      buffer->va = buffer->bo->va + bind_info->memoryOffset;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_vk_DestroyBuffer(VkDevice const deviceHandle, VkBuffer const bufferHandle,
                         VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_vk_buffer * const buffer = terakan_vk_buffer_from_handle(bufferHandle);

   if (buffer == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_buffer_finish(&buffer->vk);

   vk_free2(&device->vk.alloc, pAllocator, buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreateBuffer(VkDevice const deviceHandle, VkBufferCreateInfo const * const pCreateInfo,
                        VkAllocationCallbacks const * const pAllocator, VkBuffer * const pBuffer)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_vk_buffer * const buffer =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_vk_buffer),
                alignof(struct terakan_vk_buffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_buffer_init(&device->vk, &buffer->vk, pCreateInfo);

   buffer->bo = NULL;
   buffer->va = 0;

   *pBuffer = terakan_vk_buffer_to_handle(buffer);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_vk_DestroyBufferView(VkDevice const deviceHandle, VkBufferView const bufferView,
                             VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_vk_buffer_view * const buffer_view =
      terakan_vk_buffer_view_from_handle(bufferView);

   if (buffer_view == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_buffer_view_finish(&buffer_view->vk);

   vk_free2(&device->vk.alloc, pAllocator, buffer_view);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreateBufferView(VkDevice const deviceHandle,
                            VkBufferViewCreateInfo const * const pCreateInfo,
                            VkAllocationCallbacks const * const pAllocator,
                            VkBufferView * const pView)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_vk_buffer_view * const buffer_view =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_vk_buffer_view),
                alignof(struct terakan_vk_buffer_view), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer_view == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_buffer_view_init(&device->vk, &buffer_view->vk, pCreateInfo);

   struct terakan_vk_buffer const * const buffer =
      terakan_vk_buffer_from_handle(pCreateInfo->buffer);

   buffer_view->bo = buffer->bo;

   buffer_view->srv = (struct terakan_srv_descriptor){};
   buffer_view->color = (struct terakan_color_descriptor){};

   struct terascale_format_info const format_info =
      terascale_format_info_r8xx[vk_format_to_pipe_format(pCreateInfo->format)];
   if (format_info.supports_sq_vertex_fetch) {
      unsigned const bytes_per_element = terascale_format_bytes_per_block[format_info.format];
      VkDeviceSize elements = terakan_vk_buffer_clamp_range(
         buffer, pCreateInfo->offset, buffer_view->vk.elements, bytes_per_element);
      /* Limit the size to:
       * - `2^32 bytes, as buffer SRV descriptors contain the size minus 1 in bytes.
       * - `2^31 - 1` elements, as `texelFetch` and `textureSize` use signed 32-bit integers.
       * - `2^32 - pipe interleave` for 1 byte per element, as the 32-bit buffer UAV size minus 1 in
       *   elements is padded to include the alignment offset. This, however, is already covered by
       *   clamping to `2^31 - 1` elements.
       */
      elements = MIN3(elements, ((VkDeviceSize)1 << 32) / bytes_per_element, (uint32_t)INT32_MAX);
      if (likely(elements != 0)) {
         uint64_t const va = buffer->va + pCreateInfo->offset;

         enum terascale_endian_swap const endian_swap =
            UTIL_ARCH_BIG_ENDIAN ? terascale_format_big_endian_swap[format_info.format]
                                 : TERASCALE_ENDIAN_SWAP_NONE;

         buffer_view->srv.srv[0] = (uint32_t)va;
         assert(
            elements <= UINT32_MAX &&
            "The buffer view element count clamping total upper bound may never exceed 32 bits");
         buffer_view->srv.srv[1] = bytes_per_element * (uint32_t)elements - 1u;
         buffer_view->srv.srv[2] = S_030008_BASE_ADDRESS_HI(va >> 32) |
                                   S_030008_STRIDE(bytes_per_element) |
                                   S_030008_DATA_FORMAT(format_info.format) |
                                   S_030008_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(
                                      (enum terascale_format_number_type)format_info.number_type)) |
                                   S_030008_FORMAT_COMP_ALL(format_info.channels_signed != 0b0) |
                                   S_030008_ENDIAN_SWAP(endian_swap);
         buffer_view->srv.srv[3] =
            S_03000C_DST_SEL_X(format_info.swizzle_r) | S_03000C_DST_SEL_Y(format_info.swizzle_g) |
            S_03000C_DST_SEL_Z(format_info.swizzle_b) | S_03000C_DST_SEL_W(format_info.swizzle_a);
         buffer_view->srv.srv[4] = (uint32_t)elements;
         buffer_view->srv.srv[7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER);
         buffer_view->srv.srv[TERAKAN_SRV_BUFFER_PRIORITY_WORD] =
            TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER;

         /* The vertex buffer format is also used for the UAV IMMED buffer, so creating the UAV
          * inside the vertex format supported conditional too.
          */

         if (format_info.supports_cb_color && va % bytes_per_element == 0) {
            terakan_color_descriptor_calculate_buffer_base_view_dim(
               &buffer_view->color, va, elements, bytes_per_element,
               terakan_device_physical_device(device)->tiling_info.pipe_interleave_bytes_log2,
               true);
            buffer_view->color.info = S_028C70_ENDIAN(endian_swap) |
                                      S_028C70_FORMAT(format_info.format) |
                                      S_028C70_NUMBER_TYPE(format_info.number_type) |
                                      S_028C70_COMP_SWAP(format_info.cb_color_swap) |
                                      TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS;
            buffer_view->color.attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB;
         }
      }
   }

   *pView = terakan_vk_buffer_view_to_handle(buffer_view);
   return VK_SUCCESS;
}
