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

#ifndef TERAKAN_RESINFO_CONSTANTS_H
#define TERAKAN_RESINFO_CONSTANTS_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"

#include "compiler/shader_enums.h"
#include "util/bitset.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_resinfo_constants_usage {
   /* Constant dword indices are counts of bits in `cube_elements_or_buffer_size_for_mutable_srvs`
    * set prior to the bit for the given index of the SRV in the mutable SRV range.
    */
   BITSET_DECLARE(cube_elements_or_buffer_size_for_mutable_srvs,
                  TERAKAN_SRV_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL_OR_PIXEL);
};

struct terakan_resinfo_constants_state_stage {
   struct terakan_resinfo_constants_usage usage;

   uint8_t dwords_written;

   struct terakan_bo const * bo;
   uint64_t va;
};

struct terakan_resinfo_constants_state {
   uint8_t stages_using_resinfo_constants_;

   /* The fields for a given stage are ignored when applying the state if the bit for the stage is
    * not set in `stages_using_resinfo_constants_`.
    */

   uint8_t up_to_date_bound_to_stages_;

   struct terakan_resinfo_constants_state_stage stages_[MESA_SHADER_COMPUTE + 1];

   uint32_t written_constants_[MESA_SHADER_COMPUTE + 1]
                              [TERAKAN_SRV_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL_OR_PIXEL];
};

/* Pass a NULL usage to disable the resource info constants for the stage completely. */
static inline void
terakan_resinfo_constants_set_usage(struct terakan_resinfo_constants_state * const state,
                                    mesa_shader_stage const stage_index,
                                    struct terakan_resinfo_constants_usage const * const usage)
{
   assert((size_t)stage_index < ARRAY_SIZE(state->stages_));
   uint8_t const stage_bit = BITFIELD_BIT(stage_index);
   if (usage == NULL) {
      state->stages_using_resinfo_constants_ &= ~stage_bit;
      return;
   }
   state->stages_using_resinfo_constants_ |= stage_bit;
   state->stages_[stage_index].usage = *usage;
}

static inline void
terakan_resinfo_constants_set_unbound_to_stage(struct terakan_resinfo_constants_state * const state,
                                               mesa_shader_stage const stage)
{
   state->up_to_date_bound_to_stages_ &= ~BITFIELD_BIT(stage);
}

struct terakan_gfx_command_writer;

void terakan_resinfo_constants_apply(struct terakan_gfx_command_writer * command_writer,
                                     bool is_compute);

void terakan_resinfo_constants_reset(struct terakan_resinfo_constants_state * state);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_RESINFO_CONSTANTS_H */
