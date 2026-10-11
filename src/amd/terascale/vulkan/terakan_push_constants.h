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

#ifndef TERAKAN_PUSH_CONSTANTS_H
#define TERAKAN_PUSH_CONSTANTS_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"

#include "compiler/shader_enums.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum terakan_push_constants_driver_index {
   TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_ALIGNMENT_OFFSET,

   TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_DRAW_ID,

   TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_COUNT,
};

static_assert(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_COUNT <= 32,
              "Using driver push constant indices in a 32-bit bitfield.");

struct terakan_push_constants_driver {
   /* UAV indices in this array are zero-based, not offset by the RTV count, to avoid invalidating
    * if the RTV count is changed, but UAV bindings are not.
    * For storage buffers, in bytes.
    * For texel buffers, in elements.
    */
   uint32_t buffer_uav_alignment_offset[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];

   uint32_t draw_id;
};

/* Aligned to vec4 to avoid placing vectors in different kcache lines more likely to be accessed in
 * separate ALU clauses if they end up at the boundary, and so a pair of dwords at an even dword
 * address in application push constants is accessed via one read port on R8xx+ just like in uniform
 * buffers.
 */
#define TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES                                                      \
   ALIGN_POT(sizeof(struct terakan_push_constants_driver), sizeof(float) * 4)
/* VUID-vkCmdPushConstants-size-00369: "`size` must be a multiple of 4" */
#define TERAKAN_PUSH_CONSTANTS_APP_SIZE_BYTES                                                      \
   ((TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES - TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES) &            \
    ~(uint32_t)(4 - 1))

struct terakan_push_constants_usage {
   uint32_t driver_constants;
   uint32_t app_extent_bytes;
};

static inline bool
terakan_push_constants_usage_empty(struct terakan_push_constants_usage const usage)
{
   return !usage.driver_constants && usage.app_extent_bytes == 0;
}

static inline struct terakan_push_constants_usage
terakan_push_constants_usage_union(struct terakan_push_constants_usage const a,
                                   struct terakan_push_constants_usage const b)
{
   struct terakan_push_constants_usage usage;
   usage.driver_constants = a.driver_constants | b.driver_constants;
   usage.app_extent_bytes = MAX2(a.app_extent_bytes, b.app_extent_bytes);
   return usage;
}

struct terakan_push_constants_state {
   struct {
      /* NULL if not up to date, the rest is undefined if that's the case. */
      char * mapping_if_up_to_date;
      struct terakan_bo const * bo;
      uint32_t va_kcache_lines;
      uint32_t size_kcache_lines;
      uint32_t app_up_to_date_bytes_written;
   } allocation_;

   uint8_t up_to_date_push_constants_bound_to_stages_;

   uint8_t graphics_stages_using_push_constants_;
   struct terakan_push_constants_usage usage_pre_rasterization_;
   struct terakan_push_constants_usage usage_fragment_;
   struct terakan_push_constants_usage usage_compute_;

   uint32_t driver_constants_modified_;
   struct terakan_push_constants_driver driver_constants_;

   /* Aligned to `uint32_t` for more optimal `memcpy`.
    * In Vulkan, offsets and sizes are specified in bytes, but:
    * VUID-vkCmdPushConstants-offset-00368: "`offset` must be a multiple of 4"
    * VUID-vkCmdPushConstants-size-00369: "`size` must be a multiple of 4"
    */
   alignas(uint32_t) char app_constants_[TERAKAN_PUSH_CONSTANTS_APP_SIZE_BYTES];
};

/* Returns whether the constant was modified. */
static inline bool
terakan_push_constants_set_driver_u32_(struct terakan_push_constants_state * const state,
                                       enum terakan_push_constants_driver_index const index,
                                       uint32_t * const value_ptr, uint32_t const new_value)
{
   if (*value_ptr == new_value) {
      return false;
   }
   *value_ptr = new_value;
   state->driver_constants_modified_ |= BITFIELD_BIT(index);
   return true;
}

/* Returns whether the constant was modified. */
static inline bool
terakan_push_constants_set_driver_buffer_uav_alignment_offset(
   struct terakan_push_constants_state * const state, unsigned const uav_index,
   uint32_t const value)
{
   assert(uav_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);
   return terakan_push_constants_set_driver_u32_(
      state, TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_ALIGNMENT_OFFSET,
      &state->driver_constants_.buffer_uav_alignment_offset[uav_index], value);
}

void terakan_push_constants_set_app_constants(struct terakan_push_constants_state * state,
                                              uint32_t offset_bytes, uint32_t size_bytes,
                                              void const * values);

static inline void
terakan_push_constants_set_usage_pre_rasterization(struct terakan_push_constants_state * const state,
                                                   struct terakan_push_constants_usage const usage,
                                                   uint8_t const stages_using_push_constants)
{
   uint8_t const not_pre_rasterization_stages =
      (uint8_t) ~(BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_TESS_CTRL) |
                  BITFIELD_BIT(MESA_SHADER_TESS_EVAL) | BITFIELD_BIT(MESA_SHADER_GEOMETRY));
   assert(!(stages_using_push_constants & not_pre_rasterization_stages));
   state->usage_pre_rasterization_ = usage;
   state->graphics_stages_using_push_constants_ =
      (state->graphics_stages_using_push_constants_ & not_pre_rasterization_stages) |
      stages_using_push_constants;
}

static inline void
terakan_push_constants_set_usage_fragment(struct terakan_push_constants_state * const state,
                                          struct terakan_push_constants_usage const usage)
{
   state->usage_fragment_ = usage;
   if (!terakan_push_constants_usage_empty(usage)) {
      state->graphics_stages_using_push_constants_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   } else {
      state->graphics_stages_using_push_constants_ &= ~BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   }
}

static inline void
terakan_push_constants_set_usage_compute(struct terakan_push_constants_state * const state,
                                         struct terakan_push_constants_usage const usage)
{
   state->usage_compute_ = usage;
}

static inline void
terakan_push_constants_set_unbound_to_stage(struct terakan_push_constants_state * const state,
                                            mesa_shader_stage const stage)
{
   state->up_to_date_push_constants_bound_to_stages_ &= ~BITFIELD_BIT(stage);
}

struct terakan_gfx_command_writer;

/* `TERAKAN_SRV_RANGE_RESINFO_CONSTANTS` stride is one dword. */
void terakan_push_constants_apply(struct terakan_gfx_command_writer * command_writer,
                                  bool is_compute);

void terakan_push_constants_reset(struct terakan_push_constants_state * state);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PUSH_CONSTANTS_H */
