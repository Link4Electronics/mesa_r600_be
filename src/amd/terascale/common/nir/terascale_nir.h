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

#ifndef TERASCALE_NIR_H
#define TERASCALE_NIR_H

#include "nir.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TODO(Triang3l): Pass for lowering constant LOD biases to fixed-point in `backend_flags` where the
 * conversion is exact.
 */

/* Lowers LOD biases to partial derivate exponent biases in texture sampling.
 *
 * Cube map coordinates must be in the 3D coordinate space, because 2D coordinates have
 * discontinuities between faces.
 */
bool terascale_nir_lower_txb_to_txd(nir_shader *shader);

/* Applies transformations to texture instructions, primarily to sampling coordinates, needed to use
 * the hardware texture instructions.
 *
 * `terascale_nir_lower_txb_to_txd` must be done beforehand.
 *
 * This doesn't lower descriptor information queries, as that may depend on the bindings in the
 * shader ABI of the client API implementation.
 */
bool terascale_nir_lower_tex(nir_shader *shader);

/* For R7xx+, which expect the argument in [-512*PI, +512*PI] remapped to [-256, +256]. */
bool terascale_nir_lower_sin_cos_to_wrapped_normalized_r789xx(nir_shader *shader);

#ifdef __cplusplus
}
#endif

#endif /* TERASCALE_NIR_H */
