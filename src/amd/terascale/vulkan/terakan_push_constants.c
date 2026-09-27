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

#include "terakan_push_constants.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"

#include "compiler/shader_enums.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

void
terakan_push_constants_set_app_constants(struct terakan_push_constants_state * const state,
                                         uint32_t offset_bytes, uint32_t size_bytes,
                                         void const * const values)
{
   /* #MemoryIntegrity. */
   offset_bytes = MIN2(offset_bytes, (uint32_t)TERAKAN_PUSH_CONSTANTS_APP_SIZE_BYTES);
   size_bytes = MIN2(size_bytes, (uint32_t)TERAKAN_PUSH_CONSTANTS_APP_SIZE_BYTES - offset_bytes);

   if (state->allocation_.app_up_to_date_bytes_written > offset_bytes) {
      state->allocation_.mapping_if_up_to_date = NULL;
   }

   memcpy(state->app_constants_ + offset_bytes, values, size_bytes);
}

void
terakan_push_constants_apply(struct terakan_gfx_command_writer * const command_writer,
                             bool const is_compute)
{
   struct terakan_push_constants_state * const state = &command_writer->push_constants_state;

   struct terakan_push_constants_usage const usage =
      is_compute ? state->usage_compute_
                 : terakan_push_constants_usage_union(state->usage_pre_rasterization_,
                                                      state->usage_fragment_);
   if (terakan_push_constants_usage_empty(usage)) {
      return;
   }
   uint32_t const bytes_used = TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES + usage.app_extent_bytes;
   uint32_t const kcache_lines_used = DIV_ROUND_UP(bytes_used, TERAKAN_KCACHE_HW_LINE_BYTES);

   if ((state->driver_constants_modified_ & usage.driver_constants) ||
       state->allocation_.size_kcache_lines < kcache_lines_used) {
      state->allocation_.mapping_if_up_to_date = NULL;
   }

   if (state->allocation_.mapping_if_up_to_date == NULL) {
      state->allocation_.mapping_if_up_to_date = terakan_push_buffer_allocate_kcache(
         command_writer->base.command_buffer, TERAKAN_KCACHE_HW_LINE_BYTES * kcache_lines_used,
         &state->allocation_.bo, &state->allocation_.va_kcache_lines);
      if (unlikely(state->allocation_.mapping_if_up_to_date == NULL)) {
         return;
      }
      state->allocation_.size_kcache_lines = kcache_lines_used;
      state->allocation_.app_up_to_date_bytes_written = 0;

      state->up_to_date_push_constants_bound_to_stages_ = 0;

      memcpy(state->allocation_.mapping_if_up_to_date, &state->driver_constants_,
             sizeof(state->driver_constants_));
      state->driver_constants_modified_ = 0;
   }

   /* If merely expanding the used range, but not changing the push constant data itself (with the
    * same Vulkan pipeline layout, binding a shader stage that previously wasn't used, for example),
    * it's possible to append the newly used constants to the existing allocation without creating a
    * new one.
    */
   if (usage.app_extent_bytes > state->allocation_.app_up_to_date_bytes_written) {
      memcpy(state->allocation_.mapping_if_up_to_date + TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES +
                state->allocation_.app_up_to_date_bytes_written,
             state->app_constants_ + state->allocation_.app_up_to_date_bytes_written,
             usage.app_extent_bytes - state->allocation_.app_up_to_date_bytes_written);
      state->allocation_.app_up_to_date_bytes_written = usage.app_extent_bytes;
   }

   uint8_t const bind_to_stages = (is_compute ? (uint8_t)1 << MESA_SHADER_COMPUTE
                                              : state->graphics_stages_using_push_constants_) &
                                  ~state->up_to_date_push_constants_bound_to_stages_;
   if (bind_to_stages) {
      struct terakan_srv_descriptor const srv = {
         .srv = {
            [0] = state->allocation_.va_kcache_lines << TERAKAN_KCACHE_HW_LINE_BYTES_LOG2,
            [1] = ((uint32_t)state->allocation_.size_kcache_lines
                   << TERAKAN_KCACHE_HW_LINE_BYTES_LOG2) -
                  1u,
            [2] = S_030008_BASE_ADDRESS_HI(state->allocation_.va_kcache_lines >>
                                           (32 - TERAKAN_KCACHE_HW_LINE_BYTES_LOG2)) |
                  S_030008_STRIDE(1),
            [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
            [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
            [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER,
         }};
      u_foreach_bit (stage_index, bind_to_stages) {
         struct terakan_hw_config_sqk_set_functions const * const sqk_set_functions =
            &terakan_hw_config_sqk_stage_set_functions[stage_index];
         sqk_set_functions->kcache(&command_writer->hw_config_sqk,
                                   TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
                                   state->allocation_.size_kcache_lines, state->allocation_.bo,
                                   state->allocation_.va_kcache_lines);
         sqk_set_functions->srv(&command_writer->hw_config_sqk, TERAKAN_SRV_RANGE_PUSH_CONSTANTS,
                                state->allocation_.bo, &srv);
      }
      state->up_to_date_push_constants_bound_to_stages_ |= bind_to_stages;
   }
}

void
terakan_push_constants_reset(struct terakan_push_constants_state * const state)
{
   state->allocation_.mapping_if_up_to_date = NULL;

   state->graphics_stages_using_push_constants_ = 0b0;
   state->usage_pre_rasterization_ = (struct terakan_push_constants_usage){};
   state->usage_fragment_ = (struct terakan_push_constants_usage){};
   state->usage_compute_ = (struct terakan_push_constants_usage){};

   state->up_to_date_push_constants_bound_to_stages_ = 0b0;

   state->driver_constants_modified_ = BITFIELD_MASK(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_COUNT);

   /* For more stable comparisons. */
   memset(&state->driver_constants_, 0, sizeof(state->driver_constants_));
}
