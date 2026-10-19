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

#include "terakan_shader.h"

#include "nir/terakan_nir.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_instance.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/nir/terascale_nir.h"
#include "amd/terascale/common/terascale_wddm.h"
#include "compiler/glsl_types.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/sfn/sfn_nir.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_tex.h"
#include "spirv/nir_spirv.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_nir.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct terakan_shader_ring const terakan_shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT] = {
   [TERAKAN_SHADER_RING_INDEX_LSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_LSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E10_SQ_LSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028830_SQ_LSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_HSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_HSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E18_SQ_HSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028834_SQ_HSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_ESTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_ESTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C50_SQ_ESTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028908_SQ_ESTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_GSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_GSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C58_SQ_GSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_02890C_SQ_GSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_VSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_VSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C60_SQ_VSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028910_SQ_VSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_PSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_PSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C68_SQ_PSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028914_SQ_PSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
};

/* New version. */

nir_shader *
terakan_nir_from_spirv_pre_link(struct terakan_device * const device, size_t const spirv_size_bytes,
                                uint32_t const * const spirv, mesa_shader_stage const stage,
                                char const * const entrypoint,
                                VkSpecializationInfo const * const specialization_info)
{
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   /* Get the initial, largely not backend-specific NIR.
    *
    * After this point, the shader will still use variable derefs for I/O, bindings, and even local
    * variables.
    *
    * However, importantly, the shader's structure will be different from the original SPIR-V in the
    * following ways:
    * - Functions will be inlined, and there will be one return from the entry point.
    * - Constant initializers of variables will be lowered to store instructions.
    * - Structure copying, and per-member I/O structures, will be split.
    * - Dead I/O variables will be coarsely removed (in a way mostly sufficient for knowing the
    *   output usage).
    * - `invariant` will be propagated.
    */

   /* TODO(Triang3l): All `vk_pipeline_shader_stage_to_nir` and `vk_shader_to_nir` functionality
    * (primarily `vk_set_subgroup_size` when relevant).
    */

   static struct spirv_to_nir_options const spirv_options = {
      .environment = NIR_SPIRV_VULKAN,

      /* TODO(Triang3l): `amd_gcn_shader` when 64-bit integers (for `TimeAMD`) are supported. */
      /* TODO(Triang3l): `amd_shader_explicit_vertex_parameter` when supported and tested. */

      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = TERAKAN_KCACHE_HW_LINE_BYTES,
      .min_ssbo_alignment = sizeof(uint32_t),
   };

   nir_shader * const nir =
      vk_spirv_to_nir(&device->vk, spirv, spirv_size_bytes, stage, entrypoint, specialization_info,
                      &spirv_options, &physical_device->nir_options, false, NULL);
   if (unlikely(nir == NULL)) {
      return NULL;
   }

   /*
    * Input and output lowering.
    *
    * In SPIR-V, input and output variables may be accessed arbitrarily, and their types may be
    * composite (mapped accordingly to the "Location and Component Assignment" section of the
    * "Shader Interfaces" chapter of the Vulkan specification).
    *
    * SFN instead expects load / store intrinsics each working with a 32-bit value vector with up to
    * 4 components, corresponding to a single `Location` - therefore, arrays (unless explicitly
    * supported by SFN), structures and matrices must be lowered to individual vectors.
    *
    * Specifically, SFN compiles those intrinsics into the following:
    * - Software vertex shader input: GPR read (arrays with indirect indexing are supported by SFN).
    * - User-defined tessellation patch data (after TeraScale-specific lowering): LDS access.
    * - Tessellation factors (after TeraScale-specific lowering): LDS access (for output, additional
    *   instructions reading the factors from the LDS and writing them using the GDS clause
    *   `TF_WRITE` operation are emitted by the lowering).
    * - Hardware export shader output and geometry shader I/O: ring buffer item memory access (for
    *   GS inputs specifically, arrays with indirect indexing are supported by SFN).
    * - Last pre-rasterization stage output: Normal export (no indirect indexing in the hardware).
    * - Fragment shader:
    *   - User-defined input:
    *     - R8xx/R9xx: Interpolation instructions (no indirect indexing in the hardware).
    *     - R6xx/R7xx: GPR read.
    *   - System value input: GPR read, potentially additional ALU instructions.
    *   - Output: Normal export (no indirect indexing in the hardware).
    */

   /* Lower system values that are never represented by system varyings. */
   NIR_PASS(_, nir, nir_lower_system_values);
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      /* May perform further lowerings of instructions produced by `nir_lower_system_values`. */
      static nir_lower_compute_system_values_options const lower_compute_system_values_options = {};
      NIR_PASS(_, nir, nir_lower_compute_system_values, &lower_compute_system_values_options);
   }

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      if (nir->info.stage == MESA_SHADER_FRAGMENT) {
         /* Point coordinates are interpolated, convert them to a varying before the rest of the
          * interpolated varying lowerings.
          */
         static nir_lower_sysvals_to_varyings_options const lower_sysvals_to_varyings_options = {
            .point_coord = true,
         };
         NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &lower_sysvals_to_varyings_options);
      }

      /* Assign driver locations, used by SFN to specify the hardware locations, to input and output
       * variables passed between shader stages (for vertex input and fragment shader output, the
       * assignment is different and is done separately).
       * This assignment must be done before `nir_lower_io_passes` because it sets the driver
       * locations for variables, not the bases for load / store intrinsics.
       * Linkage (`nir_opt_varyings`) may change the load / store intrinsic bases later via
       * `nir_recompute_io_bases`.
       */
      /* TODO(Triang3l): Possibly pack `VARYING_SLOT_LAYER` and `VARYING_SLOT_VIEWPORT` into a
       * single vec4 passed to the fragment shader, to reduce the vec4 padding that needs to be
       * taken into account in the component count limits.
       */
      /* TODO(Triang3l): When the fragment shader is unlinked, pass clip and cull distance `param`s
       * to the fragment shader (but not `pos`) as separate, because in fragment shaders, the
       * presence or absence of built-in variables doesn't affect interface matching, and if the
       * last pre-rasterization stage outputs both clip and cull distances, but the fragment shader
       * uses only cull distances without clip distances, the cull distances are read from the wrong
       * components.
       */
      if (nir->info.stage != MESA_SHADER_VERTEX) {
         nir_assign_io_var_locations(nir, nir_var_shader_in);
      }
      if (nir->info.stage != MESA_SHADER_FRAGMENT) {
         nir_assign_io_var_locations(nir, nir_var_shader_out);
      }

      /* Lower deref accesses to load / store intrinsics, including handling indirect indexing and
       * 64-bit inputs and outputs, additionally with the preferred optimizations.
       */
      NIR_PASS(_, nir, nir_opt_combine_stores, nir_var_shader_out);
      if (!nir->info.io_lowered) {
         nir_lower_io_passes(nir, false);
      }
   }

   /*
    * Lowerings of bindings, to the extent to which linkage still can optimize them.
    */

   /* Lower access to buffers via derefs to intrinsics expected by the further binding lowerings. */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);

   /* After lowering buffer accesses to explicit intrinsics, split them into scalar accesses, to be
    * vectorized later according to the backend access requirements and to the post-link demand.
    */
   NIR_PASS(_, nir, nir_lower_io_to_scalar,
            nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_mem_push_const, NULL, NULL);

   /* Prepare for optimizations that may involve dependency chains.
    *
    * Specifically currently, this is done for:
    * - Constant folding before `terascale_nir_lower_txb_to_txd` to eliminate zero LOD biases.
    * - `optimize_txd` in `nir_lower_tex`.
    *   - Doesn't need copy propagation by itself due to `nir_scalar_resolved` as of August 22,
    *     2026, but needs `nir_opt_cse` to make sure the texture coordinates and the derivatives are
    *     for the same value in the trivial cases where this optimization may be done, and doing
    *     copy propagation to prepare for common subexpression elimination.
    * - Everything that's done after this function, especially that may have effect on the pipeline
    *   configuration demand.
    *
    * `nir_lower_global_vars_to_local` is needed because `nir_lower_vars_to_ssa` lowers
    * `nir_var_function_temp`, not `nir_var_shader_temp` (and because functions are inlined, there's
    * only one function).
    */
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   {
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, nir, nir_opt_copy_prop);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_dce);
      } while (progress);
   };
   NIR_PASS(_, nir, nir_opt_cse);

   /* Lower texture instructions. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* TODO(Triang3l): Pass for lowering constant LOD biases to fixed-point in `backend_flags`
       * where the conversion is exact.
       */
      /* Lower LOD biases to derivative exponent biases when cube map coordinates are still in 3D.
       */
      NIR_PASS(_, nir, terascale_nir_lower_txb_to_txd);
   }
   static nir_lower_tex_options const lower_tex_options = {
      .lower_txf_offset = true,
      .lower_invalid_implicit_lod = true,
      .optimize_txd = true,
   };
   NIR_PASS(_, nir, nir_lower_tex, &lower_tex_options);
   NIR_PASS(_, nir, terascale_nir_lower_tex);

   /* Lower Vulkan descriptor intrinsics to the format expected by the further binding lowerings. */
   terakan_nir_lower_vulkan_resource_index_passes(nir);

   /*
    * Lowering of I/O instructions that may be added during binding lowering.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Take the reciprocal of the W of the position loaded by the SPI for `gl_FragCoord.w` after
       * the lowerings potentially loading `gl_FragCoord`.
       */
      /* TODO(Triang3l): Implement the system value load intrinsics directly in SFN, and enable
       * options like `GLSLFragCoordIsSysVal` in Gallium. This will also make it possible to replace
       * `nir_lower_fragcoord_wtrans`, which must not be called multiple times, with
       * `nir_frag_coord_use_w_rcp`.
       */
      NIR_PASS(_, nir, nir_lower_fragcoord_wtrans);
   }

   /*
    * Lowerings of ALU instructions that may not be inserted by further lowerings and optimizations,
    * from more complex to more primitive.
    */

   /* Floating-point. */

   NIR_PASS(_, nir, terascale_nir_lower_sin_cos_to_wrapped_normalized_r789xx);

   if (!nir->info.flrp_lowered) {
      NIR_PASS(_, nir, nir_lower_flrp, 16 | 32 | 64, false);
      nir->info.flrp_lowered = true;
   }

   /* Integer. */

   NIR_PASS(_, nir, nir_opt_idiv_const, 32);
   static nir_lower_idiv_options const lower_idiv_options = {};
   NIR_PASS(_, nir, nir_lower_idiv, &lower_idiv_options);

   NIR_PASS(_, nir, nir_lower_alu_width, NULL, NULL);
   /* Includes lowering of instruction produced by `nir_lower_alu_width`. */
   NIR_PASS(_, nir, nir_lower_pack);

   return nir;
}

static int
terakan_nir_type_size_io_vec4(glsl_type const * const type, UNUSED bool const bindless)
{
   return (int)glsl_count_attribute_slots(type, false);
}

static void
terakan_nir_type_size_align_scratch(glsl_type const * const type, unsigned * const size,
                                    unsigned * const alignment)
{
   /* TODO(Triang3l): More compact scratch allocation, taking the `MEM_SCRATCH` instruction encoding
    * into account.
    */
   *size = glsl_count_vec4_slots(type, false, false);
   *alignment = 1;
}

struct terakan_nir_lower_and_optimize_post_link_result
terakan_nir_lower_and_optimize_post_link(
   nir_shader * const nir,
   struct terakan_nir_lower_and_optimize_post_link_options const * const options)
{
   /* Validate the options. */

   assert(!(nir->info.stage == MESA_SHADER_VERTEX && options->next_stage != MESA_SHADER_TESS_CTRL &&
            options->next_stage != MESA_SHADER_GEOMETRY &&
            options->next_stage != MESA_SHADER_FRAGMENT));
   assert(
      !(nir->info.stage == MESA_SHADER_TESS_CTRL && options->next_stage != MESA_SHADER_TESS_EVAL));
   assert(!(nir->info.stage == MESA_SHADER_TESS_EVAL &&
            options->next_stage != MESA_SHADER_GEOMETRY &&
            options->next_stage != MESA_SHADER_FRAGMENT));
   assert(
      !(nir->info.stage == MESA_SHADER_GEOMETRY && options->next_stage != MESA_SHADER_FRAGMENT));
   assert(!(nir->info.stage == MESA_SHADER_FRAGMENT && options->next_stage != MESA_SHADER_NONE));
   assert(!(nir->info.stage == MESA_SHADER_COMPUTE && options->next_stage != MESA_SHADER_NONE));

   struct terakan_nir_lower_and_optimize_post_link_result result = {};

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      /*
       * Input and output lowering that may depend on linkage.
       */

      /* Make sure dead I/O code is eliminated before lowerings contributing to I/O demand. */
      /* TODO(Triang3l): Should be done if `nir_opt_varyings` doesn't progress probably, research
       * carefully.
       */
      NIR_PASS(_, nir, nir_opt_dce);

      /* Assign driver locations, used by SFN to specify the hardware locations, to I/O loads and
       * stores not covered by `nir_assign_io_var_locations`, after unused I/O accesses are
       * eliminated (as the assigned locations may be compacted).
       */
      if (nir->info.stage == MESA_SHADER_VERTEX) {
         NIR_PASS(_, nir, terakan_nir_assign_vs_input_load_driver_locations_and_sanitize);
      } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
         NIR_PASS(_, nir, terakan_nir_assign_fs_output_store_driver_locations_and_sanitize,
                  &result.rtv_mapping);
      }
   }

   /*
    * Lowerings of bindings and everything that contributes to the SQK usage.
    */

   /* Lower texture, sampler and image derefs to the format needed by `terakan_nir_lower_bindings`,
    * with a single flattened index, for which constant propagation can also be performed.
    */
   NIR_PASS(_, nir, terakan_nir_lower_tex_image_deref_to_vulkan_resource_index);

   /* Lower accesses to used constants that will be loaded from the immediate constant buffer to the
    * format needed by `terakan_nir_lower_bindings`.
    */
   /* TODO(Triang3l): Clean up dead copy elimination invocations, earlier DCE might have been done
    * already. This is currently placed here to specify that's it's demanded.
    */
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_large_constants, glsl_get_natural_size_align_bytes, 16);
   /* Lower immediate constant loads to scalar to vectorize later according to the backend access
    * requirements.
    */
   NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_mem_constant, NULL, NULL);
   /* TODO(Triang3l): Vectorize used scalar loads of immediate constants for optimization (need
    * `load_constant` support within `nir_opt_load_store_vectorize` itself also).
    */

   /* Propagate resource addressing constants.
    * A copy propagation loop requires dead code elimination. Here it also eliminates dead code that
    * may demand SQK.
    */
   {
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, nir, nir_opt_copy_prop);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_dce);
      } while (progress);
   };

   /* Finally lower bindings to backend instructions referencing SQK at hardware register indices.
    */
   bool use_get_buffer_resinfo = !options->physical_device->chip_info.is_r8xx_eg;
#ifdef TERAKAN_REGRESSION_TEST
   if (container_of(options->physical_device->vk.base.instance, struct terakan_instance const, vk)
          ->regression_test_flags &
       BITFIELD64_BIT(TERAKAN_REGRESSION_TEST_SHIFT_NO_GET_BUFFER_RESINFO)) {
      use_get_buffer_resinfo = false;
   }
#endif
   NIR_PASS(_, nir, terakan_nir_gather_resinfo_constants_usage, options->get_binding_functions,
            options->get_binding_functions_data, use_get_buffer_resinfo,
            &result.resinfo_constants_usage);
   struct terakan_nir_shader_uav_usage uav_usage = {};
   if (nir->info.stage == MESA_SHADER_FRAGMENT || nir->info.stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, terakan_nir_gather_uav_usage, options->get_binding_functions,
               options->get_binding_functions_data,
               nir->info.stage == MESA_SHADER_FRAGMENT
                  ? util_bitcount(
                       terakan_shader_rtv_mapping_rtv_dsb_uncompacted_exports(result.rtv_mapping))
                  : 0u,
               &uav_usage);
      result.uav_usage = uav_usage.usage;
   }
   NIR_PASS(_, nir, terakan_nir_lower_bindings, options->get_binding_functions,
            options->get_binding_functions_data, options->robust_buffer_access_2,
            options->physical_device, use_get_buffer_resinfo, &result.resinfo_constants_usage,
            &uav_usage, &result.sqk_usage, &result.driver_push_constants_used);

   /* TODO(Triang3l): `nir_opt_offsets` (implement for R600 intrinsics). */

   /*
    * I/O lowerings that need to be done after various binding lowerings, and that also potentially
    * benefit from optimizations like dead code elimination after binding lowerings.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Convert fragment shader system value inputs to explicitly loaded varyings where expected by
       * SFN (in part because prior to R8xx, GPRs [0, 31] were preloaded with both interpolated
       * varyings and all used system values, so they needed to share indexing).
       *
       * Various lowerings may produce system value load intrinsics, so this is done at the end.
       *
       * Point coordinates have already been lowered early because they're interpolated.
       */
      /* TODO(Triang3l): Implement the system value load intrinsics directly in SFN, and enable
       * options like `GLSLFragCoordIsSysVal` in Gallium.
       */
      static nir_lower_sysvals_to_varyings_options const lower_sysvals_to_varyings_options = {
         .frag_coord = true,
         .front_face = true,
         .layer_id = true,
         .view_index = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &lower_sysvals_to_varyings_options);
      /* Convert the newly created variable deref loads to explicit loads.
       * `nir_lower_io_use_interpolated_input_intrinsics` should not be used for them, as they're
       * either autogenerated or integers.
       */
      /* TODO(Triang3l): `nir_lower_io_vars_to_temporaries` interaction - would it be beneficient?
       */
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in, terakan_nir_type_size_io_vec4, 0);
      NIR_PASS(_, nir, terakan_nir_assign_fs_layer_view_input_load_driver_locations);
   }

   /*
    * General representation lowering and optimization.
    */

   /* Make non-reduction ALU operations scalar as expected by SFN and for less constrained
    * optimizations.
    */
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);

   while (r600_optimize_nir_once(nir)) {
   }

   /* Perform late algebraic optimizations and lowerings, including some of the lowerings of ALU
    * instructions not supported by SFN directly, such as `ineg`.
    */
   {
      bool late_algebraic_progress;
      do {
         late_algebraic_progress = false;
         NIR_PASS(late_algebraic_progress, nir, nir_opt_algebraic_late);
         NIR_PASS(_, nir, nir_opt_copy_prop);
         NIR_PASS(_, nir, nir_opt_constant_folding);
         NIR_PASS(_, nir, nir_opt_dce);
         NIR_PASS(_, nir, nir_opt_cse);
      } while (late_algebraic_progress);
   }

   /* Convert booleans to integers when nothing may insert comparisons or selections anymore. */
   NIR_PASS(_, nir, nir_lower_bool_to_int32);

   /* Place large temporary arrays in the scratch buffer. The scratch buffer has a large performance
    * and memory cost compared to registers, thus it should be avoided where possible, and there's
    * not much that can be done from the optimization perspective with scratch variables, so this is
    * done last.
    */
   NIR_PASS(_, nir, nir_lower_vars_to_scratch, 64, terakan_nir_type_size_align_scratch,
            terakan_nir_type_size_align_scratch);

   NIR_PASS(_, nir, nir_lower_locals_to_regs, 32);
   NIR_PASS(_, nir, nir_convert_from_ssa, true, false);

   /* Final cleanup. */
   NIR_PASS(_, nir, nir_opt_dce);

   return result;
}

void
terakan_shader_impl_apply_lower_and_optimize_post_link_result(
   struct terakan_shader_impl * const shader, union r600_shader_key * const shader_key,
   struct terakan_nir_lower_and_optimize_post_link_result const * const result,
   mesa_shader_stage const stage)
{
   shader->sqk_usage = result->sqk_usage;

   shader->push_constants_usage.driver_constants = result->driver_push_constants_used;

   shader->resinfo_constants_usage = result->resinfo_constants_usage;

   shader->uav_usage = result->uav_usage;

   if (stage == MESA_SHADER_FRAGMENT) {
      shader->fs.rtv_mapping = result->rtv_mapping;
      shader_key->ps.nr_cbufs = util_bitcount(
         terakan_shader_rtv_mapping_rtv_dsb_uncompacted_exports(shader->fs.rtv_mapping));
   }

   /* TODO(Triang3l): All shader key fields that can be obtained from NIR. */
}

void
terakan_shader_impl_bind_immediate_constants(struct terakan_hw_config_sqk * const hw_config_sqk,
                                             struct terakan_shader_impl const * const shader)
{
   if (!BITSET_TEST(shader->sqk_usage.srvs,
                    TERAKAN_SRV_RANGE_IMMEDIATE_CONSTANTS_OR_META_PRIMARY)) {
      return;
   }
   terakan_hw_config_sqk_set_srv_function const set_srv =
      terakan_hw_config_sqk_stage_set_functions[shader->sw_stage].srv;
   if (unlikely(shader->immediate_constants_bytes == 0)) {
      set_srv(hw_config_sqk, TERAKAN_SRV_RANGE_IMMEDIATE_CONSTANTS_OR_META_PRIMARY, NULL, NULL);
      return;
   }
   struct terakan_srv_descriptor const srv = {
      .srv = {
         [0] = (uint32_t)shader->immediate_constants_va,
         [1] = shader->immediate_constants_bytes - 1u,
         [2] = S_030008_BASE_ADDRESS_HI(shader->immediate_constants_va >> 32) | S_030008_STRIDE(1),
         [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
         [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
         [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER,
      }};
   set_srv(hw_config_sqk, TERAKAN_SRV_RANGE_IMMEDIATE_CONSTANTS_OR_META_PRIMARY,
           shader->static_state.program_bo, &srv);
}

void
terakan_shader_impl_finish(struct terakan_shader_impl * const shader)
{
   if (shader->shader.arrays != NULL) {
      free(shader->shader.arrays);
   }
}
