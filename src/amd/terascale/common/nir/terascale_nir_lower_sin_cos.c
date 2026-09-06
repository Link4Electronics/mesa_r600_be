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

#include "terascale_nir.h"

#include "util/macros.h"
#include "nir_builder.h"

#include <stdbool.h>
#include <stddef.h>

static bool
terascale_nir_lower_sin_cos_to_wrapped_normalized_instr(nir_builder * const builder,
                                                        nir_alu_instr * const alu,
                                                        UNUSED void * const callback_data)
{
   /* Similar to `nir_normalize_sin_cos`, but with `ffract` because the hardware expects the
    * argument to be in the [-256, 256] range.
    */

   if (alu->op != nir_op_fsin && alu->op != nir_op_fcos) {
      return false;
   }

   builder->cursor = nir_before_instr(&alu->instr);
   builder->fp_math_ctrl = alu->fp_math_ctrl;

   /* The 1/(2*PI) constant is the exact value of the GFX8+ inline constant. */
   nir_def * const src = nir_ffract(builder, nir_fmul(builder, nir_ssa_for_alu_src(builder, alu, 0),
                                                      nir_imm_int(builder, 0x3E22F983)));
   nir_def_replace(&alu->def, alu->op == nir_op_fsin ? nir_fsin_normalized_2_pi(builder, src)
                                                     : nir_fcos_normalized_2_pi(builder, src));

   return true;
}

bool
terascale_nir_lower_sin_cos_to_wrapped_normalized_r789xx(nir_shader * const shader)
{
   return nir_shader_alu_pass(shader, terascale_nir_lower_sin_cos_to_wrapped_normalized_instr,
                              nir_metadata_control_flow, NULL);
}
