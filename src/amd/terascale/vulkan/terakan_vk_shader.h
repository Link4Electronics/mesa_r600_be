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

#ifndef TERAKAN_VK_SHADER_H
#define TERAKAN_VK_SHADER_H

#include "compiler/shader_enums.h"
#include "util/bitscan.h"

#include <assert.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline mesa_shader_stage
terakan_vk_shader_stage_scan(VkShaderStageFlags * const stages_remaining)
{
   static_assert(
      (VkShaderStageFlags)1 << MESA_SHADER_VERTEX == VK_SHADER_STAGE_VERTEX_BIT &&
         (VkShaderStageFlags)1 << MESA_SHADER_TESS_CTRL ==
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT &&
         (VkShaderStageFlags)1 << MESA_SHADER_TESS_EVAL ==
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT &&
         (VkShaderStageFlags)1 << MESA_SHADER_GEOMETRY == VK_SHADER_STAGE_GEOMETRY_BIT &&
         (VkShaderStageFlags)1 << MESA_SHADER_FRAGMENT == VK_SHADER_STAGE_FRAGMENT_BIT &&
         (VkShaderStageFlags)1 << MESA_SHADER_COMPUTE == VK_SHADER_STAGE_COMPUTE_BIT,
      "`mesa_shader_stage` must match `VkShaderStageFlagBits` indices for the stages supported by "
      "the driver for trivial translation.");
   static_assert(
      MESA_SHADER_NONE == -1,
      "`MESA_SHADER_NONE` must be equal to -1, so `ffs` return value minus 1 can be used directly "
      "for empty flags as well.");
   mesa_shader_stage const stage = (mesa_shader_stage)(ffs((int)*stages_remaining) - 1);
   *stages_remaining &= ~((VkShaderStageFlags)1 << (unsigned)stage);
   return stage;
}

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_SHADER_H */
