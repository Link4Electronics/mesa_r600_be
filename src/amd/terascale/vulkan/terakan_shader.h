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

#ifndef TERAKAN_SHADER_H
#define TERAKAN_SHADER_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_push_constants.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "nir.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP (0x7F - 4)

/* For easier writing of meta shaders: initializer for two array elements containing two words of an
 * instruction with the most important fields.
 *
 * To set other fields, use OR on the left for the first word, or OR on the right for the second
 * word.
 *
 * `VALID_PIXEL_MODE` may be enabled only in pixel shaders.
 */

#define TERAKAN_SHADER_CF_END_R9XX                                                                 \
   0, (S_SQ_CF_WORD1_BARRIER(true) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END)

/* All vertex shaders export at least one parameter. */

#define TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_AND_END_R8XX_2_QWORDS                     \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60)),                                                    \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE),                                      \
      (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |               \
       S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                  \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |                                                  \
       S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |                                           \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* No barriers, expecting no other parameter exports. */
#define TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_POS_PARAM_DONE_R9XX_2_QWORDS                             \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60)),                                                    \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE),                                      \
      (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |               \
       S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                  \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

#define TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_AND_END_R8XX                                        \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                     \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |                                                  \
       S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |                                           \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* No barrier, expecting no other exports. */
#define TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_R9XX                                                \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                     \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |                             \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* Note that the actual handling of ARRAY_SIZE for burst writes (that write multiple array elements
 * with the given stride from consecutive GPR indices) differs from what the Evergreen Family
 * Instruction Set Architecture reference says.
 * ARRAY_SIZE is not minus 1.
 * On R8xx, for STORE_RAW, it's in bytes.
 * On R9xx, for STORE_DWORD (and STORE_RAW, which still exists, but is changed to "reserved" in the
 * documentation), it's in dwords.
 */

#define TERAKAN_SHADER_CF_UAV(cacheless, uav_inst, uav_id, index_gpr, data_gpr, comp_mask,         \
                              valid_pixel_mode, barrier)                                           \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_ID(uav_id) |                                                \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_INST(V_RAT_INST_##uav_inst) |                               \
    S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_WRITE_IND) |              \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(data_gpr) |                                                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_INDEX_GPR(index_gpr)),                                              \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_COMP_MASK(comp_mask) |                                       \
       S_SQ_CF_ALLOC_EXPORT_WORD1_VALID_PIXEL_MODE(valid_pixel_mode) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(barrier) |                                               \
       ((cacheless) ? EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT_CACHELESS                   \
                    : EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT))

#define TERAKAN_SHADER_CF_UAV_COMBINED_STORE(is_r9xx, uav_id, data_gpr, two_component,             \
                                             valid_pixel_mode, barrier)                            \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_ID(uav_id) |                                                \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_INST((is_r9xx) ? V_RAT_INST_STORE_DWORD                     \
                                                      : V_RAT_INST_STORE_RAW) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_WRITE) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(data_gpr)),                                                  \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_COMP_MASK((two_component) ? 0b1011 : 0b1001) |               \
       S_SQ_CF_ALLOC_EXPORT_WORD1_VALID_PIXEL_MODE(valid_pixel_mode) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(barrier) |                                               \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT_COMBINED_CACHELESS)

/* If the sources need to be set up by some other means (such as using the
 * TERAKAN_KCACHE_DWORD_WORD#_SRC# macros), pass 0 as src_sel and src_chan.
 *
 * BANK_SWIZZLE is VEC_012 or SCL_210 most of the time, but it being explicitly required by these
 * macros serves as a reminder to be mindful of read port conflicts and the unit that will be
 * executing the last instruction (on R8xx, if the opcode can be executed by either the vector or
 * the scalar unit, and the destination component is not used by another instruction, the vector
 * unit will execute it, like with PREFER_VECTOR on R6xx/R7xx).
 */

static_assert(('X' & 3) == 0 && ('Y' & 3) == 1 && ('Z' & 3) == 2 && ('W' & 3) == 3,
              "XYZW character literals must be usable as component indices");
static_assert(('x' & 3) == 0 && ('y' & 3) == 1 && ('z' & 3) == 2 && ('w' & 3) == 3,
              "xyzw character literals must be usable as component indices");

/* The WM version (with an explicit write mask) is primarily for internal use by other SHADER_OP1/2
 * macros. The writing or non-writing (NW) macros should be used instead where possible.
 */
#define TERAKAN_SHADER_OP2_WM(last, write_mask, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, \
                              src1_sel, src1_chan, bank_swizzle)                                   \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(dst_gpr) | S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                 \
       S_SQ_ALU_WORD1_OP2_WRITE_MASK(write_mask) |                                                 \
       S_SQ_ALU_WORD1_OP2_ALU_INST(isa##_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_##inst) |                  \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))
#define TERAKAN_SHADER_OP2(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,      \
                           src1_chan, bank_swizzle)                                                \
   TERAKAN_SHADER_OP2_WM(last, true, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,  \
                         src1_chan, bank_swizzle)
#define TERAKAN_SHADER_OP2_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel, src1_chan, \
                              bank_swizzle)                                                        \
   TERAKAN_SHADER_OP2_WM(last, false, TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP, dst_chan, inst,     \
                         isa, src0_sel, src0_chan, src1_sel, src1_chan, bank_swizzle)
/* For single-operand instructions, passing src0 to src1. If it's a GPR, the load from the GPR read
 * port of src0 is reused for src1 in the hardware as a special case, and if it's a constant used on
 * the transcendental unit, it doesn't matter whether one or two constants are loaded, because
 * there's no other operand to load anyway.
 * Note that if relative source addressing is needed, it needs to be enabled for both src0 and src1.
 */
#define TERAKAN_SHADER_OP1(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, bank_swizzle)  \
   TERAKAN_SHADER_OP2_WM(last, true, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src0_sel,  \
                         src0_chan, bank_swizzle)
#define TERAKAN_SHADER_OP1_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, bank_swizzle)        \
   TERAKAN_SHADER_OP2_WM(last, false, TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP, dst_chan, inst,     \
                         isa, src0_sel, src0_chan, src0_sel, src0_chan, bank_swizzle)

#define TERAKAN_SHADER_OP3(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,      \
                           src1_chan, src2_sel, src2_chan, bank_swizzle)                           \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(dst_gpr) | S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                 \
       S_SQ_ALU_WORD1_OP3_ALU_INST(isa##_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_##inst) |                  \
       S_SQ_ALU_WORD1_OP3_SRC2_SEL(src2_sel) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN((src2_chan) & 3) |     \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))

/* Writing to a GPR that's generally out of bounds (but not clause-temporary), for cases where the
 * result doesn't need to be written.
 */
#define TERAKAN_SHADER_OP3_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel, src1_chan, \
                              src2_sel, src2_chan, bank_swizzle)                                   \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP) |                           \
       S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                                                   \
       S_SQ_ALU_WORD1_OP3_ALU_INST(isa##_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_##inst) |                  \
       S_SQ_ALU_WORD1_OP3_SRC2_SEL(src2_sel) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN((src2_chan) & 3) |     \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))

#define TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2 8
#define TERAKAN_SHADER_PROGRAM_ALIGNMENT      (1 << TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2)

enum terakan_shader_ring_index {
   TERAKAN_SHADER_RING_INDEX_LSTMP,
   TERAKAN_SHADER_RING_INDEX_HSTMP,
   TERAKAN_SHADER_RING_INDEX_ESTMP,
   TERAKAN_SHADER_RING_INDEX_GSTMP,
   TERAKAN_SHADER_RING_INDEX_VSTMP,
   TERAKAN_SHADER_RING_INDEX_PSTMP,

   TERAKAN_SHADER_RING_INDEX_COUNT,
};

static_assert(TERAKAN_SHADER_RING_INDEX_COUNT <= 32,
              "Using shader ring buffer indices in 32-bit bitfields.");

#define TERAKAN_SHADER_RINGS_PER_SHADER_ENGINE                                                     \
   (BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_LSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_HSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_ESTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_GSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_VSTMP) | BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP))

struct terakan_shader_ring {
   uint64_t base_wddm_patch_ids;
   /* The size register is the next after the base register. */
   uint32_t base_size_config_reg_offset;
   uint32_t item_size_context_reg_offset;
   /* Pipeline stages potentially accessing this ring. */
   VkPipelineStageFlags2 stages;
   uint32_t sx_surface_sync_mask;
};

extern struct terakan_shader_ring const terakan_shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT];

/* `DB_SHADER_CONTROL` value when the pixel shader doesn't override the depth and stencil values,
 * coverage, and interaction with other DB functionality, and doesn't have memory side effects.
 * One of the use cases of this is drawing without an application-provided pixel shader.
 * Dual export is enabled because the DB shader export isn't used, though CB may require it to be
 * disabled (if any written RTV uses a 128bpp export format).
 */
#define TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY                                                  \
   (S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) | S_02880C_DUAL_EXPORT_ENABLE(1) |              \
    S_02880C_DB_SOURCE_FORMAT(V_02880C_EXPORT_DB_TWO))

/* Fields that don't depend on any other state. */
struct terakan_shader_static {
   struct terakan_bo const * program_bo;
   uint32_t program_va_shr8;

   uint32_t sq_pgm_resources[2];

   union {
      struct {
         uint32_t spi_vs_out_id[10];
         uint32_t spi_vs_out_config;
         uint32_t pa_cl_vs_out_cntl;
      } vs;

      struct {
         uint32_t sq_pgm_exports_ps;
         uint32_t spi_ps_input_cntl[32];
         uint32_t spi_ps_in_control[2];
         uint32_t spi_input_z;
         uint32_t spi_baryc_cntl;
         /* This must include all color exports done by the shader, otherwise there will be hangs
          * (tested with dEQP-VK.pipeline.monolithic.blend.dual_source.* on Barts).
          * The CB_SHADER_MASK rules for dual-source blending described in Radeon Evergreen /
          * Northern Islands Acceleration are incorrect.
          */
         uint32_t cb_shader_mask;
      } ps;

      struct {
         uint32_t spi_compute_num_thread[3];
         /* In dwords. */
         uint16_t sq_lds_alloc_size;
      } cs;
   } stage;
};

struct terakan_shader_sqk_usage {
   uint16_t kcache;

   uint32_t samplers;

   BITSET_DECLARE(resources, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   /* If a buffer is bound at an index included in `resources_uncached`, it needs to be bound with
    * `UNCACHED = 1`. This is needed primarily for coherence between UAV writes and resource reads
    * within an invocation, and for reading the UAV operation return value.
    */
   BITSET_DECLARE(resources_uncached, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
};

struct terakan_shader_uav_usage {
   /* The actual `CB_COLOR` indices of UAVs are the counts of set bits prior to the bit for the
    * given index of the resource corresponding to the UAV in the mutable resource range, and for
    * fragment shaders, the number of set bits in `rtv_dsb_uncompacted_exports` is added (but not
    * for the `IMMED` buffer read resource index) to them.
    * Expected to be padded to the bitset size with zero bits, so it can be used in `memcmp`.
    */
   BITSET_DECLARE(uavs_for_mutable_resources_needed,
                  MAX2(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL,
                       TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL));
};

/* Information about the mapping between application and hardware RTV indices.
 *
 * Unlike in Direct3D 11.0 with `OMSetRenderTargetsAndUnorderedAccessViews`, in Vulkan, color
 * attachment output locations are unrelated to storage bindings. The only limitation shared by them
 * is `maxFragmentCombinedOutputResources`, but it applies only to the total count, not to the
 * locations.
 * It's not possible to fill holes in RTV outputs with UAVs because:
 * - According to Radeon Evergreen / Northern Islands Acceleration, "all RAT MRTs must be assigned
 *   to higher number MRTs than normal rendering MRTs";
 * - Even if UAVs between RTVs were allowed in the hardware, UAV bindings may be arrays with
 *   indirect indexing, in which case it may be desirable to always place them in contiguous MRT
 *   ranges, instead of splitting them using additional logic in both the emitted shader code and
 *   the binding architecture if there's no hole large enough.
 *
 * Therefore, in Terakan, application color attachment output locations are compacted when hardware
 * RTV indices are calculated.
 *
 * The compaction also reduces `SQ_PGM_EXPORTS_PS.EXPORT_COLORS` and the number of RTVs enabled in
 * `CB_SHADER_MASK` (according to the Gallium AMD drivers, unused attachments still need to be
 * enabled in `CB_SHADER_MASK` to prevent hangs).
 *
 * For dual-source blending, the second source is taken from the MRT 1 export in the hardware, whose
 * value may come from either:
 * - The `layout(location = 0, index = 1)` output, which is the correct way of specifying the second
 *   blend factor in Vulkan and OpenGL, or:
 * - The `layout(location = 1)` output, which is incorrect in Vulkan and OpenGL, but is used in some
 *   applications, as it happens to work on various drivers, and is the way the second blend factor
 *   is set in Direct3D.
 *
 * With one color attachment output and dual-source blending, therefore, UAV MRT indices must not be
 * smaller than 2 - this is why `maxFragmentCombinedOutputResources` is limited to 11 rather than to
 * all 12 MRTs.
 * Otherwise, UAV MRT indices must not be smaller than `util_bitcount(rtv_uncompacted_exports)`.
 * The current convention in Terakan is that UAV MRTs are placed immediately after RTV and DSB MRTs.
 *
 * Advanced blending equation feedback loop resources may share the resource range with UAV IMMED
 * buffers. However, in order to keep the length of the range 11 rather than 12 (to make it possible
 * to provide one more mutable-type resource descriptor to the application), the
 * `layout(location = 0, index = 1)` dual-source blending factor must not occupy space in the range.
 * The current convention in Terakan, for simplicity and to reduce register changes when the color
 * attachment MRT count is changed, is to place UAV IMMED resources upward at the beginning of the
 * range, and the advanced blending equation feedback loop resources downward at the end.
 */
struct terakan_shader_rtv_mapping {
   uint8_t rtv_uncompacted_exports;
   bool dual_source_blend_via_location_0_index_1;
};

static inline bool
terakan_shader_rtv_mapping_equal(struct terakan_shader_rtv_mapping const a,
                                 struct terakan_shader_rtv_mapping const b)
{
   return a.rtv_uncompacted_exports == b.rtv_uncompacted_exports &&
          a.dual_source_blend_via_location_0_index_1 == b.dual_source_blend_via_location_0_index_1;
}

static inline uint8_t
terakan_shader_rtv_mapping_rtv_dsb_uncompacted_exports(
   struct terakan_shader_rtv_mapping const rtv_mapping)
{
   return rtv_mapping.rtv_uncompacted_exports |
          (rtv_mapping.dual_source_blend_via_location_0_index_1 ? 0b10u : 0u);
}

/* Shader implementation common for both pipelines (to be used in a pipeline-cached wrapper) and
 * shader objects (for an uncached wrapper implementing VkShaderEXT).
 */
struct terakan_shader_impl {
   /* The BO isn't owned by this object. */
   struct terakan_shader_static static_state;

   uint16_t scratch_item_size_dwords;

   struct terakan_push_constants_usage push_constants_usage;

   struct terakan_shader_sqk_usage sqk_usage;

   struct terakan_shader_uav_usage uav_usage;

   struct {
      uint32_t vertex_attributes_needed;
   } vs;

   struct {
      /* DUAL_EXPORT_ENABLE specifies only whether it can potentially be enabled for the shader. */
      uint32_t db_shader_control;

      struct terakan_shader_rtv_mapping rtv_mapping;
   } fs;

   struct r600_shader shader;
};

struct terakan_device;

/* TODO(Triang3l): Replace the old version with the new. */
/* Converts SPIR-V to NIR and performs pre-link lowerings. */
nir_shader * terakan_nir_from_spirv_pre_link(struct terakan_device * device,
                                             size_t spirv_size_bytes, uint32_t const * spirv,
                                             mesa_shader_stage stage, char const * entrypoint,
                                             VkSpecializationInfo const * specialization_info);

struct terakan_nir_get_pipeline_layout_binding_functions;
struct terakan_physical_device;

struct terakan_nir_lower_and_optimize_post_link_options {
   /* Must be set to the next graphics stage for pre-rasterization stages, or to `MESA_SHADER_NONE`
    * for other stages.
    * If this is the last pre-rasterization stage, the next stage must be `MESA_SHADER_FRAGMENT`
    * regardless of whether the fragment shader is actually present (if it's not, varyings may be
    * eliminated by linkage prior to the call if desired).
    */
   mesa_shader_stage next_stage;

   struct terakan_nir_get_pipeline_layout_binding_functions const * get_binding_functions;
   void const * get_binding_functions_data;

   bool robust_buffer_access_2;

   struct terakan_physical_device const * physical_device;
};

struct terakan_nir_lower_and_optimize_post_link_result {
   struct terakan_shader_sqk_usage sqk_usage;
   uint32_t driver_push_constants_used;

   struct terakan_shader_rtv_mapping rtv_mapping;

   struct terakan_shader_uav_usage uav_usage;
};

struct terakan_nir_lower_and_optimize_post_link_result terakan_nir_lower_and_optimize_post_link(
   nir_shader * nir, struct terakan_nir_lower_and_optimize_post_link_options const * options);

void terakan_shader_impl_apply_lower_and_optimize_post_link_result(
   struct terakan_shader_impl * shader, union r600_shader_key * shader_key,
   struct terakan_nir_lower_and_optimize_post_link_result const * result, mesa_shader_stage stage);

void terakan_shader_impl_finish(struct terakan_shader_impl * shader);

/* Compiles the shader into its `shader.bc` (additionally possibly allocating `shader.arrays`,
 * which, if the function returns `VK_SUCCESS`, must be `free`d by the caller, such as via
 * `terakan_shader_impl_finish`, when it's not needed anymore), and fills the info from the backend
 * compiler. Assumes that everything in the shader not intended to be filled directly from the NIR
 * has been zeroed prior to the call.
 */
VkResult terakan_shader_impl_compile(struct terakan_shader_impl * shader,
                                     struct terakan_device * device,
                                     union r600_shader_key const * key, nir_shader * nir,
                                     VkAllocationCallbacks const * allocator);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_SHADER_H */
