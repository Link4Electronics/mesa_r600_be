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

#ifndef TERAKAN_NIR_H
#define TERAKAN_NIR_H

#include "terakan_physical_device.h"
#include "terakan_shader.h"

#include "util/bitset.h"
#include "nir.h"
#include "nir_builder.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emits a buffer SRV load intrinsic for a buffer SRV with a stride of 1 producing a NIR vector.
 * Supports 1, 2 and 4 components for 8 and 16 bits, and 1, 2, 3 and 4 components for 32 bits.
 * Vectorization is expected to be done externally before lowering to this intrinsic if needed,
 * taking alignment and robustness requirements into account.
 */
nir_def * terakan_nir_load_raw_buffer_srv(nir_builder * builder, unsigned num_components,
                                          unsigned bit_size, enum gl_access_qualifier access,
                                          unsigned srv_index_base, nir_def * srv_index,
                                          unsigned byte_address_base, nir_def * byte_address);

/*
 * I/O driver location assignment, expecting I/O variable access lowered to load / store intrinsics.
 */

bool terakan_nir_assign_vs_input_load_driver_locations_and_sanitize(nir_shader * nir);

/* Assigns free driver locations to explicit `VARYING_SLOT_LAYER` and `VARYING_SLOT_VIEW_INDEX`
 * input loads. This lowering assumes R8xx+, where those system values are passed via interpolator
 * parameters loaded explicitly, while `VARYING_SLOT_POS` and `VARYING_SLOT_FACE` are preloaded to
 * GPRs.
 */
bool terakan_nir_assign_fs_layer_view_input_load_driver_locations(nir_shader * nir);

/* Output stores must be lowered so they write up to 4 components without indirect indexing. */
bool terakan_nir_assign_fs_output_store_driver_locations_and_sanitize(
   nir_shader * nir, struct terakan_shader_rtv_mapping * rtv_mapping_out);

/*
 * Lowering of client API buffer and image bindings to hardware indices or to hardware-specific
 * access intrinsics.
 */

/* Replaces `load_vulkan_descriptor`, which is not used because TeraScale accepts binding indices
 * rather than descriptor contents directly (instead `nir_chase_binding` is used for binding access
 * lowering in Terakan), with its `vulkan_resource_index` or `vulkan_resource_reindex` source. This
 * is needed to simplify other lowerings working with bindings.
 */
bool terakan_nir_lower_load_vulkan_descriptor_to_src(nir_shader * nir);

/* Replaces `vulkan_resource_reindex` chains with integer addition chains followed by
 * `vulkan_resource_index`, so the final index can be accessed via `nir_chase_binding`, and constant
 * propagation can be performed for the reindexings fully before that final `vulkan_resource_index`.
 */
bool terakan_nir_lower_vulkan_resource_reindex_to_index(nir_shader * nir);

/* Splits the `vulkan_resource_index` vector for buffers (which, after the SPIR-V to NIR conversion,
 * `nir_lower_explicit_io`, and the preceding Vulkan resource index lowerings, are expected to use
 * an address format with the Vulkan resource index for the buffer in the first component and the
 * base data address in the other components) into a scalar `vulkan_resource_index` (to be obtained
 * via `nir_chase_binding` in binding access lowerings) providing only the buffer index, and a zero
 * base address since buffer data addresses are relative in Terakan, so constant propagation may be
 * performed for the data address.
 */
bool terakan_nir_lower_vulkan_resource_index_offset_to_index_only(nir_shader * nir);

void terakan_nir_lower_vulkan_resource_index_passes(nir_shader * nir);

/* Lowers texture, sampler and image deref chains to single `vulkan_resource_index` instructions
 * (single-component, like from `terakan_nir_lower_vulkan_resource_index_passes`) to flatten the
 * indices into a single scalar, and so constant propagation can work for the index.
 *
 * `nir_tex_src_texture/sampler_deref` and `nir_tex_src_texture/sampler_offset` are lowered to
 * `nir_tex_src_texture/sampler_handle`.
 *
 * `image_deref_*` intrinsics supported by Terakan are lowered to `bindless_image_*`.
 */
bool terakan_nir_lower_tex_image_deref_to_vulkan_resource_index(nir_shader * nir);

#define TERAKAN_NIR_BINDING_BASE_INVALID UINT8_MAX

struct terakan_nir_pipeline_layout_binding {
   /* 0 if the binding is empty, doesn't exist, or invalid. */
   uint8_t array_size;

   /* `TERAKAN_NIR_BINDING_BASE_INVALID` if the binding doesn't use the given hardware register
    * range.
    * The bases are relative to the beginning of the hardware register range for the stage. In
    * particular, `srv_base` is not relative to `TERAKAN_SRV_RANGE_MUTABLE_BASE`, for example,
    * however, lowerings may expect that the SRV range of the binding is in the mutable SRV range.
    */
   uint8_t kcache_base;
   uint8_t sampler_base;
   uint8_t srv_base;
};

extern struct terakan_nir_pipeline_layout_binding const terakan_nir_pipeline_layout_binding_empty;

/* Note that `binding->var` may be unavailable depending on the binding type, so it shouldn't be
 * used by the implementations.
 */
typedef struct terakan_nir_pipeline_layout_binding (
   *terakan_nir_get_pipeline_layout_binding_function)(nir_binding const * binding,
                                                      mesa_shader_stage stage,
                                                      void const * callback_data);

/* Functions mapping client API bindings to hardware bindings. */
struct terakan_nir_get_pipeline_layout_binding_functions {
   /* TODO(Triang3l): Immutable sampler unnormalized coordinates information. */
   terakan_nir_get_pipeline_layout_binding_function sampler;
   terakan_nir_get_pipeline_layout_binding_function sampled_image;
   terakan_nir_get_pipeline_layout_binding_function storage_image;
   terakan_nir_get_pipeline_layout_binding_function storage_texel_buffer;
   terakan_nir_get_pipeline_layout_binding_function uniform_buffer;
   terakan_nir_get_pipeline_layout_binding_function storage_buffer;
};

struct terakan_nir_shader_uav_usage {
   struct terakan_shader_uav_usage usage;

   /* TODO(Triang3l): More precise analysis of which storage bindings can alias memory written by
    * the shader.
    */
   bool any_buffer_written;
   bool any_image_written;
};

/* Before calling `terakan_nir_gather_uav_usage` or `terakan_nir_lower_bindings`:
 *
 * - Before lowering Vulkan resource indices, buffer accesses must be lowered to explicit load /
 *   store intrinsics using `nir_lower_explicit_io`.
 *
 *   Data addresses within uniform and storage buffers and push constants must be 32-bit offsets.
 *
 *   `nir_opt_load_store_vectorize` may also be done, as this pass finally lowers buffer accesses to
 *   backend-specific intrinsics.
 *
 * - The binding source must be either `load_vulkan_descriptor(vulkan_resource_index)` or
 *   `vulkan_resource_index` itself.
 *
 *   `vulkan_resource_reindex` therefore must be lowered - no arithmetic on the result of
 *   `vulkan_resource_index` is permitted by this lowering, so `nir_chase_binding` can locate the
 *   binding.
 *
 *   `terakan_nir_lower_vulkan_resource_index_passes` performs the lowerings needed to fulfill the
 *   expectations of `nir_chase_binding` and to optimally propagate constants used in addressing.
 *
 *   Textures and samplers must use a `vulkan_resource_index` source as described above as
 *   `nir_tex_src_texture/sampler_handle`, without `nir_tex_src_texture/sampler_offset`. Images must
 *   be accessed via `bindless_image_*` also similarly accepting a `vulkan_resource_index` source.
 *   Use `terakan_nir_lower_tex_image_deref_to_vulkan_resource_index` to lower the variable derefs
 *   to the expected format.
 *
 * - Constants should be propagated, so indirect addressing can be avoided where possible.
 *
 * - Dead code with binding accesses should be eliminated, so bindings that ended up not being used
 *   are not configured for this shader in command buffers, as this function adds bindings to the
 *   SQK usage of the shader.
 *
 * `terakan_nir_gather_uav_usage` can't assume binding array index ranges narrower than
 * `terakan_nir_lower_bindings` does, otherwise the latter would potentially not be aware that some
 * binding arrays have UAVs, or UAV arrays would overlap other MRTs or even have negative base
 * indices. Because of that, `terakan_nir_gather_uav_usage` shares the binding indexing calculations
 * with `terakan_nir_lower_bindings`, and they may insert new instructions even though the pass is
 * designed to merely gather the information. However, they may be removed via dead code
 * elimination, or reused by common subexpression elimination.
 *
 * UAV indices in the intrinsics produced by `terakan_nir_lower_bindings` don't include the
 * `uav_cb_color_index_base` passed to `terakan_nir_gather_uav_usage`, rather the gathering only
 * uses it for bounds checking. It's expected to be set in `r600_shader::rat_base` and applied by
 * SFN.
 */
bool terakan_nir_gather_uav_usage(
   nir_shader * nir,
   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions,
   void const * get_binding_functions_data, unsigned uav_cb_color_index_base,
   struct terakan_nir_shader_uav_usage * uav_usage_out);
bool terakan_nir_lower_bindings(
   nir_shader * nir,
   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions,
   void const * get_binding_functions_data, bool robust_buffer_access_2,
   struct terakan_physical_device const * physical_device,
   struct terakan_nir_shader_uav_usage const * uav_usage_opt,
   struct terakan_shader_sqk_usage * sqk_usage_accum, uint32_t * driver_push_constants_used_accum);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_NIR_H */
