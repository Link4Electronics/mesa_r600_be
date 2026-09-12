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

#include "terakan_descriptor.h"

#include "terakan_device.h"
#include "terakan_format.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void
terakan_color_descriptor_calculate_buffer_base_dim_offset(
   struct terakan_color_descriptor * const descriptor, uint64_t const va,
   VkDeviceSize const elements, unsigned const bytes_per_element,
   unsigned const pipe_interleave_bytes_log2, uint32_t * const alignment_offset_bytes_out)
{
   uint64_t const va_aligned = va >> pipe_interleave_bytes_log2 << pipe_interleave_bytes_log2;
   descriptor->base = (uint32_t)(va_aligned >> 8);

   uint32_t const alignment_offset_bytes = (uint32_t)(va - va_aligned);
   *alignment_offset_bytes_out = alignment_offset_bytes;

   assert(elements != 0);
   descriptor->dim = (uint32_t)(alignment_offset_bytes / bytes_per_element + elements - 1u);
}

struct terakan_srv_descriptor
terakan_color_descriptor_info_to_uav_immediate_srv(struct terakan_device const * const device,
                                                   uint32_t const color_info)
{
   enum terascale_format_index const format =
      (enum terascale_format_index)G_028C70_FORMAT(color_info);

   unsigned const bytes_per_element = terascale_format_bytes_per_block[format];

   uint32_t const va_shr8 = device->uav_immediate_va_shr8[util_logbase2(bytes_per_element)];

   uint32_t const uav_immediate_size_elements =
      terakan_device_physical_device(device)->chip_info.uav_immediate_size_elements;

   enum terascale_format_number_type const number_type =
      (enum terascale_format_number_type)G_028C70_NUMBER_TYPE(color_info);

   uint8_t const * const swizzle =
      terascale_format_cb_color_read_swizzle[terascale_format_channel_count[format]]
                                            [G_028C70_COMP_SWAP(color_info)];

   return (struct terakan_srv_descriptor){
      .srv = {
         [0] = (uint32_t)(va_shr8 << 8),
         [1] = (uav_immediate_size_elements * bytes_per_element) - 1,
         [2] = S_030008_BASE_ADDRESS_HI(va_shr8 >> (32 - 8)) | S_030008_STRIDE(bytes_per_element) |
               S_030008_DATA_FORMAT(format) |
               S_030008_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(number_type)) |
               S_030008_FORMAT_COMP_ALL(number_type == TERASCALE_FORMAT_NUMBER_TYPE_SNORM ||
                                        number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT) |
               S_030008_ENDIAN_SWAP(G_028C70_ENDIAN(color_info)),
         /* In Vulkan, storage descriptors can be created only for one aspect, and TeraScale's
          * combined depth / stencil SQ / CB formats correspond only to depth / stencil formats that
          * exist in Vulkan - more specifically, on R8xx / R9xx, depth and stencil are stored
          * separately, with stencil always being `TERASCALE_FORMAT_INDEX_8`, and `8_24` being used
          * only for the depth aspect (of `X8_D24_UNORM_PACK32` and `D24_UNORM_S8_UINT`). Therefore,
          * for reads from the hardware combined depth / stencil formats, which are two-channel from
          * the perspective of SQ and CB, and thus the CB swap is treated as two-channel, the
          * stencil channel needs to be replaced with 0.
          */
         [3] = S_03000C_UNCACHED(true) | S_03000C_DST_SEL_X(swizzle[0]) |
               S_03000C_DST_SEL_Y(TERASCALE_FORMATS_COMBINED_DEPTH_STENCIL & BITFIELD64_BIT(format)
                                     ? TERASCALE_SWIZZLE_0
                                     : swizzle[1]) |
               S_03000C_DST_SEL_Z(swizzle[2]) | S_03000C_DST_SEL_W(swizzle[3]),
         [4] = uav_immediate_size_elements,
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER,
      }};
}
