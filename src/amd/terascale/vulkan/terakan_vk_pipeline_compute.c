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

#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_shader.h"
#include "terakan_vk_entrypoints.h"
#include "terakan_vk_pipeline_layout.h"

#include "util/macros.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_pipeline.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include <stddef.h>
#include <stdint.h>

struct terakan_vk_pipeline_compute {
   struct vk_pipeline vk;

   struct terakan_bo * shader_bo;
   struct terakan_shader_impl shader;
};

static void
terakan_vk_pipeline_compute_cmd_bind(struct vk_command_buffer * const command_buffer_base,
                                     struct vk_pipeline * const pipeline_base)
{
   terakan_app_config_compute_set_sq_pgm_ls(
      &container_of(command_buffer_base, struct terakan_command_buffer, vk)
          ->command_writer.gfx->app_config_compute,
      &container_of(pipeline_base, struct terakan_vk_pipeline_compute, vk)->shader);
}

static VkResult
terakan_vk_pipeline_compute_get_executable_properties(
   UNUSED struct vk_device * const device_base, struct vk_pipeline * const pipeline_base,
   uint32_t * const executable_count, VkPipelineExecutablePropertiesKHR * const properties)
{
   /* TODO(Triang3l): Pipeline executables. */
   *executable_count = 0;
   return VK_SUCCESS;
}

static void
terakan_vk_pipeline_compute_destroy(struct vk_device * const device_base,
                                    struct vk_pipeline * const pipeline_base,
                                    VkAllocationCallbacks const * const allocator)
{
   struct terakan_vk_pipeline_compute * const pipeline =
      container_of(pipeline_base, struct terakan_vk_pipeline_compute, vk);

   terakan_shader_impl_finish(&pipeline->shader);

   if (pipeline->shader_bo != NULL) {
      terakan_bo_free(pipeline->shader_bo, allocator);
   }

   vk_pipeline_free(device_base, allocator, &pipeline->vk);
}

struct vk_pipeline_ops const terakan_vk_pipeline_compute_ops = {
   .destroy = terakan_vk_pipeline_compute_destroy,
   .get_executable_properties = terakan_vk_pipeline_compute_get_executable_properties,
   .cmd_bind = terakan_vk_pipeline_compute_cmd_bind,
};

static VkResult
terakan_vk_pipeline_compute_create(struct terakan_device * const device,
                                   VkComputePipelineCreateInfo const * const create_info,
                                   VkAllocationCallbacks const * const allocator,
                                   struct terakan_vk_pipeline_compute ** const pipeline_out)
{
   VkPipelineCreateFlags2KHR const create_flags = vk_compute_pipeline_create_flags(create_info);
   struct terakan_vk_pipeline_compute * const pipeline = vk_pipeline_zalloc(
      &device->vk, &terakan_vk_pipeline_compute_ops, VK_PIPELINE_BIND_POINT_COMPUTE, create_flags,
      allocator, sizeof(struct terakan_vk_pipeline_compute));
   if (pipeline == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   VkResult result;

   /* Compile the shader. */

   size_t spirv_size_bytes;
   uint32_t const * spirv;
   /* The `VkPipelineShaderStageCreateInfo` reference in the Vulkan 1.4.349 specification says:
    *
    *     "If `module` is not `VK_NULL_HANDLE`, the shader code used by the pipeline is defined by
    *     `module`. If `module` is `VK_NULL_HANDLE`, the shader code is defined by the chained
    *     `VkShaderModuleCreateInfo` if present."
    */
   if (create_info->stage.module != VK_NULL_HANDLE) {
      struct vk_shader_module const * const shader_module =
         vk_shader_module_from_handle(create_info->stage.module);
      spirv_size_bytes = shader_module->size;
      spirv = (uint32_t const *)shader_module->data;
   } else {
      VkShaderModuleCreateInfo const * const shader_module_create_info =
         vk_find_struct_const(create_info->stage.pNext, SHADER_MODULE_CREATE_INFO);
      if (likely(shader_module_create_info == NULL)) {
         spirv_size_bytes = shader_module_create_info->codeSize;
         spirv = shader_module_create_info->pCode;
      } else {
         result = vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                            "No shader module provided for the compute pipeline");
         goto fail_pipeline;
      }
   }

   struct terakan_vk_pipeline_layout const * const pipeline_layout =
      terakan_vk_pipeline_layout_from_handle(create_info->layout);

   struct terakan_nir_lower_and_optimize_post_link_options post_link_lowering_options = {
      .next_stage = MESA_SHADER_NONE,

      .get_binding_functions = &terakan_vk_pipeline_layout_get_binding_functions,
      .get_binding_functions_data = pipeline_layout,

      /* TODO(Triang3l): `VK_EXT_pipeline_robustness`. */
      .robust_buffer_access_2 = device->vk.enabled_features.robustBufferAccess2,

      .physical_device = terakan_device_physical_device(device),
   };

   nir_shader * const nir = terakan_nir_from_spirv_pre_link(
      device, spirv_size_bytes, spirv, MESA_SHADER_COMPUTE, create_info->stage.pName,
      create_info->stage.pSpecializationInfo);
   if (unlikely(nir == NULL)) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_pipeline;
   }

   struct terakan_nir_lower_and_optimize_post_link_result const lower_and_optimize_post_link_result =
      terakan_nir_lower_and_optimize_post_link(nir, &post_link_lowering_options);

   union r600_shader_key shader_key = {};

   terakan_shader_impl_apply_lower_and_optimize_post_link_result(
      &pipeline->shader, &shader_key, &lower_and_optimize_post_link_result, MESA_SHADER_COMPUTE);

   pipeline->shader.push_constants_usage.app_extent_bytes =
      pipeline_layout->shader_app_push_constants_extents_bytes[MESA_SHADER_COMPUTE];

   result = terakan_shader_impl_compile(&pipeline->shader, device, &shader_key, nir, allocator);
   ralloc_free(nir);
   if (result != VK_SUCCESS) {
      goto fail_shader;
   }

   /* Place the hardware shader in the BO. */
   /* TODO(Triang3l): Use a global allocator and GPU uploading instead of creating one host-visible
    * device-local BO per pipeline.
    */
   /* TODO(Triang3l): Static constant buffer. */
   /* 32 bits are enough for the shader size in bytes, as qword addresses in shader control flow
    * instructions are 24-bit.
    */
   uint32_t const shader_bytecode_size_bytes =
      (uint32_t)sizeof(uint32_t) * pipeline->shader.shader.bc.ndw;
   result = device->platform_fn->bo->allocate_device_memory(
      device, shader_bytecode_size_bytes, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &pipeline->shader_bo);
   if (result != VK_SUCCESS) {
      result = vk_error(device, result);
      goto fail_shader;
   }
   {
      uint32_t * const shader_bo_mapping = terakan_bo_map(pipeline->shader_bo);
      if (shader_bo_mapping == NULL) {
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_shader_bo;
      }
      util_memcpy_cpu_to_le32(shader_bo_mapping, pipeline->shader.shader.bc.bytecode,
                              shader_bytecode_size_bytes);
      pipeline->shader.static_state.program_bo = pipeline->shader_bo;
      pipeline->shader.static_state.program_va_shr8 = (uint32_t)(pipeline->shader_bo->va >> 8);
      terakan_bo_unmap(pipeline->shader_bo);
   }
   /* Destroy the data needed only for creation. */
   r600_bytecode_clear(&pipeline->shader.shader.bc);

   *pipeline_out = pipeline;
   return VK_SUCCESS;

fail_shader_bo:
   terakan_bo_free(pipeline->shader_bo, allocator);
fail_shader:
   r600_bytecode_clear(&pipeline->shader.shader.bc);
   free(pipeline->shader.shader.arrays);
fail_pipeline:
   terakan_vk_pipeline_compute_destroy(&device->vk, &pipeline->vk, allocator);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreateComputePipelines(VkDevice const deviceHandle, VkPipelineCache const pipelineCache,
                                  uint32_t const createInfoCount,
                                  VkComputePipelineCreateInfo const * const pCreateInfos,
                                  VkAllocationCallbacks const * const pAllocator,
                                  VkPipeline * const pPipelines)
{
   VkResult result = VK_SUCCESS;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   uint32_t pipeline_index;

   for (pipeline_index = 0; pipeline_index < createInfoCount; ++pipeline_index) {
      struct terakan_vk_pipeline_compute * pipeline = NULL;
      VkComputePipelineCreateInfo const * const create_info = &pCreateInfos[pipeline_index];
      VkResult const pipeline_result =
         terakan_vk_pipeline_compute_create(device, create_info, pAllocator, &pipeline);
      if (pipeline_result != VK_SUCCESS) {
         result = pipeline_result;
         if (vk_compute_pipeline_create_flags(create_info) &
             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {
            break;
         }
         pPipelines[pipeline_index] = VK_NULL_HANDLE;
         continue;
      }
      pPipelines[pipeline_index] = vk_pipeline_to_handle(&pipeline->vk);
   }

   for (; pipeline_index < createInfoCount; ++pipeline_index) {
      pPipelines[pipeline_index] = VK_NULL_HANDLE;
   }

   return result;
}
