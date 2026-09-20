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

#include "terakan_meta_impl.h"

#include "terakan_image.h"
#include "terakan_vk_entrypoints.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>

enum {
   TERAKAN_META_BLIT_CONST_X_SCALE,
   TERAKAN_META_BLIT_CONST_X_OFFSET,
   TERAKAN_META_BLIT_CONST_Y_SCALE,
   TERAKAN_META_BLIT_CONST_Y_OFFSET,
   TERAKAN_META_BLIT_CONST_Z_SCALE,
   TERAKAN_META_BLIT_CONST_Z_OFFSET,

   TERAKAN_META_BLIT_CONSTS_COUNT,
};

static_assert(
   (TERAKAN_META_BLIT_CONST_X_SCALE >> 1) == (TERAKAN_META_BLIT_CONST_X_OFFSET >> 1) &&
      (TERAKAN_META_BLIT_CONST_Y_SCALE >> 1) == (TERAKAN_META_BLIT_CONST_Y_OFFSET >> 1),
   "Multiply-additions for XY must not use more than 2 kcache qword read ports to be executable in "
   "one instruction group. For that, requiring that the scale and the offset are loaded via 1 read "
   "port.");

/* Use the last sampler register, which is the least likely to be used by application draws and
 * dispatches and need to be re-emitted after the blit.
 */
#define TERAKAN_META_BLIT_SAMPLER_INDEX (TERAKAN_SAMPLER_HW_COUNT_PER_STAGE - 1)

/* Like `terakan_meta_position_and_layer_from_index_vs`, but transforms the fixed-point destination
 * layer index into the floating-point source Z.
 */

static uint32_t const terakan_meta_blit_vs_r8xx[] = {
   /* 0: Export the instance ID as the render target array layer index. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT,

   /* 1: Vertex position and source Z calculation. */
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_Z_SCALE)) |
      S_SQ_CF_ALU_WORD1_COUNT(7) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 2: Export the source Z as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: Export the position in R0.XY01 and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 4: ALU clause. */

   /* +0-3: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y, and convert the instance ID to floating-point to PS (`UINT_TO_FLT` can be executed only
    * on the transcendental unit).
    * Cycle 0: X = R0.
    * Cycle 2: W = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(false, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   TERAKAN_SHADER_OP1_NW(true, 'Z', UINT_TO_FLT, EG, 0, 'W', SCL_210),
   16,
   0,

   /* +4-6: Convert the vertex position X from uint to float to R0.X, pass PV.Y further
    * (`UINT_TO_FLT` can be executed only on the transcendental unit), and transform the instance ID
    * into the source Z to R0.Z.
    */
   TERAKAN_SHADER_OP1_NW(false, 'Y', MOV, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_Z_SCALE) |
      TERAKAN_SHADER_OP3(false, 0, 'Z', MULADD_IEEE, EG, V_SQ_ALU_SRC_PS, 0, 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_Z_OFFSET),
   TERAKAN_SHADER_OP1(true, 0, 'X', UINT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', SCL_210),

   /* +7: Convert the vertex position Y from uint to float to R0.Y. */
   TERAKAN_SHADER_OP1(true, 0, 'Y', UINT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', SCL_210),
};

static uint32_t const terakan_meta_blit_vs_r9xx[] = {
   /* 0: Export the instance ID as the render target array layer index. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT,

   /* 1: Vertex position and source Z calculation. */
   S_SQ_CF_WORD0_ADDR(5) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_Z_SCALE)) |
      S_SQ_CF_ALU_WORD1_COUNT(6) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 2: Export the source Z as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: Export the position in R0.XY01. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 4: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* +0-3: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y, and convert the instance ID to floating-point to PV.Z.
    * Cycle 0: XW = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(false, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   TERAKAN_SHADER_OP1_NW(true, 'Z', UINT_TO_FLT, EG, 0, 'W', VEC_012),
   16,
   0,

   /* +4-6: Convert the vertex position from uint to float to R0.XY, and transform the instance ID
    * into the source Z to R0.Z.
    */
   TERAKAN_SHADER_OP1(false, 0, 'X', UINT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP1(false, 0, 'Y', UINT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_Z_SCALE) |
      TERAKAN_SHADER_OP3(true, 0, 'Z', MULADD_IEEE, EG, V_SQ_ALU_SRC_PV, 'Z', 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_Z_OFFSET),
};

struct terakan_meta_shader const terakan_meta_blit_vs = {
   .r8xx =
      {
         .program = terakan_meta_blit_vs_r8xx,
         .program_size_bytes = sizeof(terakan_meta_blit_vs_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .vs =
                        {
                           .pa_cl_vs_out_cntl = S_02881C_USE_VTX_RENDER_TARGET_INDX(1) |
                                                S_02881C_VS_OUT_MISC_VEC_ENA(1),
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_blit_vs_r9xx,
         .program_size_bytes = sizeof(terakan_meta_blit_vs_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .vs =
                        {
                           .pa_cl_vs_out_cntl = S_02881C_USE_VTX_RENDER_TARGET_INDX(1) |
                                                S_02881C_VS_OUT_MISC_VEC_ENA(1),
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
};

static uint32_t const terakan_meta_blit_ps_r8xx[] = {
   /* 0: Texture coordinate calculation. */
   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_X_SCALE)) |
      S_SQ_CF_ALU_WORD1_COUNT(2) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Sampling the source texture. */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: ALU clause. */

   /* +0-2: Transform the destination XY into the source coordinates, and load the Z coordinate, to
    * R0.
    * Cycle 0: XY = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_X_SCALE) |
      TERAKAN_SHADER_OP3(false, 0, 'X', MULADD_IEEE, EG, 0, 'X', 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_X_OFFSET),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_Y_SCALE) |
      TERAKAN_SHADER_OP3(false, 0, 'Y', MULADD_IEEE, EG, 0, 'Y', 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_Y_OFFSET),
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 6-7: Sample the the source texture to R0. */
   S_SQ_TEX_WORD0_TEX_INST(19) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_SRV_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_TEX_WORD1_COORD_TYPE_X(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_Y(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_Z(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_W(V_SQ_TEX_WORD1_COORD_UNNORMALIZED),
   S_SQ_TEX_WORD2_SAMPLER_ID(TERAKAN_META_BLIT_SAMPLER_INDEX) |
      S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_blit_ps_r9xx[] = {
   /* 0: Texture coordinate calculation. */
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_X_SCALE)) |
      S_SQ_CF_ALU_WORD1_COUNT(2) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Sampling the source texture. */
   S_SQ_CF_WORD0_ADDR(8),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 4: ALU clause. */

   /* +0-2: Transform the destination XY into the source coordinates, and load the Z coordinate, to
    * R0.
    * Cycle 0: XY = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_X_SCALE) |
      TERAKAN_SHADER_OP3(false, 0, 'X', MULADD_IEEE, EG, 0, 'X', 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_X_OFFSET),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_Y_SCALE) |
      TERAKAN_SHADER_OP3(false, 0, 'Y', MULADD_IEEE, EG, 0, 'Y', 0, 0, 0, 0, VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_Y_OFFSET),
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 7 (alignment padding), 8-9: Sample the the source texture to R0. */
   0,
   0,
   S_SQ_TEX_WORD0_TEX_INST(19) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_SRV_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_TEX_WORD1_COORD_TYPE_X(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_Y(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_Z(V_SQ_TEX_WORD1_COORD_UNNORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_W(V_SQ_TEX_WORD1_COORD_UNNORMALIZED),
   S_SQ_TEX_WORD2_SAMPLER_ID(TERAKAN_META_BLIT_SAMPLER_INDEX) |
      S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_blit_ps = {
   .r8xx =
      {
         .program = terakan_meta_blit_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_blit_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(true),
                              },
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_POSITION_ENA(true) |
                                    S_0286CC_POSITION_ADDR(0) | S_0286CC_LINEAR_GRADIENT_ENA(true),
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(true),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_blit_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_blit_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(true),
                              },
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_POSITION_ENA(true) |
                                    S_0286CC_POSITION_ADDR(0) | S_0286CC_LINEAR_GRADIENT_ENA(true),
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(true),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .samplers_used = BITFIELD_BIT(TERAKAN_META_BLIT_SAMPLER_INDEX),
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
   .primary_meta_srv_used = true,
};

VKAPI_ATTR void VKAPI_CALL
terakan_vk_CmdBlitImage2(VkCommandBuffer const commandBuffer,
                         VkBlitImageInfo2 const * const pBlitImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pBlitImageInfo->dstImage);
   struct terakan_image const * const src_image =
      terakan_image_from_handle(pBlitImageInfo->srcImage);

   /* Z scaling is possible only when blitting from 3D to 3D.
    *
    * VUID-VkImageBlit2-layerCount-08800:
    *     "If neither of the `layerCount` members of `srcSubresource` or `dstSubresource` are
    *     `VK_REMAINING_ARRAY_LAYERS`, the `layerCount` members of `srcSubresource` or
    *     `dstSubresource` must match"
    *
    * VUID-VkBlitImageInfo2-maintenance8-10579:
    *     "If the `maintenance8` feature is enabled, `dstImage` is `VK_IMAGE_TYPE_3D`, and
    *     `srcImage` is not of type `VK_IMAGE_TYPE_3D`, then for each element of `pRegions`, the
    *     absolute difference of the `z` member of each member of `dstOffsets` must equal
    *     `srcSubresource.layerCount`"
    *
    * VUID-VkBlitImageInfo2-maintenance8-10580:
    *     "If the `maintenance8` feature is enabled, `srcImage` is `VK_IMAGE_TYPE_3D`, and
    *     `dstImage` is not of type `VK_IMAGE_TYPE_3D`, then for each element of `pRegions`, the
    *     absolute difference of the `z` member of each member of `srcOffsets` must equal
    *     `dstSubresource.layerCount`"
    */

   bool const dst_3d = dst_image->vk.image_type == VK_IMAGE_TYPE_3D;
   bool const src_3d = src_image->vk.image_type == VK_IMAGE_TYPE_3D;

   static struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode =
         TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
      .rasterization = {.enable = true},
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer, TERAKAN_META_SHADER_BLIT_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_BLIT_PS);

   bool const filter_linear = pBlitImageInfo->filter == VK_FILTER_LINEAR;

   struct terakan_sampler_descriptor const app_sampler = *terakan_hw_config_sqk_get_sampler(
      &command_writer->hw_config_sqk, MESA_SHADER_FRAGMENT, TERAKAN_META_BLIT_SAMPLER_INDEX);
   struct terakan_sampler_descriptor const sampler = {
      .sampler = {
         S_03C000_CLAMP_X(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
            S_03C000_CLAMP_Y(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
            S_03C000_CLAMP_Z(V_03C000_SQ_TEX_CLAMP_LAST_TEXEL) |
            S_03C000_XY_MAG_FILTER(filter_linear ? V_03C000_SQ_TEX_XY_FILTER_BILINEAR
                                                 : V_03C000_SQ_TEX_XY_FILTER_POINT) |
            S_03C000_XY_MIN_FILTER(filter_linear ? V_03C000_SQ_TEX_XY_FILTER_BILINEAR
                                                 : V_03C000_SQ_TEX_XY_FILTER_POINT) |
            S_03C000_Z_FILTER(filter_linear && dst_3d && src_3d ? V_03C000_SQ_TEX_Z_FILTER_LINEAR
                                                                : V_03C000_SQ_TEX_Z_FILTER_POINT) |
            S_03C000_MIP_FILTER(V_03C000_SQ_TEX_Z_FILTER_POINT),
         0,
         S_03C008_TRUNCATE_COORD(!filter_linear) | S_03C008_TYPE(1),
      }};
   terakan_hw_config_sqk_set_sampler_fs(&command_writer->hw_config_sqk,
                                        TERAKAN_META_BLIT_SAMPLER_INDEX, &sampler);
   /* Must not return without restoring the application sampler from now. */

   float constants[TERAKAN_META_BLIT_CONSTS_COUNT] = {};
   char old_constants[sizeof(constants)];
   struct terakan_meta_config_push_constants_state constants_state =
      terakan_meta_config_push_constants_state_init(sizeof(old_constants), old_constants);

   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {.image = dst_image};
   struct terakan_image_descriptor_create_info src_descriptor_create_info = {.image = src_image};

   for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
      VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

      /* Source layer offsetting is done in the shader, because the range isn't used for 3D SRVs,
       * and to reduce SRV binding changes when blitting from different ranges of layers of the same
       * image.
       */

      bool const dst_reverse_z = dst_3d && region->dstOffsets[0].z > region->dstOffsets[1].z;
      struct terakan_image_descriptor_subresource_range dst_subresource_range = {
         .base_mip_level = region->dstSubresource.mipLevel,
         .max_level_count = 1,
         .base_z_or_array_layer = dst_3d ? (uint32_t)region->dstOffsets[dst_reverse_z].z
                                         : region->dstSubresource.baseArrayLayer,
         .max_depth_or_layer_count = dst_3d ? (uint32_t)(region->dstOffsets[!dst_reverse_z].z -
                                                         region->dstOffsets[dst_reverse_z].z)
                                            : region->dstSubresource.layerCount,
      };
      src_descriptor_create_info.subresource_range =
         (struct terakan_image_descriptor_subresource_range){
            .base_mip_level = region->srcSubresource.mipLevel,
            .max_level_count = 1,
            .max_depth_or_layer_count = UINT32_MAX,
         };
      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                      dst_image, &dst_subresource_range, false) ||
                   !terakan_image_descriptor_subresource_range_sanitize(
                      src_image, &src_descriptor_create_info.subresource_range, false))) {
         continue;
      }

      bool const dst_reverse_x = region->dstOffsets[0].x > region->dstOffsets[1].x;
      bool const dst_reverse_y = region->dstOffsets[0].y > region->dstOffsets[1].y;
      struct terakan_screen_rect const dst_region_rect = terakan_vk_rect_to_screen_rect(
         (VkRect2D){
            .offset =
               {
                  .x = region->dstOffsets[dst_reverse_x].x,
                  .y = region->dstOffsets[dst_reverse_y].y,
               },
            .extent =
               {
                  .width = (uint32_t)(region->dstOffsets[!dst_reverse_x].x -
                                      region->dstOffsets[dst_reverse_x].x),
                  .height = (uint32_t)(region->dstOffsets[!dst_reverse_y].y -
                                       region->dstOffsets[dst_reverse_y].y),
               },
         },
         (struct terakan_screen_rect){
            .bounds = {
               [1] = {u_minify(dst_image->vk.extent.width,
                               dst_descriptor_create_info.subresource_range.base_mip_level),
                      u_minify(dst_image->vk.extent.height,
                               dst_descriptor_create_info.subresource_range.base_mip_level)}}});

      constants[TERAKAN_META_BLIT_CONST_X_SCALE] =
         (float)(region->srcOffsets[1].x - region->srcOffsets[0].x) /
         (float)(region->dstOffsets[1].x - region->dstOffsets[0].x);
      constants[TERAKAN_META_BLIT_CONST_X_OFFSET] =
         (float)region->srcOffsets[0].x -
         (float)region->dstOffsets[0].x * constants[TERAKAN_META_BLIT_CONST_X_SCALE];
      constants[TERAKAN_META_BLIT_CONST_Y_SCALE] =
         (float)(region->srcOffsets[1].y - region->srcOffsets[0].y) /
         (float)(region->dstOffsets[1].y - region->dstOffsets[0].y);
      constants[TERAKAN_META_BLIT_CONST_Y_OFFSET] =
         (float)region->srcOffsets[0].y -
         (float)region->dstOffsets[0].y * constants[TERAKAN_META_BLIT_CONST_Y_SCALE];

      if (src_3d) {
         if (dst_3d) {
            constants[TERAKAN_META_BLIT_CONST_Z_SCALE] =
               (float)(region->srcOffsets[1].z - region->srcOffsets[0].z) /
               (float)(region->dstOffsets[1].z - region->dstOffsets[0].z);
         } else {
            /* No Z scaling when copying to non-3D, but may reverse when copying from 3D. */
            constants[TERAKAN_META_BLIT_CONST_Z_SCALE] =
               region->srcOffsets[0].z > region->srcOffsets[1].z ? -1.0f : 1.0f;
         }
      } else {
         /* No Z scaling when copying from non-3D, but may reverse when copying to 3D. */
         constants[TERAKAN_META_BLIT_CONST_Z_SCALE] = dst_reverse_z ? -1.0f : 1.0f;
      }
      float const z_offset_src =
         src_3d ? (float)region->srcOffsets[0].z : (float)region->srcSubresource.baseArrayLayer;
      /* Offsetting by 0.5 is needed for 3D to 3D, and also for a reverse source Z range. It's also
       * safe for a non-3D source, because scaling along Z is not allowed in Vulkan in this case,
       * and the GPU floors the fixed-point Z when sampling array textures (like for 3D textures
       * with point sampling) to determine the array layer, rather than rounding it to the nearest.
       */
      float const z_offset_dest_add_base =
         0.5f - (dst_reverse_z ? (float)(region->dstOffsets[0].z - region->dstOffsets[1].z) : 0.0f);

      u_foreach_bit (dst_vk_aspect_bit_index, region->dstSubresource.aspectMask) {
         VkImageAspectFlagBits const dst_vk_aspect =
            (VkImageAspectFlagBits)((VkImageAspectFlags)1 << dst_vk_aspect_bit_index);
         dst_descriptor_create_info.image_aspect_index =
            terakan_format_aspect_index(dst_image->format_info.aspect_map, dst_vk_aspect, 0);
         dst_descriptor_create_info.view_format =
            dst_image->format_info.aspect_formats[dst_descriptor_create_info.image_aspect_index];
         /* VUID-VkImageBlit2-aspectMask-00238: "The `aspectMask` member of `srcSubresource` and
          * `dstSubresource` must match"
          */
         src_descriptor_create_info.image_aspect_index =
            terakan_format_aspect_index(src_image->format_info.aspect_map, dst_vk_aspect, 0);
         src_descriptor_create_info.view_format =
            src_image->format_info.aspect_formats[src_descriptor_create_info.image_aspect_index];

         struct terakan_srv_descriptor src_descriptor;
         if (unlikely(!terakan_image_create_srv_descriptor(
                &src_descriptor_create_info,
                src_3d ? V_030000_SQ_TEX_DIM_3D : V_030000_SQ_TEX_DIM_2D_ARRAY, NULL,
                &src_descriptor))) {
            continue;
         }
         terakan_hw_config_sqk_set_srv_fs(&command_writer->hw_config_sqk,
                                          TERAKAN_SRV_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                          src_image->bo, &src_descriptor);

         dst_descriptor_create_info.subresource_range = dst_subresource_range;
         /* Stored explicitly as "integer + 0.5", not scaled by `TERAKAN_META_BLIT_CONST_Z_SCALE`,
          * so it can be adjusted to a different layer without accumulating rounding error.
          */
         float z_offset_dest_add = z_offset_dest_add_base;

         do {
            struct terakan_color_descriptor dst_descriptor;
            uint32_t const dst_descriptor_slices = terakan_image_create_color_descriptor(
               &dst_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &dst_descriptor, NULL);
            if (unlikely(dst_descriptor_slices == 0)) {
               break;
            }
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xF, &dst_image->bo, &dst_descriptor, NULL,
               TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

            constants[TERAKAN_META_BLIT_CONST_Z_OFFSET] =
               z_offset_src + z_offset_dest_add * constants[TERAKAN_META_BLIT_CONST_Z_SCALE];
            terakan_meta_config_draw_update_kcache_push_constants(command_writer, &constants_state,
                                                                  constants, true, true);

            terakan_meta_draw_rect(command_writer, dst_region_rect, dst_descriptor_slices);

            dst_descriptor_create_info.subresource_range.base_z_or_array_layer +=
               dst_descriptor_slices;
            dst_descriptor_create_info.subresource_range.max_depth_or_layer_count -=
               dst_descriptor_slices;
            z_offset_dest_add += (float)dst_descriptor_slices;
         } while (dst_descriptor_create_info.subresource_range.max_depth_or_layer_count != 0);
      }
   }

   terakan_hw_config_sqk_set_sampler_fs(&command_writer->hw_config_sqk,
                                        TERAKAN_META_BLIT_SAMPLER_INDEX, &app_sampler);
}
