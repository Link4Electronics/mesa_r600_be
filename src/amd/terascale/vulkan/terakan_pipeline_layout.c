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

#include "terakan_pipeline_layout.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_descriptor_set.h"
#include "terakan_descriptor_set_layout.h"
#include "terakan_device.h"
#include "terakan_vk_entrypoints.h"

#include "amd/terascale/common/terascale_format.h"
#include "compiler/shader_enums.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Note that for logging, the device object rather than the pipeline layout object must be used,
 * because a pipeline layout object may be created internally by the driver, and thus not have its
 * handle known to the application, in cases such as `vkCreateShadersEXT`.
 */

VKAPI_ATTR void VKAPI_CALL
terakan_vk_CmdBindDescriptorSets(VkCommandBuffer const commandBuffer,
                                 VkPipelineBindPoint const pipelineBindPoint,
                                 VkPipelineLayout const layoutHandle, uint32_t const firstSet,
                                 uint32_t const descriptorSetCount,
                                 VkDescriptorSet const * const pDescriptorSets,
                                 UNUSED uint32_t const dynamicOffsetCount,
                                 uint32_t const * const pDynamicOffsets)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_pipeline_layout const * const layout =
      terakan_pipeline_layout_from_handle(layoutHandle);

   bool const is_compute = pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE;
   mesa_shader_stage const shader_stage_first =
      is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX;
   mesa_shader_stage const shader_stage_last =
      is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;

   mesa_shader_stage const uav_shader_stage =
      is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;

   uint32_t const * set_dynamic_offsets = pDynamicOffsets;

   for (uint32_t set_relative_index = 0; set_relative_index < descriptorSetCount;
        ++set_relative_index) {
      struct terakan_pipeline_layout_set const * const layout_set =
         &layout->sets[firstSet + set_relative_index];

      struct terakan_descriptor_set const * const set =
         terakan_descriptor_set_from_handle(pDescriptorSets[set_relative_index]);
      if (set == NULL) {
         continue;
      }
      struct terakan_descriptor_set_layout const * const set_layout = set->layout;

      struct terakan_descriptor_set_resource const * const set_resources =
         (struct terakan_descriptor_set_resource const *)set->descriptors;
      struct terakan_descriptor_set_sampler const * const set_samplers =
         (struct terakan_descriptor_set_sampler const *)(set->descriptors +
                                                         set_layout
                                                            ->pool_first_sampler_offset_bytes);

      for (mesa_shader_stage shader_stage = shader_stage_first; shader_stage <= shader_stage_last;
           ++shader_stage) {
         struct terakan_descriptor_set_layout_shader const * const set_layout_shader =
            &set_layout->shaders[shader_stage];
         struct terakan_hw_config_sqk_set_functions const * const sqk_set_functions =
            &terakan_hw_config_sqk_stage_set_functions[shader_stage];

         /* Resources. */

         uint8_t const shader_resource_set_base = layout_set->first_shader_resources[shader_stage];
         struct terakan_descriptor_set_layout_shader_range const * const resource_ranges =
            set_layout->shader_ranges + set_layout_shader->first_resource_range;
         for (uint8_t range_index = 0; range_index < set_layout_shader->resource_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &resource_ranges[range_index];
            struct terakan_descriptor_set_resource const * const range_set_resources =
               set_resources + range->first_set_descriptor;
            uint8_t const range_shader_base =
               shader_resource_set_base + range->first_shader_descriptor;
            if (range->first_dynamic_offset != UINT16_MAX) {
               uint32_t const * const range_dynamic_offsets =
                  set_dynamic_offsets + range->first_dynamic_offset;
               for (uint8_t resource_index = 0; resource_index < range->descriptor_count;
                    ++resource_index) {
                  struct terakan_descriptor_set_resource const * const set_resource =
                     &range_set_resources[resource_index];
                  struct terakan_resource_descriptor resource = set_resource->resource;
                  /* Because descriptors for bindings not statically referenced by the pipeline can
                   * be undefined, the BO pointer must not be dereferenced here as it may be
                   * outdated.
                   */
                  /* TODO(Triang3l): #MemoryIntegrity with an additional remaining size variable. */
                  if (set_resource->bo != NULL) {
                     assert(G_03001C_TYPE(resource.resource[7]) ==
                            V_03001C_SQ_TEX_VTX_VALID_BUFFER);
                     uint64_t const resource_address =
                        (resource.resource[0] |
                         ((uint64_t)G_030008_BASE_ADDRESS_HI(resource.resource[2]) << 32)) +
                        range_dynamic_offsets[resource_index];
                     resource.resource[0] = (uint32_t)resource_address;
                     resource.resource[2] = (resource.resource[2] & C_030008_BASE_ADDRESS_HI) |
                                            S_030008_BASE_ADDRESS_HI(resource_address >> 32);
                  }
                  sqk_set_functions->resource(&command_writer->hw_config_sqk,
                                              range_shader_base + resource_index, set_resource->bo,
                                              &resource);
               }
            } else {
               for (uint8_t resource_index = 0; resource_index < range->descriptor_count;
                    ++resource_index) {
                  struct terakan_descriptor_set_resource const * const resource =
                     &range_set_resources[resource_index];
                  sqk_set_functions->resource(&command_writer->hw_config_sqk,
                                              range_shader_base + resource_index, resource->bo,
                                              &resource->resource);
               }
            }
         }

         /* Samplers. */

         uint8_t const shader_sampler_set_base = layout_set->first_shader_samplers[shader_stage];
         struct terakan_descriptor_set_layout_shader_range const * const sampler_ranges =
            set_layout->shader_ranges + set_layout_shader->first_sampler_range;
         for (uint8_t range_index = 0; range_index < set_layout_shader->sampler_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &sampler_ranges[range_index];
            struct terakan_descriptor_set_sampler const * const range_set_samplers =
               set_samplers + range->first_set_descriptor;
            uint8_t const range_shader_base =
               shader_sampler_set_base + range->first_shader_descriptor;
            for (uint8_t sampler_index = 0; sampler_index < range->descriptor_count;
                 ++sampler_index) {
               struct terakan_descriptor_set_sampler const * const sampler =
                  &range_set_samplers[sampler_index];
               /* Skip uninitialized (zeroed in descriptor set allocation) samplers, as descriptors
                * may be left uninitialized if they're not statically referenced by the pipeline.
                */
               if (likely(G_03C008_TYPE(sampler->sampler.sampler[2]))) {
                  sqk_set_functions->sampler(&command_writer->hw_config_sqk,
                                             range_shader_base + sampler_index, &sampler->sampler);
               }
               /* TODO(Triang3l): Unnormalized coordinates on R8xx. */
            }
         }
      }

      /* Unordered access views. */

      struct terakan_descriptor_set_layout_shader const * const set_layout_uav_shader =
         &set_layout->shaders[uav_shader_stage];
      if (set_layout_uav_shader->uav_range_count != 0) {
         struct terakan_descriptor_set_uav const * const set_uavs =
            (struct terakan_descriptor_set_uav const *)(set->descriptors +
                                                        set_layout->pool_first_uav_offset_bytes);
         assert(layout_set->first_shader_resources[uav_shader_stage] >=
                TERAKAN_RESOURCE_RANGE_MUTABLE_BASE);
         uint8_t const shader_uav_set_base = layout_set->first_shader_resources[uav_shader_stage] -
                                             TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
         struct terakan_descriptor_set_layout_shader_range const * const uav_ranges =
            set_layout->shader_ranges + set_layout_uav_shader->first_uav_range;
         for (uint8_t range_index = 0; range_index < set_layout_uav_shader->uav_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &uav_ranges[range_index];
            struct terakan_descriptor_set_uav const * const range_set_uavs =
               set_uavs + range->first_set_descriptor;
            uint8_t const range_shader_base = shader_uav_set_base + range->first_shader_descriptor;
            for (uint8_t uav_index = 0; uav_index < range->descriptor_count; ++uav_index) {
               uint8_t const shader_uav_index = range_shader_base + uav_index;
               assert(shader_uav_index < (is_compute
                                             ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL
                                             : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL));
               struct terakan_descriptor_set_uav const * const set_uav = &range_set_uavs[uav_index];
               /* Because descriptors for bindings not statically referenced by the pipeline can be
                * undefined, the BO pointer must not be dereferenced here as it may be outdated.
                */
               if (set_uav->bo != NULL) {
                  /* TODO(Triang3l): #MemoryIntegrity with an additional remaining size variable. */
                  struct terakan_color_descriptor const * new_uav_color = &set_uav->color;
                  struct terakan_color_descriptor new_uav_color_with_dynamic_offset;
                  if (range->first_dynamic_offset != UINT16_MAX) {
                     assert(G_028C70_RESOURCE_TYPE(set_uav->color.info) == V_028C70_BUFFER);
                     unsigned const uav_bytes_per_texel =
                        terascale_format_bytes_per_block[G_028C70_FORMAT(set_uav->color.info)];
                     uint64_t const new_uav_va =
                        ((uint64_t)set_uav->color.base << 8) +
                        set_uav->color.view * uav_bytes_per_texel +
                        set_dynamic_offsets[range->first_dynamic_offset + uav_index];
                     unsigned const tile_pipe_interleave_bytes_log2 =
                        terakan_gfx_command_writer_physical_device(command_writer)
                           ->tiling_info.pipe_interleave_bytes_log2;
                     uint64_t const new_uav_va_aligned =
                        new_uav_va >> tile_pipe_interleave_bytes_log2
                                         << tile_pipe_interleave_bytes_log2;
                     new_uav_color_with_dynamic_offset = set_uav->color;
                     new_uav_color_with_dynamic_offset.base = (uint32_t)(new_uav_va_aligned >> 8);
                     new_uav_color_with_dynamic_offset.view =
                        (uint32_t)(new_uav_va - new_uav_va_aligned) * uav_bytes_per_texel;
                     new_uav_color = &new_uav_color_with_dynamic_offset;
                  }
                  if (is_compute) {
                     terakan_app_config_compute_set_cb_color(&command_writer->app_config_compute,
                                                             shader_uav_index, set_uav->bo,
                                                             new_uav_color);
                  } else {
                     terakan_app_config_draw_set_cb_color_uav(&command_writer->app_config_draw,
                                                              shader_uav_index, set_uav->bo,
                                                              new_uav_color);
                  }
               } else {
                  if (is_compute) {
                     terakan_app_config_compute_set_cb_color(&command_writer->app_config_compute,
                                                             shader_uav_index, NULL, NULL);
                  } else {
                     terakan_app_config_draw_set_cb_color_uav(&command_writer->app_config_draw,
                                                              shader_uav_index, NULL, NULL);
                  }
               }
               /* TODO(Triang3l): Apply UAV bindings to the compute stage. */
            }
         }
      }

      set_dynamic_offsets += set_layout->dynamic_offset_count;
   }
}

/* `compatible_types` must be terminated with `VK_DESCRIPTOR_TYPE_MAX_ENUM`, and the first element
 * must most unambiguously describe the expected descriptor type since it's used for logging.
 */
static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_binding(nir_binding const * const binding,
                                    mesa_shader_stage const stage,
                                    struct terakan_pipeline_layout const * const pipeline_layout,
                                    VkDescriptorType const * const compatible_types)
{
   if (unlikely(binding->desc_set >= pipeline_layout->vk.set_count)) {
      vk_loge(VK_LOG_OBJS(pipeline_layout->vk.base.device),
              "Descriptor set %u doesn't exist in the pipeline layout (contains %" PRIu32
              " descriptor sets)",
              binding->desc_set, pipeline_layout->vk.set_count);
      return terakan_nir_pipeline_layout_binding_empty;
   }
   struct terakan_descriptor_set_layout const * const descriptor_set_layout =
      container_of(pipeline_layout->vk.set_layouts[binding->desc_set],
                   struct terakan_descriptor_set_layout const, vk);

   if (unlikely(binding->binding >= descriptor_set_layout->binding_count)) {
      vk_loge(VK_LOG_OBJS(pipeline_layout->vk.base.device),
              "Descriptor set %u binding %u doesn't exist in the descriptor set layout (contains "
              "%zu bindings)",
              binding->desc_set, binding->binding, descriptor_set_layout->binding_count);
      return terakan_nir_pipeline_layout_binding_empty;
   }
   struct terakan_descriptor_set_layout_binding const * const descriptor_set_binding =
      &descriptor_set_layout->bindings[binding->binding];

   /* If a `terakan_descriptor_set_layout_binding` has 0 descriptors, its fields may be
    * uninitialized. The inclusive array index range would also be impossible to calculate.
    */
   if (unlikely(descriptor_set_binding->descriptor_count == 0)) {
      vk_loge(VK_LOG_OBJS(pipeline_layout->vk.base.device),
              "Descriptor set %u binding %u doesn't contain any descriptors", binding->desc_set,
              binding->binding);
      return terakan_nir_pipeline_layout_binding_empty;
   }

   if (unlikely(!(descriptor_set_binding->stage_flags & mesa_to_vk_shader_stage(stage)))) {
      vk_loge(VK_LOG_OBJS(pipeline_layout->vk.base.device),
              "Descriptor set %u binding %u can't be accessed in a %s shader", binding->desc_set,
              binding->binding, _mesa_shader_stage_to_string(stage));
      return terakan_nir_pipeline_layout_binding_empty;
   }

   struct terakan_nir_pipeline_layout_binding pipeline_layout_binding =
      terakan_nir_pipeline_layout_binding_empty;
   pipeline_layout_binding.array_size = descriptor_set_binding->descriptor_count;
   struct terakan_pipeline_layout_set * const hw_descriptor_set_layout =
      &pipeline_layout->sets[binding->desc_set];
   /* TODO(Triang3l): Kcache when allocation is set up. */
   if (terakan_descriptor_type_has_sampler(descriptor_set_binding->descriptor_type)) {
      pipeline_layout_binding.sampler_base =
         hw_descriptor_set_layout->first_shader_samplers[stage] +
         descriptor_set_binding->first_shader_samplers[stage];
   }
   if (terakan_descriptor_type_has_resource(descriptor_set_binding->descriptor_type)) {
      pipeline_layout_binding.resource_base =
         hw_descriptor_set_layout->first_shader_resources[stage] +
         descriptor_set_binding->first_shader_resources[stage];
   }

   /* Return the binding if the type is compatible. */
   for (VkDescriptorType const * compatible_type_ptr = compatible_types;
        *compatible_type_ptr != VK_DESCRIPTOR_TYPE_MAX_ENUM; ++compatible_type_ptr) {
      if (descriptor_set_binding->descriptor_type == *compatible_type_ptr) {
         return pipeline_layout_binding;
      }
   }
   vk_loge(
      VK_LOG_OBJS(pipeline_layout->vk.base.device),
      "Descriptor set %u binding %u has type %s, which is incompatible with a shader binding with "
      "type %s",
      binding->desc_set, binding->binding,
      vk_DescriptorType_to_str(descriptor_set_binding->descriptor_type),
      vk_DescriptorType_to_str(*compatible_types));
   return terakan_nir_pipeline_layout_binding_empty;
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_sampler_binding(nir_binding const * const binding,
                                            mesa_shader_stage const stage,
                                            void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {VK_DESCRIPTOR_TYPE_SAMPLER,
                                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_sampled_image_binding(nir_binding const * const binding,
                                                  mesa_shader_stage const stage,
                                                  void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_storage_image_binding(nir_binding const * const binding,
                                                  mesa_shader_stage const stage,
                                                  void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                       VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_storage_texel_buffer_binding(nir_binding const * const binding,
                                                         mesa_shader_stage const stage,
                                                         void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                                                       VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_uniform_buffer_binding(nir_binding const * const binding,
                                                   mesa_shader_stage const stage,
                                                   void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                                       VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

static struct terakan_nir_pipeline_layout_binding
terakan_pipeline_layout_get_storage_buffer_binding(nir_binding const * const binding,
                                                   mesa_shader_stage const stage,
                                                   void const * const callback_data)
{
   static VkDescriptorType const compatible_types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                                       VK_DESCRIPTOR_TYPE_MAX_ENUM};
   return terakan_pipeline_layout_get_binding(binding, stage, callback_data, compatible_types);
}

struct terakan_nir_get_pipeline_layout_binding_functions const
   terakan_pipeline_layout_get_binding_functions = {
      .sampler = terakan_pipeline_layout_get_sampler_binding,
      .sampled_image = terakan_pipeline_layout_get_sampled_image_binding,
      .storage_image = terakan_pipeline_layout_get_storage_image_binding,
      .storage_texel_buffer = terakan_pipeline_layout_get_storage_texel_buffer_binding,
      .uniform_buffer = terakan_pipeline_layout_get_uniform_buffer_binding,
      .storage_buffer = terakan_pipeline_layout_get_storage_buffer_binding,
};

VkResult
terakan_pipeline_layout_create(struct terakan_device * const device,
                               VkPipelineLayoutCreateInfo const * const create_info,
                               VkShaderStageFlags const stage_mask,
                               struct terakan_pipeline_layout ** const pipeline_layout_out)
{
   assert(!(stage_mask & ~(VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT)));

   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout, layout, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout_set, sets,
                      create_info->setLayoutCount);
   /* Mesa pipeline layout has a different lifetime than the corresponding VkPipelineLayout since
    * other objects hold additional references to them, allocation must be done in the device scope.
    */
   if (vk_pipeline_layout_multizalloc(&device->vk, &multialloc, create_info) == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   layout->sets = sets;

   uint8_t next_first_mutable_shader_resources[MESA_SHADER_STAGES] = {};
   uint8_t next_first_shader_uniform_buffers[MESA_SHADER_STAGES] = {};
   uint8_t next_first_shader_samplers[MESA_SHADER_STAGES] = {};

   for (uint32_t set_index = 0; set_index < layout->vk.set_count; ++set_index) {
      struct vk_descriptor_set_layout const * const set_layout_base =
         layout->vk.set_layouts[set_index];
      if (set_layout_base == NULL) {
         continue;
      }
      struct terakan_descriptor_set_layout const * const set_layout =
         container_of(set_layout_base, struct terakan_descriptor_set_layout const, vk);
      struct terakan_pipeline_layout_set * const set = &layout->sets[set_index];

      unsigned remaining_stages = (unsigned)stage_mask;
      while (remaining_stages) {
         mesa_shader_stage const stage_index = vk_to_mesa_shader_stage(
            (VkShaderStageFlagBits)BITFIELD_BIT(u_bit_scan(&remaining_stages)));
         struct terakan_descriptor_set_layout_shader const * const set_layout_shader =
            &set_layout->shaders[stage_index];

         uint8_t const first_mutable_shader_resource =
            next_first_mutable_shader_resources[stage_index];
         if (terakan_resource_range_mutable_max_count(stage_index) - first_mutable_shader_resource <
             set_layout_shader->resource_count) {
            goto too_many_descriptors;
         }
         set->first_shader_resources[stage_index] =
            TERAKAN_RESOURCE_RANGE_MUTABLE_BASE + first_mutable_shader_resource;
         next_first_mutable_shader_resources[stage_index] += set_layout_shader->resource_count;

         set->first_shader_uniform_buffers[stage_index] =
            next_first_shader_uniform_buffers[stage_index];
         next_first_shader_uniform_buffers[stage_index] += set_layout_shader->uniform_buffer_count;

         uint8_t const first_shader_sampler = next_first_shader_samplers[stage_index];
         if (TERAKAN_SAMPLER_HW_COUNT_PER_STAGE - first_shader_sampler <
             set_layout_shader->sampler_count) {
            goto too_many_descriptors;
         }
         set->first_shader_samplers[stage_index] = first_shader_sampler;
         layout->shader_non_immutable_samplers[stage_index] |=
            set_layout_shader->non_immutable_samplers << first_shader_sampler;
         layout->shader_immutable_samplers_unnormalized_coordinates[stage_index] |=
            set_layout_shader->immutable_samplers_unnormalized_coordinates << first_shader_sampler;
         next_first_shader_samplers[stage_index] += set_layout_shader->sampler_count;
      }
   }

   for (uint32_t push_constant_range_index = 0;
        push_constant_range_index < create_info->pushConstantRangeCount;
        ++push_constant_range_index) {
      VkPushConstantRange const * const push_constant_range =
         &create_info->pPushConstantRanges[push_constant_range_index];
      uint32_t const push_constant_range_extent =
         push_constant_range->offset + push_constant_range->size;
      unsigned remaining_stages = (unsigned)(push_constant_range->stageFlags & stage_mask);
      while (remaining_stages) {
         uint32_t * const shader_app_push_constants_extent =
            &layout->shader_app_push_constants_extents_bytes[u_bit_scan(&remaining_stages)];
         *shader_app_push_constants_extent =
            MAX2(push_constant_range_extent, *shader_app_push_constants_extent);
      }
   }

   *pipeline_layout_out = layout;
   return VK_SUCCESS;

   /* While Vulkan implementations generally shouldn't perform validation, TeraScale has very low
    * binding count limits, while modern games demand many more. If they're launched on Terakan,
    * catch that early and report that instead of proceeding with invalid state. Doing the same for
    * push constants isn't needed as Terakan provides a much larger amount than most other drivers
    * (accurate validation of their limit also would be more complex due to cube array layer counts
    * being passed alongside push constants).
    */
too_many_descriptors:
   vk_pipeline_layout_unref(&device->vk, &layout->vk);
   return vk_errorf(
      device, VK_ERROR_VALIDATION_FAILED_EXT,
      "The application creates a pipeline layout that is too large to fit into the hardware "
      "binding register spaces");
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_vk_CreatePipelineLayout(VkDevice const deviceHandle,
                                VkPipelineLayoutCreateInfo const * const pCreateInfo,
                                UNUSED VkAllocationCallbacks const * const pAllocator,
                                VkPipelineLayout * const pPipelineLayout)
{
   struct terakan_pipeline_layout * layout;
   VkResult const result = terakan_pipeline_layout_create(
      terakan_device_from_handle(deviceHandle), pCreateInfo,
      VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, &layout);
   if (result != VK_SUCCESS) {
      return result;
   }
   *pPipelineLayout = terakan_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}
