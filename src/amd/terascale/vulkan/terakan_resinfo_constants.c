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

#include "terakan_resinfo_constants.h"

#include "terakan_command_buffer.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void
terakan_resinfo_constants_apply(struct terakan_gfx_command_writer * const command_writer,
                                bool const is_compute)
{
   struct terakan_resinfo_constants_state * const state = &command_writer->resinfo_constants_state;

   u_foreach_bit (stage_index, state->stages_using_resinfo_constants_ &
                                  (is_compute ? BITFIELD_BIT(MESA_SHADER_COMPUTE)
                                              : BITFIELD_MASK(MESA_SHADER_FRAGMENT + 1))) {
      struct terakan_resinfo_constants_state_stage * const stage = &state->stages_[stage_index];

      uint32_t constants[TERAKAN_SRV_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL_OR_PIXEL];
      uint8_t constant_count = 0;

      uint8_t const stage_mutable_srv_count =
         terakan_srv_range_mutable_max_count((mesa_shader_stage)stage_index);
      unsigned mutable_srv_index;
      BITSET_FOREACH_SET (mutable_srv_index,
                          stage->usage.cube_elements_or_buffer_size_for_mutable_srvs,
                          stage_mutable_srv_count) {
         /* Section "SPIR-V Image Query Instructions" of the Vulkan 1.4.365 specification says:
          *
          *     "If the descriptor bound to the image is a null descriptor, all values returned by
          *     the above queries are replaced with 0."
          */
         uint32_t constant = 0;
         struct terakan_srv_descriptor const * const srv_descriptor =
            terakan_hw_config_sqk_get_bound_srv_descriptor(
               &command_writer->hw_config_sqk, (mesa_shader_stage)stage_index,
               TERAKAN_SRV_RANGE_MUTABLE_BASE + mutable_srv_index);
         if (srv_descriptor != NULL) {
            if (G_03001C_TYPE(srv_descriptor->srv[7]) == V_03001C_SQ_TEX_VTX_VALID_BUFFER) {
               constant = srv_descriptor->srv[4];
            } else {
               if (G_030000_DIM(srv_descriptor->srv[0]) == V_030000_SQ_TEX_DIM_CUBEMAP) {
                  constant = (G_030014_LAST_ARRAY(srv_descriptor->srv[5]) -
                              G_030014_BASE_ARRAY(srv_descriptor->srv[5]) + 1u) /
                             6u;
               }
            }
         }
         constants[constant_count++] = constant;
      }

      uint8_t const stage_bit = BITFIELD_BIT(stage_index);

      terakan_hw_config_sqk_set_srv_function const set_srv =
         terakan_hw_config_sqk_stage_set_functions[stage_index].srv;

      if (unlikely(constant_count == 0)) {
         if (!(state->up_to_date_bound_to_stages_ & stage_bit)) {
            state->up_to_date_bound_to_stages_ |= stage_bit;
            set_srv(&command_writer->hw_config_sqk, TERAKAN_SRV_RANGE_RESINFO_CONSTANTS, NULL,
                    NULL);
         }
         continue;
      }

      uint16_t const constants_size_bytes = (uint16_t)sizeof(uint32_t) * constant_count;
      uint32_t * const stage_written_constants = state->written_constants_[stage_index];
      if (stage->bo != NULL &&
          (constant_count > stage->dwords_written ||
           memcmp(stage_written_constants, constants, constants_size_bytes) != 0)) {
         stage->bo = NULL;
      }

      if (stage->bo == NULL) {
         void * const constants_mapping =
            terakan_push_buffer_allocate(command_writer->base.command_buffer, constants_size_bytes,
                                         sizeof(uint32_t), &stage->bo, &stage->va);
         if (likely(constants_mapping != NULL)) {
            memcpy(constants_mapping, constants, constants_size_bytes);
            memcpy(stage_written_constants, constants, constants_size_bytes);
            stage->dwords_written = constant_count;
            state->up_to_date_bound_to_stages_ &= ~stage_bit;
         }
      }

      if (!(state->up_to_date_bound_to_stages_ & stage_bit)) {
         state->up_to_date_bound_to_stages_ |= stage_bit;
         /* Initialized in the loop because resource info constant usage is expected to be rare in
          * general.
          */
         struct terakan_srv_descriptor const constants_srv = {
            .srv = {
               [0] = (uint32_t)stage->va,
               [1] = constants_size_bytes - 1u,
               [2] = S_030008_BASE_ADDRESS_HI(stage->va >> 32) | S_030008_STRIDE(sizeof(uint32_t)),
               [3] = TERAKAN_SRV_BUFFER_WORD3_DST_SEL_IDENTITY,
               [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
               [TERAKAN_SRV_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER,
            }};
         set_srv(&command_writer->hw_config_sqk, TERAKAN_SRV_RANGE_RESINFO_CONSTANTS, stage->bo,
                 &constants_srv);
      }
   }
}

void
terakan_resinfo_constants_reset(struct terakan_resinfo_constants_state * const state)
{
   state->stages_using_resinfo_constants_ = 0b0;

   state->up_to_date_bound_to_stages_ = 0b0;

   memset(state->stages_, 0, sizeof(state->stages_));
}
