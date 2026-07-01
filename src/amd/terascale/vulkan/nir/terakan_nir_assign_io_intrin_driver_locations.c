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
#include "terakan_shader.h"

#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Note that all metadata can be preserved if the pass only removes instructions, so location
 * assignment and sanitization can be done in a single pass for outputs.
 */

/* Vertex shader input.
 *
 * Currently there's no compaction (via semantic fetch or any other method) of application attribute
 * indices to vertex shader GPR indices.
 */

static bool
terakan_nir_assign_vs_input_load_driver_location(UNUSED nir_builder * const builder,
                                                 nir_intrinsic_instr * const intrinsic,
                                                 void * const sanitize_ptr)
{
   if (intrinsic->intrinsic != nir_intrinsic_load_input) {
      return false;
   }
   bool * const sanitize = (bool *)sanitize_ptr;
   unsigned const location = nir_intrinsic_io_semantics(intrinsic).location;
   if (location >= VERT_ATTRIB_GENERIC0) {
      unsigned const attribute_index = location - VERT_ATTRIB_GENERIC0;
      /* TODO(Triang3l): Remove any limiting to `VERT_ATTRIB_GENERIC15` in the compilation process.
       */
      if (unlikely(attribute_index >= TERAKAN_RESOURCE_HW_COUNT_FETCH)) {
         *sanitize = true;
      } else {
         nir_intrinsic_set_base(intrinsic, attribute_index);
         return true;
      }
   }
   return false;
}

static bool
terakan_nir_sanitize_vs_input_load_location(nir_builder * const builder,
                                            nir_intrinsic_instr * const intrinsic,
                                            UNUSED void * const callback_data)
{
   if (intrinsic->intrinsic != nir_intrinsic_load_input) {
      return false;
   }
   unsigned const location = nir_intrinsic_io_semantics(intrinsic).location;
   if (location >= VERT_ATTRIB_GENERIC0) {
      unsigned const attribute_index = location - VERT_ATTRIB_GENERIC0;
      if (attribute_index >= TERAKAN_RESOURCE_HW_COUNT_FETCH) {
         builder->cursor = nir_instr_remove(&intrinsic->instr);
         /* Replace an out-of-bounds attribute with zero, similarly to an unbound attribute. */
         nir_def_replace(&intrinsic->def, nir_imm_zero(builder, intrinsic->def.num_components,
                                                       intrinsic->def.bit_size));
         return true;
      }
   }
   return false;
}

bool
terakan_nir_assign_vs_input_load_driver_locations_and_sanitize(nir_shader * const nir)
{
   assert(nir->info.stage == MESA_SHADER_VERTEX);
   bool sanitize = false;
   bool progress = nir_shader_intrinsics_pass(nir, terakan_nir_assign_vs_input_load_driver_location,
                                              nir_metadata_all, &sanitize);
   if (unlikely(sanitize)) {
      progress |= nir_shader_intrinsics_pass(nir, terakan_nir_sanitize_vs_input_load_location,
                                             nir_metadata_control_flow, NULL);
   }
   return progress;
}

/* Fragment shader input system values. */

/* TODO(Triang3l): Merge all integer system values passed from the last pre-rasterization stage into
 * one interpolator parameter.
 */

static bool
terakan_nir_is_gpr_fs_input_varying_r89xx(gl_varying_slot const varying_slot)
{
   return varying_slot == VARYING_SLOT_POS || varying_slot == VARYING_SLOT_FACE;
}

struct terakan_nir_assign_fs_layer_view_input_load_driver_locations_state {
   uint32_t free_driver_locations;

   bool layer_used;
   bool view_index_used;

   uint8_t layer_driver_location;
   uint8_t view_index_driver_location;
};

static bool
terakan_nir_assign_fs_layer_view_input_load_driver_locations_gather_usage_instr(
   UNUSED nir_builder * const builder, nir_intrinsic_instr * const intrinsic,
   void * const state_ptr)
{
   if (intrinsic->intrinsic != nir_intrinsic_load_input &&
       intrinsic->intrinsic != nir_intrinsic_load_interpolated_input) {
      return false;
   }

   struct terakan_nir_assign_fs_layer_view_input_load_driver_locations_state * const state =
      state_ptr;

   nir_io_semantics const semantics = nir_intrinsic_io_semantics(intrinsic);

   gl_varying_slot const varying_slot = (gl_varying_slot)semantics.location;
   if (varying_slot == VARYING_SLOT_LAYER) {
      state->layer_used = true;
      return false;
   }
   if (varying_slot == VARYING_SLOT_VIEW_INDEX) {
      state->view_index_used = true;
      return false;
   }
   if (terakan_nir_is_gpr_fs_input_varying_r89xx(varying_slot)) {
      return false;
   }

   unsigned const base = nir_intrinsic_base(intrinsic);
   if (unlikely(base > 31u)) {
      return false;
   }
   unsigned const num_slots = MIN2(semantics.num_slots, 32u - base);
   state->free_driver_locations &= ~BITFIELD_RANGE(base, num_slots);

   return false;
}

static bool
terakan_nir_assign_fs_layer_view_input_load_driver_locations_instr(
   UNUSED nir_builder * const builder, nir_intrinsic_instr * const intrinsic,
   void * const state_ptr)
{
   if (intrinsic->intrinsic != nir_intrinsic_load_input &&
       intrinsic->intrinsic != nir_intrinsic_load_interpolated_input) {
      return false;
   }

   struct terakan_nir_assign_fs_layer_view_input_load_driver_locations_state * const state =
      state_ptr;

   gl_varying_slot const varying_slot =
      (gl_varying_slot)nir_intrinsic_io_semantics(intrinsic).location;

   if (varying_slot == VARYING_SLOT_LAYER) {
      assert(state->layer_used);
      nir_intrinsic_set_base(intrinsic, state->layer_driver_location);
      return true;
   }

   if (varying_slot == VARYING_SLOT_VIEW_INDEX) {
      assert(state->view_index_used);
      nir_intrinsic_set_base(intrinsic, state->view_index_driver_location);
      return true;
   }

   return false;
}

bool
terakan_nir_assign_fs_layer_view_input_load_driver_locations(nir_shader * const nir)
{
   bool progress = false;

   struct terakan_nir_assign_fs_layer_view_input_load_driver_locations_state state = {
      .free_driver_locations = BITFIELD_MASK(32),
   };

   progress |= nir_shader_intrinsics_pass(
      nir, terakan_nir_assign_fs_layer_view_input_load_driver_locations_gather_usage_instr,
      nir_metadata_all, &state);

   if (state.layer_used || state.view_index_used) {
      if (state.layer_used && likely(state.free_driver_locations != 0)) {
         state.layer_driver_location = (uint8_t)(ffs(state.free_driver_locations) - 1);
         state.free_driver_locations &= ~BITFIELD_BIT(state.layer_driver_location);
      }
      if (state.view_index_used && likely(state.free_driver_locations != 0)) {
         state.view_index_driver_location = (uint8_t)(ffs(state.free_driver_locations) - 1);
         state.free_driver_locations &= ~BITFIELD_BIT(state.view_index_driver_location);
      }

      progress |= nir_shader_intrinsics_pass(
         nir, terakan_nir_assign_fs_layer_view_input_load_driver_locations_instr, nir_metadata_all,
         &state);
   }

   return progress;
}

/* Fragment shader output.
 *
 * Compacted, and the dual source blending factor is exported into MRT 1.
 * See the description of `terakan_shader_rtv_mapping` for more details.
 *
 * To reduce implicit dependencies, the lowering is designed to work irrespective of whether
 * `nir_io_use_frag_result_dual_src_blend` is enabled.
 */

static bool
terakan_nir_gather_rtv_mapping_from_fs_output_store(UNUSED nir_builder * const builder,
                                                    nir_intrinsic_instr * const intrinsic,
                                                    void * const rtv_mapping_ptr)
{
   if (intrinsic->intrinsic != nir_intrinsic_store_output) {
      return false;
   }
   struct terakan_shader_rtv_mapping * const rtv_mapping =
      (struct terakan_shader_rtv_mapping *)rtv_mapping_ptr;
   nir_io_semantics const semantics = nir_intrinsic_io_semantics(intrinsic);
   if (semantics.location == FRAG_RESULT_DUAL_SRC_BLEND) {
      rtv_mapping->dual_source_blend_via_location_0_index_1 = true;
   } else if (semantics.location >= FRAG_RESULT_DATA0) {
      assert(semantics.num_slots == 1);
      unsigned const color_attachment_index = semantics.location - FRAG_RESULT_DATA0;
      if (semantics.dual_source_blend_index) {
         /* TODO(Triang3l): Possibly allow (including in SFN) exporting to other (unbound) RTVs if
          * the dual-source blending source output is written, if allowed by the hardware, to
          * implement the literal description of the limit from the Vulkan 1.4.360 specification:
          *     "`maxFragmentDualSrcAttachments` is the maximum number of output attachments which
          *     can be written to by the fragment shader stage when blending is enabled and one of
          *     the dual source blend modes is in use."
          * The collision between the `location = 1` and the `location = 0, index = 1` outputs may
          * be resolved with conditional stores in the shader based on a toggle from a driver
          * constant.
          */
         if (color_attachment_index == 0) {
            rtv_mapping->dual_source_blend_via_location_0_index_1 = true;
         }
      } else {
         if (color_attachment_index < TERAKAN_COLOR_HW_RTV_COUNT) {
            rtv_mapping->rtv_uncompacted_exports |= BITFIELD_BIT(color_attachment_index);
         }
      }
   }
   return false;
}

static bool
terakan_nir_assign_vs_input_load_driver_location_and_sanitize(UNUSED nir_builder * const builder,
                                                              nir_intrinsic_instr * const intrinsic,
                                                              void * const rtv_mapping_ptr)
{
   if (intrinsic->intrinsic != nir_intrinsic_store_output) {
      return false;
   }
   struct terakan_shader_rtv_mapping const * const rtv_mapping =
      (struct terakan_shader_rtv_mapping const *)rtv_mapping_ptr;
   nir_io_semantics const semantics = nir_intrinsic_io_semantics(intrinsic);
   if (semantics.location == FRAG_RESULT_DUAL_SRC_BLEND) {
      assert(semantics.num_slots == 1);
      if (unlikely(!rtv_mapping->dual_source_blend_via_location_0_index_1)) {
         nir_instr_remove(&intrinsic->instr);
         return true;
      }
      nir_intrinsic_set_base(intrinsic, 1);
      return true;
   } else if (semantics.location >= FRAG_RESULT_DATA0) {
      assert(semantics.num_slots == 1);
      unsigned const color_attachment_index = semantics.location - FRAG_RESULT_DATA0;
      if (semantics.dual_source_blend_index) {
         if (unlikely(!rtv_mapping->dual_source_blend_via_location_0_index_1 ||
                      color_attachment_index != 0)) {
            nir_instr_remove(&intrinsic->instr);
            return true;
         }
         nir_intrinsic_set_base(intrinsic, 1);
         return true;
      } else {
         unsigned const color_attachment_bit = BITFIELD_BIT(color_attachment_index);
         if (unlikely(color_attachment_index >= TERAKAN_COLOR_HW_RTV_COUNT ||
                      !(rtv_mapping->rtv_uncompacted_exports & color_attachment_bit))) {
            nir_instr_remove(&intrinsic->instr);
            return true;
         }
         nir_intrinsic_set_base(intrinsic, util_bitcount(rtv_mapping->rtv_uncompacted_exports &
                                                         (color_attachment_bit - 1u)));
         return true;
      }
   }
   return false;
}

bool
terakan_nir_assign_fs_output_store_driver_locations_and_sanitize(
   nir_shader * const nir, struct terakan_shader_rtv_mapping * const rtv_mapping_out)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   bool progress = false;

   /* Gather the RTV mapping. */
   *rtv_mapping_out = (struct terakan_shader_rtv_mapping){};
   progress |= nir_shader_intrinsics_pass(nir, terakan_nir_gather_rtv_mapping_from_fs_output_store,
                                          nir_metadata_all, rtv_mapping_out);

   /* Sanitize the RTV mapping. */
   if (rtv_mapping_out->dual_source_blend_via_location_0_index_1) {
      /* SFN may have special paths handling the dual-source blending factor output being present.
       */
      if (likely(rtv_mapping_out->rtv_uncompacted_exports & BITFIELD_BIT(0))) {
         /* Don't write to RTVs other than the first one with dual-source blending. */
         rtv_mapping_out->rtv_uncompacted_exports = BITFIELD_BIT(0);
      } else {
         rtv_mapping_out->dual_source_blend_via_location_0_index_1 = false;
      }
   }

   /* Assign the driver locations and sanitize the store instructions. */
   progress |=
      nir_shader_intrinsics_pass(nir, terakan_nir_assign_vs_input_load_driver_location_and_sanitize,
                                 nir_metadata_all, rtv_mapping_out);

   return progress;
}
