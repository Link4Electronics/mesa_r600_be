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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

static bool
terascale_nir_lower_txb_to_txd_instr(nir_builder * const builder, nir_tex_instr * const tex,
                                     UNUSED void * const callback_data)
{
   if (tex->op != nir_texop_txb) {
      return false;
   }

   builder->cursor = nir_before_instr(&tex->instr);

   nir_def * const derivative_scale = nir_fexp2(builder, nir_steal_tex_src(tex, nir_tex_src_bias));

   nir_def * coord = nir_get_tex_src(tex, nir_tex_src_coord);
   if (tex->is_array) {
      assert(!tex->array_is_lowered_cube);
      coord = nir_trim_vector(builder, coord, coord->num_components - 1u);
   }

   nir_tex_instr_add_src(tex, nir_tex_src_ddx,
                         nir_fmul(builder, nir_ddx_coarse(builder, coord), derivative_scale));
   nir_tex_instr_add_src(tex, nir_tex_src_ddy,
                         nir_fmul(builder, nir_ddy_coarse(builder, coord), derivative_scale));

   tex->op = nir_texop_txd;

   return true;
}

bool
terascale_nir_lower_txb_to_txd(nir_shader * const shader)
{
   return nir_shader_tex_pass(shader, terascale_nir_lower_txb_to_txd_instr,
                              nir_metadata_control_flow, NULL);
}

static bool
terascale_nir_lower_tex_instr(nir_builder * const builder, nir_tex_instr * const tex,
                              UNUSED void * const callback_data)
{
   bool progress = false;

   builder->cursor = nir_before_instr(&tex->instr);

   assert(tex->op != nir_texop_txb &&
          "Texture sampling with LOD biasing must be lowered to partial derivative exponent "
          "biasing before transforming cube map coordinates to 2D");
   if (tex->op == nir_texop_tex || tex->op == nir_texop_txl || tex->op == nir_texop_txd ||
       tex->op == nir_texop_lod || tex->op == nir_texop_tg4) {
      int const tex_coord_src_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      assert(tex_coord_src_index >= 0);
      nir_src * const tex_coord_src = &tex->src[tex_coord_src_index].src;

      if (tex->is_array && tex->op != nir_texop_lod) {
         /* The hardware applies the `Z_FILTER` of the sampler to array layers, but Vulkan defines
          * the selection of the layer as rounding towards the nearest even.
          */
         unsigned const array_layer_component = tex_coord_src->ssa->num_components - 1u;
         nir_src_rewrite(
            tex_coord_src,
            nir_vector_insert_imm(builder, tex_coord_src->ssa,
                                  nir_fround_even(builder, nir_channel(builder, tex_coord_src->ssa,
                                                                       array_layer_component)),
                                  array_layer_component));
      }

      if (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE) {
         progress = true;

         /* The hardware accepts 2D ST coordinates within the primary face in the 1...2 range in XY,
          * and `cube index in the array * 8 + face` as floating-point in Z.
          *
          * The `cube_amd` ALU operation, which corresponds to the hardware `CUBE` instruction,
          * provides {t, s, major axis * 2, face}.
          *
          * The 2D coordinates used for sampling in the hardware are therefore
          * `cube.yx / abs(cube.z) + 1.5`.
          */

         static unsigned const swizzle_xyz[3] = {0, 1, 2};
         nir_def * const cube_tc_sc_ma2_face =
            nir_cube_amd(builder, nir_swizzle(builder, tex_coord_src->ssa, swizzle_xyz, 3));

         nir_def * const cube_sc = nir_channel(builder, cube_tc_sc_ma2_face, 1);
         nir_def * const cube_tc = nir_channel(builder, cube_tc_sc_ma2_face, 0);

         nir_def * const cube_ma2 = nir_channel(builder, cube_tc_sc_ma2_face, 2);
         nir_def * const cube_ma2_abs = nir_fabs(builder, cube_ma2);
         nir_def * const cube_ma2_abs_rcp = nir_frcp(builder, cube_ma2_abs);

         nir_def * const cube_face = nir_channel(builder, cube_tc_sc_ma2_face, 3);

         nir_def * cube_2d_coord_components[3] = {
            nir_ffma_weak_imm2(builder, cube_sc, cube_ma2_abs_rcp, 1.5),
            nir_ffma_weak_imm2(builder, cube_tc, cube_ma2_abs_rcp, 1.5),
            cube_face,
         };

         if (tex->is_array) {
            nir_def * const cube_array_layer = nir_channel(builder, tex_coord_src->ssa, 3);
            /* TODO(Triang3l): Array layer clamping if needed. */
            cube_2d_coord_components[2] =
               nir_ffma_weak_imm1(builder, cube_array_layer, 8.0, cube_2d_coord_components[2]);
            tex->array_is_lowered_cube = true;
         }

         nir_src_rewrite(tex_coord_src, nir_vec(builder, cube_2d_coord_components, 3));

         if (tex->op == nir_texop_txd) {
            /* Section 7.18.11 "LOD Calculations" of the Direct3D 11.3 Functional Specification
             * says:
             *
             *     "If the current texture is a TextureCube, transform the partial derivative
             *     vectors into the space of the primary TextureCube face as follows:
             *
             *     - Using TC, determine which component is of the largest magnitude, as when
             *       calculating the texel location(7.18.7). If any of the components are
             *       equivalent, precedence is as follows: Z, Y, X. The absolute value of this will
             *       be referred to as AxisMajor.
             *     - select and mirror the minor axes of TC as defined by the TextureCube coordinate
             *       space to generate TC'.uv
             *     - select and mirror the minor axes of the partial derivative vectors as defined
             *       by the TextureCube coordinate space, generating 2 new partial derivative
             *       vectors dX'.uv & dY'.uv.
             *     - Suppose DerivativeMajorX and DerivativeMajorY are the major axis component of
             *       the original partial derivative vectors.
             *     - Calculate 2 new dX and dY vectors for future calculations as follows:
             *           dX.uv = (AxisMajor*dX'.uv - TC'.uv*DerivativeMajorX)/(AxisMajor*AxisMajor)
             *           dY.uv = (AxisMajor*dY'.uv - TC'.uv*DerivativeMajorY)/(AxisMajor*AxisMajor)
             *
             *     Scale the derivatives by the texture size at largest mipmap:
             *
             *     if (IsTextureCube)
             *     {
             *         // multiplying by 0.5f to adjust for TextureCube coordinate system
             *         dX.uvw = 0.5f * dX.uvw * [NumTexelsAlongCubeSide,NumTexelsAlongCubeSide,0];
             *         dY.uvw = 0.5f * dY.uvw * [NumTexelsAlongCubeSide,NumTexelsAlongCubeSide,0];
             *     }
             *     else // [...]"
             *
             * With `cube_amd` returning the major axis times 2, the calculation, including the
             * final multiplication by 0.5, can be written as:
             *     (AxisMajorX2 * d' - TC' * DerivativeMajor * 2) / (AxisMajorX2 * AxisMajorX2)
             * Note that `AxisMajorX2` is specifically the absolute value - that is, `abs(cube.z)`.
             * `DerivativeMajor` is signed, however.
             *
             * Section "Cube Map Derivative Transformation" of the Vulkan 1.4.360 specification
             * defines the derivative calculation the same way.
             */

            int const cube_d_indices[2] = {
               nir_tex_instr_src_index(tex, nir_tex_src_ddx),
               nir_tex_instr_src_index(tex, nir_tex_src_ddy),
            };
            nir_src * cube_d[2];
            for (unsigned screen_axis = 0; screen_axis < 2; ++screen_axis) {
               int const cube_d_index = cube_d_indices[screen_axis];
               assert(cube_d_index >= 0);
               cube_d[screen_axis] = &tex->src[cube_d_index].src;
            }

            /* Major axis is Z:
             *    s = z < 0 ? -x : +x
             *    t = -y
             * Major axis is Y:
             *    s = +x
             *    t = y < 0 ? -z : +z
             * Major axis is X:
             *    s = x < 0 ? +z : -z
             *    t = -y
             *
             * Alternatively:
             * - s:
             *   - Major axis is Y: +x
             *   - Major axis is Z or X:
             *     - Major axis is Z: s_minor = +x
             *     - Major axis is X: s_minor = -z
             *     - ma >= 0 ? +s_minor : -s_minor
             * - t:
             *   - Major axis is Y: ma >= 0 ? +z : -z
             *   - Major axis is Z or X: -y
             */

            nir_def * cube_dxyz[3];
            for (unsigned cube_axis = 0; cube_axis < 3; ++cube_axis) {
               cube_dxyz[cube_axis] =
                  nir_vec2(builder, nir_channel(builder, cube_d[0]->ssa, cube_axis),
                           nir_channel(builder, cube_d[1]->ssa, cube_axis));
            }
            nir_def * const cube_dz_neg = nir_fneg(builder, cube_dxyz[2]);

            /* Independent operations, and values when the major axis is Y. */

            nir_def * const cube_ma_is_xy =
               nir_fge(builder, nir_imm_float(builder, 3.0f), cube_face);
            /* 1.0 is an inline constant in the ISA. */
            nir_def * const cube_ma_is_x =
               nir_fge(builder, nir_imm_float(builder, 1.0f), cube_face);

            nir_def * cube_dt = nir_fcsel_ge(builder, cube_ma2, cube_dxyz[2], cube_dz_neg);

            /* Dependencies on `cube_ma_is_xy` and `cube_ma_is_x`, and values when the major axis is
             * X or Z.
             */

            nir_def * const cube_ma_is_y = nir_ixor(builder, cube_ma_is_xy, cube_ma_is_x);

            nir_def * cube_dma2 = nir_bcsel(builder, cube_ma_is_x, cube_dxyz[0], cube_dxyz[2]);

            nir_def * cube_ds = nir_bcsel(builder, cube_ma_is_x, cube_dz_neg, cube_dxyz[0]);
            cube_ds = nir_fcsel_ge(builder, cube_ma2, cube_ds, nir_fneg(builder, cube_ds));

            /* Dependencies on `cube_ma_is_y`. */

            /* Multiplication by 2 can be done via the hardware ALU instruction output modifier. */
            cube_dma2 = nir_fmul_imm(
               builder, nir_bcsel(builder, cube_ma_is_y, cube_dxyz[1], cube_dma2), 2.0);

            cube_ds = nir_bcsel(builder, cube_ma_is_y, cube_dxyz[0], cube_ds);

            cube_dt = nir_bcsel(builder, cube_ma_is_y, cube_dt, nir_fneg(builder, cube_dxyz[1]));

            /* Calculate the derivatives given the vectors in the major axis coordinate space. */

            static unsigned const swizzle_yxyx[4] = {1, 0, 1, 0};
            static unsigned const swizzle_xxyy[4] = {0, 0, 1, 1};
            nir_def * const cube_ddh_ddv_2d = nir_fmul(
               builder,
               nir_ffma_weak(
                  builder, cube_ma2_abs,
                  nir_vec4(builder, nir_channel(builder, cube_ds, 0),
                           nir_channel(builder, cube_dt, 0), nir_channel(builder, cube_ds, 1),
                           nir_channel(builder, cube_dt, 1)),
                  nir_fmul(
                     builder,
                     nir_fneg(builder, nir_swizzle(builder, cube_tc_sc_ma2_face, swizzle_yxyx, 4)),
                     nir_swizzle(builder, cube_dma2, swizzle_xxyy, 4))),
               nir_fmul(builder, cube_ma2_abs_rcp, cube_ma2_abs_rcp));

            nir_def * const cube_zero = nir_imm_zero(builder, 1, 32);
            for (unsigned screen_axis = 0; screen_axis < 2; ++screen_axis) {
               nir_src_rewrite(
                  cube_d[screen_axis],
                  nir_vec3(builder, nir_channel(builder, cube_ddh_ddv_2d, 2u * screen_axis),
                           nir_channel(builder, cube_ddh_ddv_2d, 2u * screen_axis + 1u),
                           cube_zero));
            }
         }
      }
   }

   return progress;
}

bool
terascale_nir_lower_tex(nir_shader * const shader)
{
   return nir_shader_tex_pass(shader, terascale_nir_lower_tex_instr, nir_metadata_control_flow,
                              NULL);
}
