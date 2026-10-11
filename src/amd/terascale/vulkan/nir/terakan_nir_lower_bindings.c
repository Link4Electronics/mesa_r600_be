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

#include "terakan_descriptor.h"
#include "terakan_nir.h"
#include "terakan_physical_device.h"
#include "terakan_push_constants.h"

#include "compiler/glsl_types.h"
#include "gallium/drivers/r600/eg_sq.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "nir_deref.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

static bool
terakan_nir_is_load_vulkan_descriptor(nir_instr const * const instr,
                                      UNUSED void const * const callback_data)
{
   return instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_vulkan_descriptor;
}

static nir_def *
terakan_nir_lower_load_vulkan_descriptor_to_src_impl(UNUSED nir_builder * const builder,
                                                     nir_instr * const instr,
                                                     UNUSED void * const callback_data)
{
   return nir_instr_as_intrinsic(instr)->src[0].ssa;
}

bool
terakan_nir_lower_load_vulkan_descriptor_to_src(nir_shader * const nir)
{
   return nir_shader_lower_instructions(nir, terakan_nir_is_load_vulkan_descriptor,
                                        terakan_nir_lower_load_vulkan_descriptor_to_src_impl, NULL);
}

/* `terakan_nir_lower_vulkan_resource_reindex_to_index` is done by iterating
 * `vulkan_resource_reindex` chains forward starting from `vulkan_resource_index`, so the
 * association of the additions is consistent and allowing for common subexpression elimination for
 * reindexing hierarchies.
 */

static bool
terakan_nir_lower_vulkan_resource_reindex_to_index_filter(nir_instr const * const instr,
                                                          UNUSED void const * const callback_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr const * const intrinsic = nir_instr_as_intrinsic(instr);
   if (intrinsic->intrinsic != nir_intrinsic_vulkan_resource_reindex) {
      return false;
   }
   nir_instr * const previous_index_instr = nir_def_instr(intrinsic->src[0].ssa);
   return previous_index_instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(previous_index_instr)->intrinsic ==
             nir_intrinsic_vulkan_resource_index;
}

static nir_def *
terakan_nir_lower_vulkan_resource_reindex_to_index_impl(nir_builder * const builder,
                                                        nir_instr * const instr,
                                                        UNUSED void * const callback_data)
{
   builder->cursor = nir_before_instr(instr);
   nir_intrinsic_instr const * const intrinsic = nir_instr_as_intrinsic(instr);
   nir_intrinsic_instr * const previous_index_intrinsic =
      nir_def_as_intrinsic(intrinsic->src[0].ssa);
   nir_def * const new_index = nir_vulkan_resource_index(
      builder, intrinsic->def.num_components, intrinsic->def.bit_size,
      nir_iadd_nuw(builder, previous_index_intrinsic->src[0].ssa, intrinsic->src[1].ssa));
   nir_intrinsic_copy_const_indices(nir_def_as_intrinsic(new_index), previous_index_intrinsic);
   return new_index;
}

bool
terakan_nir_lower_vulkan_resource_reindex_to_index(nir_shader * const nir)
{
   bool progress = false;
   while (nir_shader_lower_instructions(
      nir, terakan_nir_lower_vulkan_resource_reindex_to_index_filter,
      terakan_nir_lower_vulkan_resource_reindex_to_index_impl, NULL)) {
      progress = true;
   }
   return progress;
}

static bool
terakan_nir_lower_vulkan_resource_index_offset_to_index_only_filter(
   nir_instr const * const instr, UNUSED void const * const callback_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr const * const intrinsic = nir_instr_as_intrinsic(instr);
   return intrinsic->intrinsic == nir_intrinsic_vulkan_resource_index &&
          intrinsic->def.num_components > 1;
}

static nir_def *
terakan_nir_lower_vulkan_resource_index_offset_to_index_only_impl(nir_builder * const builder,
                                                                  nir_instr * const instr,
                                                                  UNUSED void * const callback_data)
{
   builder->cursor = nir_before_instr(instr);
   nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
   /* Build a vector because `nir_chase_binding` can chase through it. */
   nir_def * index_and_zero_offset_components[NIR_MAX_VEC_COMPONENTS];
   index_and_zero_offset_components[0] =
      nir_vulkan_resource_index(builder, 1, intrinsic->def.bit_size, intrinsic->src[0].ssa);
   nir_intrinsic_copy_const_indices(nir_def_as_intrinsic(index_and_zero_offset_components[0]),
                                    intrinsic);
   nir_def * const zero = nir_imm_zero(builder, 1, intrinsic->def.bit_size);
   for (unsigned component_index = 1; component_index < intrinsic->num_components;
        ++component_index) {
      index_and_zero_offset_components[component_index] = zero;
   }
   return nir_vec(builder, index_and_zero_offset_components, intrinsic->num_components);
}

bool
terakan_nir_lower_vulkan_resource_index_offset_to_index_only(nir_shader * const nir)
{
   return nir_shader_lower_instructions(
      nir, terakan_nir_lower_vulkan_resource_index_offset_to_index_only_filter,
      terakan_nir_lower_vulkan_resource_index_offset_to_index_only_impl, NULL);
}

void
terakan_nir_lower_vulkan_resource_index_passes(nir_shader * const nir)
{
   NIR_PASS(_, nir, terakan_nir_lower_load_vulkan_descriptor_to_src);
   /* Doesn't process `load_vulkan_descriptor`. */
   NIR_PASS(_, nir, terakan_nir_lower_vulkan_resource_reindex_to_index);
   /* Doesn't process `vulkan_resource_reindex`. */
   NIR_PASS(_, nir, terakan_nir_lower_vulkan_resource_index_offset_to_index_only);
}

/* Chapter 12. "Non-Normative SPIR-V Mappings" of the GLSL 4.60.8 specification says:
 *
 *     "Vulkan does not allow multi-dimensional arrays of resources like UBOs and SSBOs in its
 *     SPIR-V environment spec. SPIR-V supports it and OpenGL already allows this for GLSL shaders.
 *     SPIR-V for OpenGL also allows it."
 *
 * It's possible that for Vulkan, merely using the first index provided by `nir_chase_binding` for
 * the deref as the array index would be sufficient, but performing generic flattening instead to
 * make fewer assumptions and have more predictable behavior if these lowerings are used for client
 * APIs other than Vulkan.
 */

static void
terakan_nir_type_size_align_aoa_or_1(struct glsl_type const * const type, unsigned * const size,
                                     unsigned * const align)
{
   unsigned const type_size = glsl_type_is_array(type) ? glsl_get_aoa_size(type) : 1u;
   *size = type_size;
   *align = type_size;
}

static nir_def *
terakan_nir_build_tex_image_variable_vulkan_resource_index(nir_builder * const builder,
                                                           nir_variable const * const variable,
                                                           nir_def * const index)
{
   return nir_vulkan_resource_index(
      builder, 1, 32, index, .desc_set = variable->data.descriptor_set,
      .binding = variable->data.binding,
      .desc_count = glsl_type_is_array(variable->type) ? glsl_get_aoa_size(variable->type) : 1u,
      .resource_type = variable->data.resource_type);
}

static bool
terakan_nir_lower_tex_deref_to_vulkan_resource_index(nir_builder * const builder,
                                                     nir_tex_instr * const tex, bool is_sampler)
{
   int const deref_src_index = nir_tex_instr_src_index(
      tex, is_sampler ? nir_tex_src_sampler_deref : nir_tex_src_texture_deref);
   if (deref_src_index < 0) {
      return false;
   }
   nir_deref_instr * const deref = nir_src_as_deref(tex->src[deref_src_index].src);

   nir_variable const * const variable = nir_deref_instr_get_variable(deref);
   if (unlikely(variable == NULL)) {
      return false;
   }

   builder->cursor = nir_before_instr(&tex->instr);
   nir_def * index = nir_build_deref_offset(builder, deref, terakan_nir_type_size_align_aoa_or_1);
   /* Remove the deref source when its index is still valid. */
   nir_tex_instr_remove_src(tex, deref_src_index);
   nir_def * const offset =
      nir_steal_tex_src(tex, is_sampler ? nir_tex_src_sampler_offset : nir_tex_src_texture_offset);
   if (offset != NULL) {
      index = nir_iadd_nuw(builder, index, offset);
   }

   nir_tex_src_type const handle_src_type =
      is_sampler ? nir_tex_src_sampler_handle : nir_tex_src_texture_handle;
   assert(nir_tex_instr_src_index(tex, handle_src_type) < 0);
   nir_tex_instr_add_src(
      tex, handle_src_type,
      terakan_nir_build_tex_image_variable_vulkan_resource_index(builder, variable, index));
   return true;
}

static bool
terakan_nir_lower_tex_image_deref_to_vulkan_resource_index_instr(nir_builder * const builder,
                                                                 nir_instr * const instr,
                                                                 UNUSED void * const callback_data)
{
   bool progress = false;
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr * const tex = nir_instr_as_tex(instr);
      progress |= terakan_nir_lower_tex_deref_to_vulkan_resource_index(builder, tex, false);
      progress |= terakan_nir_lower_tex_deref_to_vulkan_resource_index(builder, tex, true);
   } else if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
      switch (intrinsic->intrinsic) {
      case nir_intrinsic_image_deref_load:
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
      case nir_intrinsic_image_deref_size:
      case nir_intrinsic_image_deref_levels:
      case nir_intrinsic_image_deref_samples: {
         nir_deref_instr * const image_deref = nir_src_as_deref(intrinsic->src[0]);
         nir_variable const * const image_variable = nir_deref_instr_get_variable(image_deref);
         if (unlikely(image_variable != NULL)) {
            progress = true;
            builder->cursor = nir_before_instr(&intrinsic->instr);
            nir_rewrite_image_intrinsic(
               intrinsic,
               terakan_nir_build_tex_image_variable_vulkan_resource_index(
                  builder, image_variable,
                  nir_build_deref_offset(builder, image_deref,
                                         terakan_nir_type_size_align_aoa_or_1)),
               nir_image_intrinsic_type_bindless);
         }
      } break;
      default:
         break;
      }
   }
   return progress;
}

bool
terakan_nir_lower_tex_image_deref_to_vulkan_resource_index(nir_shader * const nir)
{
   return nir_shader_instructions_pass(
      nir, terakan_nir_lower_tex_image_deref_to_vulkan_resource_index_instr,
      nir_metadata_control_flow, NULL);
}

struct terakan_nir_pipeline_layout_binding const terakan_nir_pipeline_layout_binding_empty = {
   .kcache_base = TERAKAN_NIR_BINDING_BASE_INVALID,
   .sampler_base = TERAKAN_NIR_BINDING_BASE_INVALID,
   .mutable_srv_base = TERAKAN_NIR_BINDING_BASE_INVALID,
};

/* Section 8.2 "Dataflow in Memory Hierarchy" of Evergreen Family Instruction Set Architecture says:
 *
 *     "Actually, buffer reads may use L1 and L2. When caching is not used for a buffer, reads from
 *     that buffer bypass L2; however, there is data sharing in L1 within a clause. After a buffer
 *     read, the line is invalidated; then, on the next read, it is read again (from the same
 *     wavefront or from a different clause). After a buffer write, the changed parts of the cache
 *     line are written to memory.
 *
 *     Buffers and images are written through the CB/DB cache, but this is flushed immediately after
 *     an image write.
 *
 *     [...]
 *
 *     Buffer objects are generally read and written directly by the work-items. Data is accessed
 *     through the L2 and L1 data caches on the GPU, but immediately invalidated at the end of a
 *     clause. Thus, data reuse is available within a wavefront for a given clause. This limited
 *     form of caching provides read coalescing among work-items in a wavefront. Similarly, writes
 *     are executed through the "fast-path" (depth buffer or DB) or "complete-path" (color buffer or
 *     CB), which have write-only caches that are invalidated, and all update bits are sent to
 *     memory at the end of a clause. The DB is the raw, high-speed, 32-bit only data write path.
 *     The CB is used for format conversion and atomics.
 *
 *     [...]
 *
 *     Image objects are limited to read-only or write-only (no concurrent r/w). Thus, on reads, the
 *     data is cached through the L2 and L1 data caches; on writes, the data is cached through the
 *     CB/DB buffers."
 *
 * For UAV write-read coherence within a draw / dispatch, therefore, the rules for
 * non-`ACCESS_NON_WRITEABLE` loads from memory potentially (including via a different binding,
 * unless `ACCESS_RESTRICT` disallows aliasing in the specific case) written to via a UAV earlier in
 * the invocation (although it may be preferable to disregard whether a write was done specifically
 * earlier, rather than simply being done at all, especially if the cache lines being read are going
 * to be overwritten anyway), or potentially written to via a UAV in another invocation in the
 * draw / dispatch if queue family scope coherence is required by the application, are the
 * following, because the CB and DB write-only caches are not coherent with the texture and vertex
 * caches:
 * - Storage buffer and storage texel buffer reads may be done via vertex fetch, but the SRV must
 *   have the `UNCACHED` field set to true, and the completion of the writes in the invocation must
 *   be acknowledged and awaited before the read.
 * - Storage images must be read using the `NOP_RTN` UAV instruction, as texture SRVs don't have the
 *   `UNCACHED` field.
 *
 * For mixing CB and DB access to the same memory locations, the requirements are not clear. Maybe
 * that's covered by "all update bits are sent to memory at the end of a clause". However, AMD's
 * OpenCL shader compiler, according to the AMD Accelerated Parallel Processing OpenCL Programming
 * Guide rev2.3 (July 2012) section 6.1.1.2 "Determining The Used Path", appears not to access UAVs
 * via the DB at all if the shader requires any CB access:
 *
 *     "The compiler is conservative when it selects memory paths. The compiler often maps all user
 *     data into a single unordered access view (UAV), so a single atomic operation (even one that
 *     is not executed) may force all loads and stores to use CompletePath."
 */

/* TODO(Triang3l): "at the end of a clause" sounds like if a UAV write is immediately followed by a
 * vertex fetch from the same memory locations, a no-op clause needs to be executed between them for
 * coherence. Research this, as well as `SCOption_R800_UAV_NONUAV_SYNC_WORKAROUND_BUG216513_1` in
 * AMD OpenCL shader compiler disassembly output. Maybe consecutive UAV exports are considered
 * implicit "clauses" from the point of view of write combining, even though they're control flow
 * instructions, it's not clear.
 *
 * Section 2.3 "Control Flow and Clauses" of Evergreen Family Instruction Set Architecture says:
 *
 *     "Clause - A homogeneous group of instructions; each clause comprises ALU, fetch through a
 *     texture cache clause, fetch through a vertex cache clause, global data share, local data
 *     share, or memory read instructions exclusively. A control flow instruction that initiates an
 *     ALU, a fetch through a texture cache clause, or fetch through a vertex cache clause does so
 *     by referring to an appropriate clause."
 *
 * All the instructions listed in this description, including `MEM_RD_*`, are executed within ALU or
 * fetch clauses, but UAV instructions are exports, which are not a part of clauses.
 */

struct terakan_nir_binding {
   /* 0 if the binding is invalid, the rest of the fields should be ignored in this case. */
   uint8_t array_size;
   uint8_t base;
   nir_scalar array_index_opt;
};

/* The builder cursor must be before the instructions that will be using the `array_index`.
 * Instructions may be added to the shader even if this function returns an invalid binding.
 */
static struct terakan_nir_binding
terakan_nir_get_binding(nir_builder * const builder, nir_binding const * const binding,
                        uint8_t const pipeline_layout_array_size,
                        uint8_t const pipeline_layout_base, bool const allow_null_array_index)
{
   assert(pipeline_layout_array_size != 0);
   assert(pipeline_layout_base != TERAKAN_NIR_BINDING_BASE_INVALID);

   assert(binding->num_indices <= 1 &&
          "Binding index offset must be flattened, such as by lowering the deref chain to "
          "`vulkan_resource_index`");

   struct terakan_nir_binding hw_binding = {
      .array_size = pipeline_layout_array_size,
      .base = pipeline_layout_base,
   };

   /* If `binding->desc_count` is 0, the shader variable is an unsized array. */
   if (binding->desc_count != 0) {
      hw_binding.array_size = MIN2(hw_binding.array_size, binding->desc_count);
   }

   if (hw_binding.array_size > 1u && binding->num_indices != 0) {
      hw_binding.array_index_opt = nir_scalar_resolved(binding->indices[0].ssa, 0);
   }

   if (hw_binding.array_index_opt.def == NULL) {
      hw_binding.array_size = 1;
   } else {
      if (nir_scalar_is_const(hw_binding.array_index_opt)) {
         uint32_t const array_index_const =
            nir_scalar_as_const_value(hw_binding.array_index_opt).u32;
         if (unlikely(array_index_const >= hw_binding.array_size)) {
            return (struct terakan_nir_binding){.base = TERAKAN_NIR_BINDING_BASE_INVALID};
         }
         hw_binding.array_size = 1;
         hw_binding.base += array_index_const;
         hw_binding.array_index_opt = (nir_scalar){};
      }
      /* TODO(Triang3l): Traverse additions and gather a constant offset that can be added to the
       * base, similarly to `nir_opt_offsets`, to avoid adding the constants using the ALU in cases
       * such as "arrays of structures" of bindings.
       */
   }

   if (!allow_null_array_index) {
      hw_binding.array_index_opt = nir_get_scalar(nir_imm_zero(builder, 1, 32), 0);
   }

   return hw_binding;
}

static struct terakan_nir_binding
terakan_nir_get_ssbo_mutable_srv_binding(
   nir_builder * const builder, nir_intrinsic_instr * const intrinsic,
   unsigned const binding_src_index,
   struct terakan_nir_get_pipeline_layout_binding_functions const * const get_binding_functions,
   void const * const get_binding_functions_data)
{
   nir_binding const binding = nir_chase_binding(intrinsic->src[binding_src_index]);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return (struct terakan_nir_binding){.base = TERAKAN_NIR_BINDING_BASE_INVALID};
   }

   struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding =
      get_binding_functions->storage_buffer(&binding, builder->shader->info.stage,
                                            get_binding_functions_data);
   if (unlikely(pipeline_layout_binding.array_size == 0)) {
      return (struct terakan_nir_binding){.base = TERAKAN_NIR_BINDING_BASE_INVALID};
   }

   return terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                                  pipeline_layout_binding.mutable_srv_base, false);
}

static struct terakan_nir_binding
terakan_nir_get_image_mutable_srv_binding(
   nir_builder * const builder, nir_intrinsic_instr * const intrinsic,
   struct terakan_nir_get_pipeline_layout_binding_functions const * const get_binding_functions,
   void const * const get_binding_functions_data)
{
   nir_binding const binding = nir_chase_binding(intrinsic->src[0]);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return (struct terakan_nir_binding){.base = TERAKAN_NIR_BINDING_BASE_INVALID};
   }

   struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding =
      (nir_intrinsic_image_dim(intrinsic) == GLSL_SAMPLER_DIM_BUF
          ? get_binding_functions->storage_texel_buffer
          : get_binding_functions->storage_image)(&binding, builder->shader->info.stage,
                                                  get_binding_functions_data);
   if (unlikely(pipeline_layout_binding.array_size == 0)) {
      return (struct terakan_nir_binding){.base = TERAKAN_NIR_BINDING_BASE_INVALID};
   }

   return terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                                  pipeline_layout_binding.mutable_srv_base, false);
}

struct terakan_nir_gather_resinfo_constants_usage_state {
   struct terakan_resinfo_constants_usage * resinfo_constants_usage;

   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions;
   void const * get_binding_functions_data;
};

static bool
terakan_nir_gather_resinfo_constants_usage_cube_elements_or_buffer_size_instr(
   nir_builder * const builder, nir_instr * const instr, void * const state_ptr)
{
   struct terakan_nir_gather_resinfo_constants_usage_state * const state = state_ptr;

   nir_src * binding_src = NULL;
   terakan_nir_get_pipeline_layout_binding_function get_binding_function = NULL;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
      switch (intrinsic->intrinsic) {
      case nir_intrinsic_get_ssbo_size:
         binding_src = &intrinsic->src[0];
         get_binding_function = state->get_binding_functions->storage_buffer;
         break;

      case nir_intrinsic_bindless_image_size:
         switch (nir_intrinsic_image_dim(intrinsic)) {
         case GLSL_SAMPLER_DIM_BUF:
            binding_src = &intrinsic->src[0];
            get_binding_function = state->get_binding_functions->storage_texel_buffer;
            break;
         default:
            return false;
         }
         break;

      default:
         return false;
      }
   } else {
      return false;
   }

   assert(binding_src != NULL);
   assert(get_binding_function != NULL);

   nir_binding const binding = nir_chase_binding(*binding_src);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return false;
   }

   struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding = get_binding_function(
      &binding, builder->shader->info.stage, state->get_binding_functions_data);
   if (unlikely(pipeline_layout_binding.array_size == 0)) {
      return false;
   }
   assert(pipeline_layout_binding.mutable_srv_base != TERAKAN_NIR_BINDING_BASE_INVALID);

   builder->cursor = nir_before_instr(instr);
   /* True must be returned from now on because `terakan_nir_get_binding` may add instructions. */
   struct terakan_nir_binding const mutable_srv_binding =
      terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                              pipeline_layout_binding.mutable_srv_base, true);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      return true;
   }

   BITSET_SET_RANGE(state->resinfo_constants_usage->cube_elements_or_buffer_size_for_mutable_srvs,
                    mutable_srv_binding.base,
                    mutable_srv_binding.base + (mutable_srv_binding.array_size - (uint8_t)1));

   return true;
}

bool
terakan_nir_gather_resinfo_constants_usage(
   nir_shader * const nir,
   struct terakan_nir_get_pipeline_layout_binding_functions const * const get_binding_functions,
   void const * const get_binding_functions_data,
   struct terakan_resinfo_constants_usage * const resinfo_constants_usage_out)
{
   *resinfo_constants_usage_out = (struct terakan_resinfo_constants_usage){};

   struct terakan_nir_gather_resinfo_constants_usage_state state = {
      .resinfo_constants_usage = resinfo_constants_usage_out,

      .get_binding_functions = get_binding_functions,
      .get_binding_functions_data = get_binding_functions_data,
   };

   return nir_shader_instructions_pass(
      nir, terakan_nir_gather_resinfo_constants_usage_cube_elements_or_buffer_size_instr,
      nir_metadata_control_flow, &state);
}

struct terakan_nir_gather_uav_usage_state {
   struct terakan_nir_shader_uav_usage * uav_usage;

   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions;
   void const * get_binding_functions_data;
};

static bool
terakan_nir_gather_uav_usage_any_written_instr(nir_builder * const builder, nir_instr * const instr,
                                               void * const state_ptr)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr const * const intrinsic = nir_instr_as_intrinsic(instr);

   struct terakan_nir_gather_uav_usage_state * const state = state_ptr;

   switch (intrinsic->intrinsic) {
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      state->uav_usage->any_buffer_written = true;
      break;
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
      if (nir_intrinsic_image_dim(intrinsic) == GLSL_SAMPLER_DIM_BUF) {
         state->uav_usage->any_buffer_written = true;
      } else {
         state->uav_usage->any_image_written = true;
      }
      break;
   default:
      return false;
   }

   return false;
}

static bool
terakan_nir_gather_uav_usage_resource_instr(nir_builder * const builder, nir_instr * const instr,
                                            void * const state_ptr)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);

   struct terakan_nir_gather_uav_usage_state * const state = state_ptr;

   unsigned binding_src_index = 0;

   terakan_nir_get_pipeline_layout_binding_function get_binding_function = NULL;

   switch (intrinsic->intrinsic) {
   case nir_intrinsic_store_ssbo:
      binding_src_index = 1;
      FALLTHROUGH;
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      get_binding_function = state->get_binding_functions->storage_buffer;
      break;

   case nir_intrinsic_bindless_image_load:
      if (nir_intrinsic_access(intrinsic) & ACCESS_NON_WRITEABLE) {
         return false;
      }
      if (nir_intrinsic_image_dim(intrinsic) == GLSL_SAMPLER_DIM_BUF) {
         /* Buffers are always read via vertex fetch. */
         return false;
      } else {
         if (!state->uav_usage->any_image_written) {
            return false;
         }
      }
      FALLTHROUGH;
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
      if (nir_intrinsic_image_dim(intrinsic) == GLSL_SAMPLER_DIM_BUF) {
         assert(state->uav_usage->any_buffer_written);
         get_binding_function = state->get_binding_functions->storage_texel_buffer;
      } else {
         assert(state->uav_usage->any_image_written);
         get_binding_function = state->get_binding_functions->storage_image;
      }
      break;

   default:
      return false;
   }

   assert(get_binding_function != NULL);

   nir_binding const binding = nir_chase_binding(intrinsic->src[binding_src_index]);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return false;
   }

   struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding = get_binding_function(
      &binding, builder->shader->info.stage, state->get_binding_functions_data);
   if (unlikely(pipeline_layout_binding.array_size == 0)) {
      return false;
   }
   assert(pipeline_layout_binding.mutable_srv_base != TERAKAN_NIR_BINDING_BASE_INVALID);

   builder->cursor = nir_before_instr(&intrinsic->instr);
   /* True must be returned from now on because `terakan_nir_get_binding` may add instructions. */
   struct terakan_nir_binding const mutable_srv_binding =
      terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                              pipeline_layout_binding.mutable_srv_base, true);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      return true;
   }

   BITSET_SET_RANGE(state->uav_usage->usage.uavs_for_mutable_srvs_needed, mutable_srv_binding.base,
                    mutable_srv_binding.base + (mutable_srv_binding.array_size - (uint8_t)1));

   return true;
}

bool
terakan_nir_gather_uav_usage(
   nir_shader * const nir,
   struct terakan_nir_get_pipeline_layout_binding_functions const * const get_binding_functions,
   void const * const get_binding_functions_data, unsigned const uav_cb_color_index_base,
   struct terakan_nir_shader_uav_usage * const uav_usage_out)
{
   *uav_usage_out = (struct terakan_nir_shader_uav_usage){};

   bool progress = false;

   struct terakan_nir_gather_uav_usage_state state = {
      .uav_usage = uav_usage_out,

      .get_binding_functions = get_binding_functions,
      .get_binding_functions_data = get_binding_functions_data,
   };

   progress |= nir_shader_instructions_pass(nir, terakan_nir_gather_uav_usage_any_written_instr,
                                            nir_metadata_all, &state);
   if (!uav_usage_out->any_buffer_written && !uav_usage_out->any_image_written) {
      return progress;
   }

   progress |= nir_shader_instructions_pass(nir, terakan_nir_gather_uav_usage_resource_instr,
                                            nir_metadata_control_flow, &state);

   /* Sanitize the total number of used UAVs. */
   {
      unsigned const stage_mutable_srv_count = terakan_srv_range_mutable_max_count(nir->info.stage);
      unsigned valid_uav_mutable_srvs_end = 0;
      unsigned cb_color_used_count = uav_cb_color_index_base;
      unsigned mutable_srv_index;
      BITSET_FOREACH_SET (mutable_srv_index, uav_usage_out->usage.uavs_for_mutable_srvs_needed,
                          stage_mutable_srv_count) {
         if (likely(cb_color_used_count < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)) {
            ++cb_color_used_count;
            valid_uav_mutable_srvs_end = mutable_srv_index + 1u;
         }
      }
      BITSET_CLEAR_RANGE(uav_usage_out->usage.uavs_for_mutable_srvs_needed,
                         valid_uav_mutable_srvs_end, stage_mutable_srv_count - 1u);
   }

   return progress;
}

/* Returns `TERAKAN_NIR_BINDING_BASE_INVALID` if the given SRV range doesn't have corresponding
 * UAVs.
 */
static uint8_t
terakan_nir_get_mutable_srv_range_uav_base(
   uint8_t const mutable_srv_base, uint8_t const array_size,
   struct terakan_nir_shader_uav_usage const * const uav_usage, mesa_shader_stage const stage)
{
   assert(array_size != 0 &&
          "Bindings with an array size of 0 are considered invalid and must be sanitized");
   /* Checking that all SRVs in the array have corresponding UAVs, without taking shortcuts like
    * checking only the first SRV, in particular to make sure the IMMED buffer SRV usage doesn't
    * overflow the range those buffers must be in.
    */
   for (uint8_t array_index = 0; array_index < array_size; ++array_index) {
      if (!BITSET_TEST(uav_usage->usage.uavs_for_mutable_srvs_needed,
                       mutable_srv_base + array_index)) {
         return TERAKAN_NIR_BINDING_BASE_INVALID;
      }
   }
   uint8_t uav_base = 0;
   unsigned mutable_srv_index;
   BITSET_FOREACH_SET (mutable_srv_index, uav_usage->usage.uavs_for_mutable_srvs_needed,
                       mutable_srv_base) {
      ++uav_base;
   }
   return uav_base;
}

/* If there was an error while lowering the binding, such as if the binding was not obtained
 * successfully, to avoid leaving the shader in an indeterminate state as it's very easy for an
 * application to cause issues like those, treating invalid instructions largely as null descriptor
 * accesses - returning zero for loads, ignoring stores.
 */
static void
terakan_nir_lower_bindings_instr_to_null(nir_instr * const instr)
{
   nir_def * const instr_def = nir_instr_def(instr);
   if (instr_def != NULL) {
      nir_builder builder = nir_builder_at(nir_before_instr(instr));
      nir_def_rewrite_uses(instr_def,
                           nir_imm_zero(&builder, instr_def->num_components, instr_def->bit_size));
   }
   nir_instr_remove(instr);
}

struct terakan_nir_lower_bindings_state {
   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions;
   void const * get_binding_functions_data;

   bool robust_buffer_access_2;

   struct terakan_physical_device const * physical_device;

   struct terakan_resinfo_constants_usage const * resinfo_constants_usage;

   struct terakan_nir_shader_uav_usage uav_usage;

   struct terakan_shader_sqk_usage * sqk_usage_accum;
   uint32_t * driver_push_constants_used_accum;
};

static nir_def *
terakan_nir_load_resinfo_constants_cube_elements_or_buffer_size(
   nir_builder * const builder, uint8_t const mutable_srv_index_base, nir_def * const array_index,
   bool const include_helpers, struct terakan_nir_lower_bindings_state * const state)
{
   BITSET_WORD const * const cube_elements_or_buffer_size_for_mutable_srvs =
      state->resinfo_constants_usage->cube_elements_or_buffer_size_for_mutable_srvs;
   assert(BITSET_TEST(cube_elements_or_buffer_size_for_mutable_srvs, mutable_srv_index_base));
   uint8_t constant_dword_index = 0;
   unsigned preceding_mutable_srv_index;
   BITSET_FOREACH_SET (preceding_mutable_srv_index, cube_elements_or_buffer_size_for_mutable_srvs,
                       mutable_srv_index_base) {
      ++constant_dword_index;
   }
   BITSET_SET(state->sqk_usage_accum->srvs, TERAKAN_SRV_RANGE_RESINFO_CONSTANTS);
   return nir_load_buffer_resource_r600(
      builder, 1, 32, nir_imm_zero(builder, 1, 32), array_index,
      .access = ACCESS_CAN_REORDER |
                (include_helpers && builder->shader->info.stage == MESA_SHADER_FRAGMENT
                    ? ACCESS_INCLUDE_HELPERS
                    : 0),
      .binding = TERAKAN_SRV_RANGE_RESINFO_CONSTANTS,
      .base = (uint16_t)sizeof(uint32_t) * constant_dword_index, .format = PIPE_FORMAT_R32_UINT);
}

/* Returns 0 in case of failure. */
static unsigned
terakan_nir_atomic_uav_op(nir_atomic_op const atomic_op, bool const result_used, bool const is_r9xx)
{
   unsigned uav_op;
   switch (atomic_op) {
   case nir_atomic_op_iadd:
      uav_op = V_RAT_INST_ADD;
      break;
   case nir_atomic_op_isub:
      uav_op = V_RAT_INST_SUB;
      break;
   case nir_atomic_op_imin:
      uav_op = V_RAT_INST_MIN_INT;
      break;
   case nir_atomic_op_umin:
      uav_op = V_RAT_INST_MIN_UINT;
      break;
   case nir_atomic_op_imax:
      uav_op = V_RAT_INST_MAX_INT;
      break;
   case nir_atomic_op_umax:
      uav_op = V_RAT_INST_MAX_UINT;
      break;
   case nir_atomic_op_iand:
      uav_op = V_RAT_INST_AND;
      break;
   case nir_atomic_op_ior:
      uav_op = V_RAT_INST_OR;
      break;
   case nir_atomic_op_ixor:
      uav_op = V_RAT_INST_XOR;
      break;
   case nir_atomic_op_xchg:
      return result_used ? V_RAT_INST_XCHG_RTN
                         : (is_r9xx ? V_RAT_INST_STORE_DWORD : V_RAT_INST_STORE_RAW);
   case nir_atomic_op_cmpxchg:
      uav_op = V_RAT_INST_CMPXCHG_INT;
      break;
   case nir_atomic_op_inc_wrap:
      uav_op = V_RAT_INST_INC_UINT;
      break;
   case nir_atomic_op_dec_wrap:
      uav_op = V_RAT_INST_DEC_UINT;
      break;
   default:
      assert(!"Unsupported atomic operation");
      return 0;
   }
   if (result_used) {
      uav_op |= 0x20;
   }
   return uav_op;
}

static nir_def *
terakan_nir_uav_immed_coord(nir_builder * const builder,
                            struct terakan_physical_device_chip_info const * const chip_info)
{
   nir_def * wave_id = nir_load_hw_wave_id_r600(builder);
   if (chip_info->two_shader_engines_max) {
      wave_id = nir_umad24_relaxed(builder, wave_id, nir_imm_int(builder, 2),
                                   nir_load_shader_engine_id_r600(builder));
   }
   /* TODO(Triang3l): See how `MBCNT` behaves on wave32 chips, and possibly scale the wave ID by 32
    * there.
    */
   return nir_umad24_relaxed(
      builder, wave_id, nir_imm_int(builder, 64),
      nir_mbcnt_amd(builder, nir_imm_int(builder, ~0), nir_imm_zero(builder, 1, 32)));
}

static nir_def *
terakan_nir_buffer_uav_coord(nir_builder * const builder, nir_def * coord, uint8_t const uav_base,
                             nir_def * const uav_array_index, bool const include_helpers,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Add the UAV pipe interleave alignment offset. */

   if (state->robust_buffer_access_2) {
      /* If the coordinate provided by the application is already near `UINT32_MAX`, adding the
       * alignment offset may result in wrapping and turn an out-of-bounds address into a in-bounds
       * address near zero. Clamp the coordinate so that no possible alignment offset value will
       * result in wrapping.
       */
      coord = nir_umin_imm(
         builder, coord,
         ~(((uint32_t)1 << state->physical_device->tiling_info.pipe_interleave_bytes_log2) -
           (uint32_t)1));
   }

   *state->driver_push_constants_used_accum |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_ALIGNMENT_OFFSET);

   /* TODO(Triang3l): If the array index is constant, load via kcache rather than vertex fetch. */
   BITSET_SET(state->sqk_usage_accum->srvs, TERAKAN_SRV_RANGE_PUSH_CONSTANTS);
   nir_def * const alignment_offset = nir_load_buffer_resource_r600(
      builder, 1, 32, nir_imm_zero(builder, 1, 32), nir_ishl_imm(builder, uav_array_index, 2),
      .access = ACCESS_CAN_REORDER | (include_helpers ? ACCESS_INCLUDE_HELPERS : 0),
      .binding = TERAKAN_SRV_RANGE_PUSH_CONSTANTS,
      .base = offsetof(struct terakan_push_constants_driver, buffer_uav_alignment_offset) +
              sizeof(uint32_t) * uav_base,
      .format = PIPE_FORMAT_R32_UINT);
   coord = nir_iadd_nuw(builder, coord, alignment_offset);

   return coord;
}

static nir_def *
terakan_nir_image_uav_coord(nir_builder * const builder, nir_def * const image_coord,
                            enum glsl_sampler_dim const dimensionality, bool const is_array)
{
   /* Buffers need separate handling due to the UAV alignment offset. */
   assert(dimensionality != GLSL_SAMPLER_DIM_BUF);

   unsigned uav_coord_num_components = 2;
   nir_def * uav_coord_components[3] = {
      nir_channel(builder, image_coord, 0),
      /* 1D images may be promoted to 2D if they're tiled (such as if they're used by DB), so always
       * specify Y = 0 for them.
       */
      dimensionality == GLSL_SAMPLER_DIM_1D ? nir_imm_zero(builder, 1, 32)
                                            : nir_channel(builder, image_coord, 1),
   };

   /* The hardware accepts the array layer in Z for both 1D and 2D/3D. It's relevant only if
    * `RESOURCE_TYPE` is `TEXTURE#DARRAY` or `TEXTURE3D`. UAV instructions don't accept a coordinate
    * swizzle, so don't initialize the array layer if it's not needed to avoid emitting an ALU
    * instruction for it.
    */
   if (dimensionality == GLSL_SAMPLER_DIM_3D || is_array) {
      uav_coord_num_components = 3;
      uav_coord_components[2] =
         nir_channel(builder, image_coord, dimensionality == GLSL_SAMPLER_DIM_1D ? 1 : 2);
   }

   return nir_vec(builder, uav_coord_components, uav_coord_num_components);
}

static bool
terakan_nir_lower_bindings_instr_tex(nir_builder * const builder, nir_tex_instr * const tex,
                                     struct terakan_nir_lower_bindings_state * const state)
{
   bool progress = false;
   bool bindings_invalid = false;

   /* TODO(Triang3l): Load the texture size from the push constant buffer where necessary (cube
    * array layer count, buffer size on certain chips). First mark the SRVs that need the size in
    * the buffer, then run a compaction pass. Maybe, however, do this as part of a separate lowering
    * before this lowering to be able to eliminate the texture instruction if only the components
    * loaded from the buffer are needed.
    */

   /* TODO(Triang3l): Unnormalized coordinates conditional for indirect sampler indexing, if not
    * known from the immutable samplers.
    */

   {
      int const handle_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);
      if (handle_src_index >= 0) {
         nir_binding const binding = nir_chase_binding(tex->src[handle_src_index].src);
         assert(binding.success);
         if (unlikely(!binding.success)) {
            bindings_invalid = true;
         } else {
            assert(nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) < 0 &&
                   "Texture offset source must be lowered to the index in the handle source");
            struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding =
               state->get_binding_functions->sampled_image(&binding, builder->shader->info.stage,
                                                           state->get_binding_functions_data);
            if (unlikely(pipeline_layout_binding.array_size == 0)) {
               bindings_invalid = true;
            } else {
               progress = true;
               struct terakan_nir_binding const hw_mutable_srv_binding =
                  terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                                          pipeline_layout_binding.mutable_srv_base, true);
               if (unlikely(hw_mutable_srv_binding.array_size == 0)) {
                  bindings_invalid = true;
               } else {
                  uint8_t const first_srv_index =
                     TERAKAN_SRV_RANGE_MUTABLE_BASE + hw_mutable_srv_binding.base;
                  BITSET_SET_RANGE(
                     state->sqk_usage_accum->srvs, first_srv_index,
                     first_srv_index + (hw_mutable_srv_binding.array_size - (uint8_t)1));
                  tex->texture_index = first_srv_index;
                  nir_tex_instr_remove_src(tex, handle_src_index);
                  if (hw_mutable_srv_binding.array_index_opt.def != NULL) {
                     nir_tex_instr_add_src(tex, nir_tex_src_texture_offset,
                                           hw_mutable_srv_binding.array_index_opt.def);
                  }
               }
            }
         }
      }
   }

   {
      int const handle_src_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle);
      if (handle_src_index >= 0) {
         nir_binding const binding = nir_chase_binding(tex->src[handle_src_index].src);
         assert(binding.success);
         if (unlikely(!binding.success)) {
            bindings_invalid = true;
         } else {
            assert(nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) < 0 &&
                   "Sampler offset source must be lowered to the index in the handle source");
            struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding =
               state->get_binding_functions->sampler(&binding, builder->shader->info.stage,
                                                     state->get_binding_functions_data);
            if (unlikely(pipeline_layout_binding.array_size == 0)) {
               bindings_invalid = true;
            } else {
               progress = true;
               struct terakan_nir_binding const hw_sampler_binding =
                  terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                                          pipeline_layout_binding.sampler_base, true);
               if (unlikely(hw_sampler_binding.array_size == 0)) {
                  bindings_invalid = true;
               } else {
                  state->sqk_usage_accum->samplers |=
                     BITFIELD_RANGE(hw_sampler_binding.base, hw_sampler_binding.array_size);
                  tex->sampler_index = hw_sampler_binding.base;
                  nir_tex_instr_remove_src(tex, handle_src_index);
                  if (hw_sampler_binding.array_index_opt.def != NULL) {
                     nir_tex_instr_add_src(tex, nir_tex_src_sampler_offset,
                                           hw_sampler_binding.array_index_opt.def);
                  }
               }
            }
         }
      }
   }

   if (unlikely(bindings_invalid)) {
      terakan_nir_lower_bindings_instr_to_null(&tex->instr);
      return true;
   }

   return progress;
}

static void
terakan_nir_lower_bindings_instr_load_constant(nir_builder * const builder,
                                               nir_intrinsic_instr * const intrinsic,
                                               struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_load_constant);

   BITSET_SET(state->sqk_usage_accum->srvs, TERAKAN_SRV_RANGE_IMMEDIATE_CONSTANTS_OR_META_PRIMARY);
   nir_def_replace(
      &intrinsic->def,
      terakan_nir_load_raw_buffer_srv(
         builder, intrinsic->num_components, intrinsic->def.bit_size, ACCESS_CAN_REORDER,
         TERAKAN_SRV_RANGE_IMMEDIATE_CONSTANTS_OR_META_PRIMARY, nir_imm_zero(builder, 1, 32),
         nir_intrinsic_base(intrinsic), intrinsic->src[0].ssa));
}

static void
terakan_nir_lower_bindings_instr_load_push_constant(
   nir_builder * const builder, nir_intrinsic_instr * const intrinsic,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_load_push_constant);

   /* Push constants don't have access robustness, simply add the base without an integer overflow
    * check.
    */

   /* TODO(Triang3l): Load from the kcache. */

   BITSET_SET(state->sqk_usage_accum->srvs, TERAKAN_SRV_RANGE_PUSH_CONSTANTS);
   nir_def_replace(
      &intrinsic->def,
      terakan_nir_load_raw_buffer_srv(
         builder, intrinsic->num_components, intrinsic->def.bit_size, ACCESS_CAN_REORDER,
         TERAKAN_SRV_RANGE_PUSH_CONSTANTS, nir_imm_zero(builder, 1, 32),
         TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES + nir_intrinsic_base(intrinsic),
         intrinsic->src[0].ssa));
}

static void
terakan_nir_lower_bindings_instr_load_ubo(nir_builder * const builder,
                                          nir_intrinsic_instr * const intrinsic,
                                          struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_load_ubo);

   nir_binding const binding = nir_chase_binding(intrinsic->src[0]);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   struct terakan_nir_pipeline_layout_binding const pipeline_layout_binding =
      state->get_binding_functions->uniform_buffer(&binding, builder->shader->info.stage,
                                                   state->get_binding_functions_data);
   if (unlikely(pipeline_layout_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   /* TODO(Triang3l): Load from the kcache. */

   struct terakan_nir_binding const mutable_srv_binding =
      terakan_nir_get_binding(builder, &binding, pipeline_layout_binding.array_size,
                              pipeline_layout_binding.mutable_srv_base, false);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   uint8_t const first_srv_index = TERAKAN_SRV_RANGE_MUTABLE_BASE + mutable_srv_binding.base;
   BITSET_SET_RANGE(state->sqk_usage_accum->srvs, first_srv_index,
                    first_srv_index + (mutable_srv_binding.array_size - (uint8_t)1));
   nir_def_replace(
      &intrinsic->def,
      terakan_nir_load_raw_buffer_srv(
         builder, intrinsic->num_components, intrinsic->def.bit_size,
         nir_intrinsic_access(intrinsic) | ACCESS_CAN_REORDER, first_srv_index,
         nir_mov_scalar(builder, mutable_srv_binding.array_index_opt), 0, intrinsic->src[1].ssa));
}

static void
terakan_nir_lower_bindings_instr_load_ssbo(nir_builder * const builder,
                                           nir_intrinsic_instr * const intrinsic,
                                           struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_load_ssbo);

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_ssbo_mutable_srv_binding(
      builder, intrinsic, 0, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   enum gl_access_qualifier access = nir_intrinsic_access(intrinsic);
   bool const invocation_scope_coherent =
      !(access & ACCESS_NON_WRITEABLE) && state->uav_usage.any_buffer_written;
   if (invocation_scope_coherent) {
      access &= ~ACCESS_CAN_REORDER;
   } else {
      access |= ACCESS_CAN_REORDER;
   }

   uint8_t const first_srv_index = TERAKAN_SRV_RANGE_MUTABLE_BASE + mutable_srv_binding.base;
   uint8_t const last_srv_index = first_srv_index + (mutable_srv_binding.array_size - (uint8_t)1);
   BITSET_SET_RANGE(state->sqk_usage_accum->srvs, first_srv_index, last_srv_index);
   if (!invocation_scope_coherent) {
      BITSET_SET_RANGE(state->sqk_usage_accum->srvs_uncached, first_srv_index, last_srv_index);
   }

   nir_def_replace(
      &intrinsic->def,
      terakan_nir_load_raw_buffer_srv(
         builder, intrinsic->num_components, intrinsic->def.bit_size, access, first_srv_index,
         nir_mov_scalar(builder, mutable_srv_binding.array_index_opt), 0, intrinsic->src[1].ssa));
}

static void
terakan_nir_lower_bindings_instr_store_ssbo(nir_builder * const builder,
                                            nir_intrinsic_instr * const intrinsic,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_store_ssbo);

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_ssbo_mutable_srv_binding(
      builder, intrinsic, 1, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      nir_instr_remove(&intrinsic->instr);
      return;
   }

   uint8_t const uav_base = terakan_nir_get_mutable_srv_range_uav_base(
      mutable_srv_binding.base, mutable_srv_binding.array_size, &state->uav_usage,
      builder->shader->info.stage);
   if (unlikely(uav_base == TERAKAN_NIR_BINDING_BASE_INVALID)) {
      nir_instr_remove(&intrinsic->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrinsic);

   nir_def * const binding_array_index =
      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt);

   nir_def * uav_coord =
      terakan_nir_buffer_uav_coord(builder, intrinsic->src[2].ssa, uav_base, binding_array_index,
                                   (access & ACCESS_INCLUDE_HELPERS) != 0, state);

   /* `STORE_RAW` / `STORE_DWORD` can write 1 dword via `MEM_RAT` or up to 2 dwords via
    * `MEM_RAT_CACHELESS`, but currently `MEM_RAT` is always used.
    *
    * On R8xx, vectorization of 8-bit and 16-bit stores also may be possible with 32-bit alignment,
    * as well as vectorization of two 8-bit stores with 16-bit alignment, if the `MSKOR` mask is
    * constructed from the write mask, and the components are combined into 32 bits (potentially
    * using binary reduction, and `umad24` instead of shift and OR where possible - the << 24 byte
    * would need either a shift or two multiplications), but this would add significant complexity
    * to both the store intrinsic and the vectorization.
    */
   /* TODO(Triang3l): Support 2-dword writes for `MEM_RAT_CACHELESS`. */
   assert(nir_intrinsic_write_mask(intrinsic) == 0b1);
   nir_def * value = nir_channel(builder, intrinsic->src[0].ssa, 0);
   unsigned const bits_per_component = value->bit_size;
   nir_def * value_mask = NULL;
   unsigned uav_op;
   if (bits_per_component == 32) {
      uav_op =
         state->physical_device->chip_info.is_r9xx ? V_RAT_INST_STORE_DWORD : V_RAT_INST_STORE_RAW;
      uav_coord = nir_ushr_imm(builder, uav_coord, 2);
   } else {
      value = nir_u2u32(builder, intrinsic->src[0].ssa);
      assert(bits_per_component == 8 || bits_per_component == 16);
      unsigned const bytes_per_component = bits_per_component / 8u;
      if (state->physical_device->chip_info.is_r9xx) {
         uav_op = bytes_per_component == 2 ? V_RAT_INST_STORE_SHORT : V_RAT_INST_STORE_BYTE;
         uav_coord = nir_ushr_imm(builder, uav_coord, ffs(bytes_per_component) - 1);
      } else {
         uav_op = V_RAT_INST_MSKOR;
         nir_def * const byte_address = uav_coord;
         uav_coord = nir_ushr_imm(builder, uav_coord, 2);
         if (nir_intrinsic_align(intrinsic) >= 4) {
#if UTIL_ARCH_BIG_ENDIAN
            value = nir_ishl_imm(builder, value, 32 - bits_per_component);
            value_mask = nir_imm_int(builder, (int)~BITFIELD_MASK(32 - bits_per_component));
#else
            value_mask = nir_imm_int(builder, (int)BITFIELD_MASK(bits_per_component));
#endif
         } else {
            nir_def * byte_offset_in_dword =
               nir_iand(builder, byte_address, nir_imm_int(builder, 3));
#if UTIL_ARCH_BIG_ENDIAN
            byte_offset_in_dword = nir_ixor(builder, byte_offset_in_dword,
                                            nir_imm_int(builder, (int)(4u - bytes_per_component)));
#endif
            nir_def * const bit_offset_in_dword = nir_ishl_imm(builder, byte_offset_in_dword, 3);
            value = nir_ishl(builder, value, bit_offset_in_dword);
            value_mask =
               nir_ishl(builder, nir_imm_int(builder, (int)BITFIELD_MASK(bits_per_component)),
                        bit_offset_in_dword);
         }
      }
   }

   nir_uav_instr_r600(builder, binding_array_index, uav_coord, value,
                      value_mask != NULL ? value_mask : nir_undef(builder, 1, 32),
                      .uav_op_r600 = uav_op, .access = access, .binding = uav_base);
   nir_instr_remove(&intrinsic->instr);
}

static void
terakan_nir_lower_bindings_instr_ssbo_atomic(nir_builder * const builder,
                                             nir_intrinsic_instr * const intrinsic,
                                             struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_ssbo_atomic ||
          intrinsic->intrinsic == nir_intrinsic_ssbo_atomic_swap);

   bool const result_used = !list_is_empty(&intrinsic->def.uses);
   unsigned const uav_op = terakan_nir_atomic_uav_op(
      nir_intrinsic_atomic_op(intrinsic), result_used, state->physical_device->chip_info.is_r9xx);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_ssbo_mutable_srv_binding(
      builder, intrinsic, 0, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   uint8_t const uav_base = terakan_nir_get_mutable_srv_range_uav_base(
      mutable_srv_binding.base, mutable_srv_binding.array_size, &state->uav_usage,
      builder->shader->info.stage);
   if (unlikely(uav_base == TERAKAN_NIR_BINDING_BASE_INVALID)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrinsic);

   nir_def * const binding_array_index =
      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt);

   nir_def * const uav_coord = nir_ushr_imm(
      builder,
      terakan_nir_buffer_uav_coord(builder, intrinsic->src[1].ssa, uav_base, binding_array_index,
                                   (access & ACCESS_INCLUDE_HELPERS) != 0, state),
      2);

   nir_def * const compare_value = intrinsic->intrinsic == nir_intrinsic_ssbo_atomic_swap
                                      ? intrinsic->src[3].ssa
                                      : nir_undef(builder, 1, 32);

   if (result_used) {
      nir_def_replace(
         &intrinsic->def,
         nir_uav_returning_instr_r600(
            builder, 1, 32, binding_array_index, uav_coord, intrinsic->src[2].ssa, compare_value,
            terakan_nir_uav_immed_coord(builder, &state->physical_device->chip_info),
            .uav_op_r600 = uav_op, .access = access, .binding = uav_base,
            .uav_return_binding_r600 = (builder->shader->info.stage == MESA_SHADER_FRAGMENT
                                           ? TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                           : TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                       uav_base,
            .mega_fetch_count_r600 = sizeof(uint32_t)));
   } else {
      nir_uav_instr_r600(builder, binding_array_index, uav_coord, intrinsic->src[2].ssa,
                         compare_value, .uav_op_r600 = uav_op, .access = access,
                         .binding = uav_base);
      nir_def_replace(&intrinsic->def,
                      nir_undef(builder, intrinsic->def.num_components, intrinsic->def.bit_size));
   }
}

static void
terakan_nir_lower_bindings_instr_get_ssbo_size(nir_builder * const builder,
                                               nir_intrinsic_instr * const intrinsic,
                                               struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_get_ssbo_size);

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_ssbo_mutable_srv_binding(
      builder, intrinsic, 0, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   nir_def_replace(&intrinsic->def,
                   terakan_nir_load_resinfo_constants_cube_elements_or_buffer_size(
                      builder, mutable_srv_binding.base,
                      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt), true, state));
}

static void
terakan_nir_lower_bindings_instr_image_load(nir_builder * const builder,
                                            nir_intrinsic_instr * const intrinsic,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_bindless_image_load);

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_image_mutable_srv_binding(
      builder, intrinsic, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   enum glsl_sampler_dim const dimensionality = nir_intrinsic_image_dim(intrinsic);
   bool const is_buffer = dimensionality == GLSL_SAMPLER_DIM_BUF;

   enum gl_access_qualifier access = nir_intrinsic_access(intrinsic);
   bool const invocation_scope_coherent =
      !(access & ACCESS_NON_WRITEABLE) &&
      (is_buffer ? state->uav_usage.any_buffer_written : state->uav_usage.any_image_written);
   if (invocation_scope_coherent) {
      access &= ~ACCESS_CAN_REORDER;
   } else {
      access |= ACCESS_CAN_REORDER;
   }

   nir_def * const binding_array_index =
      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt);

   uint8_t const first_srv_index = TERAKAN_SRV_RANGE_MUTABLE_BASE + mutable_srv_binding.base;
   uint8_t const last_srv_index = first_srv_index + (mutable_srv_binding.array_size - (uint8_t)1);

   nir_def * const image_coord = intrinsic->src[1].ssa;

   if (is_buffer) {
      BITSET_SET_RANGE(state->sqk_usage_accum->srvs, first_srv_index, last_srv_index);
      if (!invocation_scope_coherent) {
         BITSET_SET_RANGE(state->sqk_usage_accum->srvs_uncached, first_srv_index, last_srv_index);
      }
      nir_def_replace(&intrinsic->def, nir_load_buffer_resource_r600(
                                          builder, intrinsic->def.num_components, 32,
                                          binding_array_index, nir_channel(builder, image_coord, 0),
                                          .access = access, .binding = first_srv_index));
      return;
   }

   bool const image_is_array = nir_intrinsic_image_array(intrinsic);

   uint8_t const uav_base = invocation_scope_coherent
                               ? terakan_nir_get_mutable_srv_range_uav_base(
                                    mutable_srv_binding.base, mutable_srv_binding.array_size,
                                    &state->uav_usage, builder->shader->info.stage)
                               : TERAKAN_NIR_BINDING_BASE_INVALID;

   if (uav_base == TERAKAN_NIR_BINDING_BASE_INVALID) {
      BITSET_SET_RANGE(state->sqk_usage_accum->srvs, first_srv_index, last_srv_index);
      nir_def_replace(
         &intrinsic->def,
         nir_load_texture_resource_r600(
            builder, intrinsic->def.num_components, 32, binding_array_index,
            nir_vec4(
               builder, nir_channel(builder, image_coord, 0),
               dimensionality != GLSL_SAMPLER_DIM_1D ? nir_channel(builder, image_coord, 1)
                                                     : nir_imm_zero(builder, 1, 32),
               dimensionality == GLSL_SAMPLER_DIM_3D || image_is_array
                  ? nir_channel(builder, image_coord, dimensionality == GLSL_SAMPLER_DIM_1D ? 1 : 2)
                  : nir_imm_zero(builder, 1, 32),
               nir_imm_zero(builder, 1, 32)),
            .access = access, .binding = first_srv_index));
      return;
   }

   nir_def * const undef = nir_undef(builder, 1, 32);
   nir_def_replace(
      &intrinsic->def,
      nir_uav_returning_instr_r600(
         builder, 1, 32, binding_array_index,
         terakan_nir_image_uav_coord(builder, image_coord, dimensionality, image_is_array), undef,
         undef, terakan_nir_uav_immed_coord(builder, &state->physical_device->chip_info),
         .uav_op_r600 = V_RAT_INST_NOP_RTN, .access = access, .binding = uav_base,
         .uav_return_binding_r600 = (builder->shader->info.stage == MESA_SHADER_FRAGMENT
                                        ? TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                        : TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                    uav_base));
}

static void
terakan_nir_lower_bindings_instr_image_store(nir_builder * const builder,
                                             nir_intrinsic_instr * const intrinsic,
                                             struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_bindless_image_store);

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_image_mutable_srv_binding(
      builder, intrinsic, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      nir_instr_remove(&intrinsic->instr);
      return;
   }

   uint8_t const uav_base = terakan_nir_get_mutable_srv_range_uav_base(
      mutable_srv_binding.base, mutable_srv_binding.array_size, &state->uav_usage,
      builder->shader->info.stage);
   if (unlikely(uav_base == TERAKAN_NIR_BINDING_BASE_INVALID)) {
      nir_instr_remove(&intrinsic->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrinsic);

   nir_def * const binding_array_index =
      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt);

   enum glsl_sampler_dim const dimensionality = nir_intrinsic_image_dim(intrinsic);

   nir_uav_instr_r600(
      builder, binding_array_index,
      dimensionality == GLSL_SAMPLER_DIM_BUF
         ? terakan_nir_buffer_uav_coord(builder, nir_channel(builder, intrinsic->src[1].ssa, 0),
                                        uav_base, binding_array_index,
                                        (access & ACCESS_INCLUDE_HELPERS) != 0, state)
         : terakan_nir_image_uav_coord(builder, intrinsic->src[1].ssa, dimensionality,
                                       nir_intrinsic_image_array(intrinsic)),
      intrinsic->src[3].ssa, nir_undef(builder, 1, 32), .uav_op_r600 = V_RAT_INST_STORE_TYPED,
      .access = access, .binding = uav_base);
   nir_instr_remove(&intrinsic->instr);
}

static void
terakan_nir_lower_bindings_instr_image_atomic(nir_builder * const builder,
                                              nir_intrinsic_instr * const intrinsic,
                                              struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_bindless_image_atomic ||
          intrinsic->intrinsic == nir_intrinsic_bindless_image_atomic_swap);

   bool const result_used = !list_is_empty(&intrinsic->def.uses);
   unsigned const uav_op = terakan_nir_atomic_uav_op(
      nir_intrinsic_atomic_op(intrinsic), result_used, state->physical_device->chip_info.is_r9xx);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   struct terakan_nir_binding const mutable_srv_binding = terakan_nir_get_image_mutable_srv_binding(
      builder, intrinsic, state->get_binding_functions, state->get_binding_functions_data);
   if (unlikely(mutable_srv_binding.array_size == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   uint8_t const uav_base = terakan_nir_get_mutable_srv_range_uav_base(
      mutable_srv_binding.base, mutable_srv_binding.array_size, &state->uav_usage,
      builder->shader->info.stage);
   if (unlikely(uav_base == TERAKAN_NIR_BINDING_BASE_INVALID)) {
      terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrinsic);

   nir_def * const binding_array_index =
      nir_mov_scalar(builder, mutable_srv_binding.array_index_opt);

   enum glsl_sampler_dim const dimensionality = nir_intrinsic_image_dim(intrinsic);

   nir_def * const uav_coord =
      dimensionality == GLSL_SAMPLER_DIM_BUF
         ? terakan_nir_buffer_uav_coord(builder, nir_channel(builder, intrinsic->src[1].ssa, 0),
                                        uav_base, binding_array_index,
                                        (access & ACCESS_INCLUDE_HELPERS) != 0, state)
         : terakan_nir_image_uav_coord(builder, intrinsic->src[1].ssa, dimensionality,
                                       nir_intrinsic_image_array(intrinsic));

   nir_def * const compare_value = intrinsic->intrinsic == nir_intrinsic_bindless_image_atomic_swap
                                      ? intrinsic->src[4].ssa
                                      : nir_undef(builder, 1, 32);

   if (result_used) {
      nir_def_replace(
         &intrinsic->def,
         nir_uav_returning_instr_r600(
            builder, 1, 32, binding_array_index, uav_coord, intrinsic->src[3].ssa, compare_value,
            terakan_nir_uav_immed_coord(builder, &state->physical_device->chip_info),
            .uav_op_r600 = uav_op, .access = access, .binding = uav_base,
            .uav_return_binding_r600 = (builder->shader->info.stage == MESA_SHADER_FRAGMENT
                                           ? TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                           : TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                       uav_base,
            .mega_fetch_count_r600 = sizeof(uint32_t)));
   } else {
      nir_uav_instr_r600(builder, binding_array_index, uav_coord, intrinsic->src[3].ssa,
                         compare_value, .uav_op_r600 = uav_op, .access = access,
                         .binding = uav_base);
      nir_def_replace(&intrinsic->def,
                      nir_undef(builder, intrinsic->def.num_components, intrinsic->def.bit_size));
   }
}

static bool
terakan_nir_lower_bindings_instr_image_size(nir_builder * const builder,
                                            nir_intrinsic_instr * const intrinsic,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrinsic->intrinsic == nir_intrinsic_bindless_image_size);

   bool const include_helpers = (nir_intrinsic_access(intrinsic) & ACCESS_INCLUDE_HELPERS) != 0;

   enum glsl_sampler_dim const dimensionality = nir_intrinsic_image_dim(intrinsic);

   if (dimensionality == GLSL_SAMPLER_DIM_BUF) {
      struct terakan_nir_binding const mutable_srv_binding =
         terakan_nir_get_image_mutable_srv_binding(builder, intrinsic, state->get_binding_functions,
                                                   state->get_binding_functions_data);
      if (unlikely(mutable_srv_binding.array_size == 0)) {
         terakan_nir_lower_bindings_instr_to_null(&intrinsic->instr);
         /* Return true because `terakan_nir_get_image_mutable_srv_binding` might have added
          * addressing instructions.
          */
         return true;
      }
      nir_def_replace(
         &intrinsic->def,
         terakan_nir_load_resinfo_constants_cube_elements_or_buffer_size(
            builder, mutable_srv_binding.base,
            nir_mov_scalar(builder, mutable_srv_binding.array_index_opt), include_helpers, state));
      return true;
   }

   return false;
}

static bool
terakan_nir_lower_bindings_instr(nir_builder * const builder, nir_instr * const instr,
                                 void * const state_ptr)
{
   struct terakan_nir_lower_bindings_state * const state = state_ptr;
   builder->cursor = nir_before_instr(instr);
   if (instr->type == nir_instr_type_tex) {
      return terakan_nir_lower_bindings_instr_tex(builder, nir_instr_as_tex(instr), state);
   } else if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
      switch (intrinsic->intrinsic) {
      case nir_intrinsic_load_constant:
         terakan_nir_lower_bindings_instr_load_constant(builder, intrinsic, state);
         return true;
      case nir_intrinsic_load_push_constant:
         terakan_nir_lower_bindings_instr_load_push_constant(builder, intrinsic, state);
         return true;
      case nir_intrinsic_load_ubo:
         terakan_nir_lower_bindings_instr_load_ubo(builder, intrinsic, state);
         return true;
      case nir_intrinsic_load_ssbo:
         terakan_nir_lower_bindings_instr_load_ssbo(builder, intrinsic, state);
         return true;
      case nir_intrinsic_store_ssbo:
         terakan_nir_lower_bindings_instr_store_ssbo(builder, intrinsic, state);
         return true;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         terakan_nir_lower_bindings_instr_ssbo_atomic(builder, intrinsic, state);
         return true;
      case nir_intrinsic_get_ssbo_size:
         terakan_nir_lower_bindings_instr_get_ssbo_size(builder, intrinsic, state);
         return true;
      case nir_intrinsic_bindless_image_load:
         terakan_nir_lower_bindings_instr_image_load(builder, intrinsic, state);
         return true;
      case nir_intrinsic_bindless_image_store:
         terakan_nir_lower_bindings_instr_image_store(builder, intrinsic, state);
         return true;
      case nir_intrinsic_bindless_image_atomic:
      case nir_intrinsic_bindless_image_atomic_swap:
         terakan_nir_lower_bindings_instr_image_atomic(builder, intrinsic, state);
         return true;
      case nir_intrinsic_bindless_image_size:
         return terakan_nir_lower_bindings_instr_image_size(builder, intrinsic, state);
      default:
         break;
      }
   }
   return false;
}

bool
terakan_nir_lower_bindings(
   nir_shader * const nir,
   struct terakan_nir_get_pipeline_layout_binding_functions const * const get_binding_functions,
   void const * const get_binding_functions_data, bool const robust_buffer_access_2,
   struct terakan_physical_device const * const physical_device,
   struct terakan_resinfo_constants_usage const * const resinfo_constants_usage,
   struct terakan_nir_shader_uav_usage const * const uav_usage_opt,
   struct terakan_shader_sqk_usage * const sqk_usage_accum,
   uint32_t * const driver_push_constants_used_accum)
{
   struct terakan_nir_lower_bindings_state state = {
      .get_binding_functions = get_binding_functions,
      .get_binding_functions_data = get_binding_functions_data,

      .robust_buffer_access_2 = robust_buffer_access_2,

      .physical_device = physical_device,

      .resinfo_constants_usage = resinfo_constants_usage,

      .sqk_usage_accum = sqk_usage_accum,
      .driver_push_constants_used_accum = driver_push_constants_used_accum,
   };

   if (uav_usage_opt != NULL) {
      state.uav_usage = *uav_usage_opt;
   }

   bool const progress = nir_shader_instructions_pass(nir, terakan_nir_lower_bindings_instr,
                                                      nir_metadata_none, &state);

   return progress;
}
