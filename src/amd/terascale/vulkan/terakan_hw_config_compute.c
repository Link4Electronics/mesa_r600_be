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

#include "terakan_command_buffer.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

void
terakan_hw_config_compute_set_cb_color(struct terakan_hw_config_compute * const config,
                                       unsigned const color_index,
                                       struct terakan_bo const * const bo,
                                       struct terakan_color_descriptor const * const color)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   struct terakan_bo_and_color_descriptor * const config_uav = &config->cb_color_.uav[color_index];

   bool const color_was_bound =
      terakan_color_descriptor_is_bound(config_uav->bo, &config_uav->color);

   if (!terakan_color_descriptor_is_bound(bo, color)) {
      if (!color_was_bound) {
         return;
      }
      /* Clear the BO pointer specifically for quick comparison when a UAV is bound later. */
      config_uav->bo = NULL;
      config->modified_.cb_color |= BITFIELD_BIT(color_index);
      return;
   }

   assert(G_028C70_RAT(color->info));
   struct terakan_color_descriptor color_normalized = *color;
   terakan_color_descriptor_to_hw_uav(&color_normalized);

   if (color_was_bound && config_uav->bo == bo &&
       memcmp(&config_uav->color, &color_normalized, sizeof(struct terakan_color_descriptor)) ==
          0) {
      return;
   }
   config_uav->bo = bo;
   config_uav->color = color_normalized;
   config->modified_.cb_color |= BITFIELD_BIT(color_index);
}

void
terakan_hw_config_compute_state_cleared(struct terakan_hw_config_compute * const config)
{
   config->modified_.sq_pgm_ls = true;

   config->modified_.sq_ring_itemsize_lstmp = config->sq_ring_itemsize_lstmp_ != 0;

   config->modified_.cb_immed = BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   config->modified_.cb_color = 0b0;
   for (unsigned color_index = 0; color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT; ++color_index) {
      struct terakan_bo_and_color_descriptor const * const uav =
         &config->cb_color_.uav[color_index];
      if (terakan_color_descriptor_is_bound(uav->bo, &uav->color)) {
         config->modified_.cb_color |= BITFIELD_BIT(color_index);
      }
   }
}

/* Note that section 3.2 "State Requirements for Compute" of Radeon Evergreen / Northern Islands
 * Acceleration has various differences in how it defines the required configuration from what the
 * Windows driver 15.301.1901.0000 does, which in some cases configures less than defined in the
 * documentation.
 */

void
terakan_hw_config_compute_emit_constant(struct terakan_gfx_command_writer * const command_writer)
{
   /* This array contains only compile-time constants. */
   static uint32_t const common_config[] = {
      /* Same as in the Windows driver 15.301.1901.0000 on Barts. */
      PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
      TERAKAN_CONTEXT_REG_OFFSET(R_028B54_VGT_SHADER_STAGES_EN),
      S_028B54_LS_EN(V_028B54_CS_STAGE_ON),

      /* Same as in the Windows driver 15.301.1901.0000 on Barts. */
      PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
      TERAKAN_CONTEXT_REG_OFFSET(R_028A40_VGT_GS_MODE),
      0,

      /* Same as in the Windows driver 15.301.1901.0000 on Barts. */
      PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
      TERAKAN_CONTEXT_REG_OFFSET(R_028808_CB_COLOR_CONTROL),
      S_028808_MODE(V_028808_CB_NORMAL) |
         S_028808_ROP3(TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY),

      /* Currently constant for simplicity, not updated depending on the actual input usage of the
       * shader, as it's rare that either of those inputs is not used (mainly in single-group
       * shaders, which inherently are dispatched with small thread counts).
       */
      PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
      TERAKAN_CONTEXT_REG_OFFSET(R_0286E8_SPI_COMPUTE_INPUT_CNTL),
      S_0286E8_TID_IN_GROUP_ENA(true) | S_0286E8_TGID_ENA(true) | S_0286E8_DISABLE_INDEX_PACK(true),
   };

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         ARRAY_SIZE(common_config));
      if (unlikely(packet == NULL)) {
         return;
      }
      memcpy(packet, common_config, sizeof(common_config));
      packet += ARRAY_SIZE(common_config);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      /* This array contains only compile-time constants. */
      static uint32_t const r9xx_config[] = {
         /* Disable the soft LDS allocation limit for LS. */
         PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
         TERAKAN_CONTEXT_REG_OFFSET(CM_R_0286FC_SPI_LDS_MGMT),
         0,
      };

      {
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
            ARRAY_SIZE(r9xx_config));
         if (unlikely(packet == NULL)) {
            return;
         }
         memcpy(packet, r9xx_config, sizeof(r9xx_config));
         packet += ARRAY_SIZE(r9xx_config);
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   } else {
      /* This array contains only compile-time constants. */
      static uint32_t const r8xx_config[] = {
         /* Workaround for hardware issues with dynamic GPRs - must set all limits to 240 (in units
          * of 8 registers) instead of 0.
          */
         PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE,
         TERAKAN_CONTEXT_REG_OFFSET(R_028838_SQ_DYN_GPR_RESOURCE_LIMIT_1),
         S_028838_PS_GPRS(0x1E) | S_028838_VS_GPRS(0x1E) | S_028838_GS_GPRS(0x1E) |
            S_028838_ES_GPRS(0x1E) | S_028838_HS_GPRS(0x1E) | S_028838_LS_GPRS(0x1E),
      };

      {
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
            ARRAY_SIZE(r8xx_config));
         if (unlikely(packet == NULL)) {
            return;
         }
         memcpy(packet, r8xx_config, sizeof(r8xx_config));
         packet += ARRAY_SIZE(r8xx_config);
         terakan_gfx_command_writer_emit_done(command_writer, packet);
      }
   }
}

static void
terakan_hw_config_compute_emit_sq_pgm_ls(struct terakan_gfx_command_writer * const command_writer)
{
   if (!command_writer->hw_config_compute.modified_.sq_pgm_ls) {
      return;
   }
   command_writer->hw_config_compute.modified_.sq_pgm_ls = false;

   struct terakan_shader_static const * shader = command_writer->hw_config_compute.sq_pgm_ls_;
   if (shader == NULL) {
      shader = &terakan_gfx_command_writer_device(command_writer)
                   ->meta_shaders[TERAKAN_META_SHADER_DUMMY_EMPTY_CS];
   }

   uint32_t const packet_dwords =
      2 + ((R_0288D8_SQ_PGM_RESOURCES_2_LS - R_0288D0_SQ_PGM_START_LS) / sizeof(uint32_t) + 1) +
      /* R_0286EC_SPI_COMPUTE_NUM_THREAD_X, R_0286F0_SPI_COMPUTE_NUM_THREAD_Y,
       * R_0286F4_SPI_COMPUTE_NUM_THREAD_Z
       */
      2 + 3 +
      /* R_0288E8_SQ_LDS_ALLOC */
      2 + 1;

   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, packet_dwords, 1, 1, 0);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ =
      PKT3(PKT3_SET_CONTEXT_REG,
           (R_0288D8_SQ_PGM_RESOURCES_2_LS - R_0288D0_SQ_PGM_START_LS) / sizeof(uint32_t) + 1, 0) |
      TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0288D0_SQ_PGM_START_LS);
   uint32_t const * const packet_pgm_start = packet;
   *packet++ = shader->program_va_shr8;
   *packet++ = shader->sq_pgm_resources[0];
   *packet++ = shader->sq_pgm_resources[1];
   terakan_gfx_command_writer_add_relocation(
      command_writer, &packet, packet_pgm_start, *packet_pgm_start,
      TERASCALE_WDDM_PATCH_IDS_SQ_PGM_START_CS,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                shader->program_bo, true, false,
                                                TERAKAN_BO_PRIORITY_SHADER_BINARY));

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0286EC_SPI_COMPUTE_NUM_THREAD_X);
   for (unsigned dimension = 0; dimension < 3; ++dimension) {
      *packet++ = shader->stage.cs.spi_compute_num_thread[dimension];
   }

   uint32_t const group_thread_count = shader->stage.cs.spi_compute_num_thread[0] *
                                       shader->stage.cs.spi_compute_num_thread[1] *
                                       shader->stage.cs.spi_compute_num_thread[2];
   unsigned const wave_lanes_log2 =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_info.wave_lanes_log2;
   uint32_t const group_wave_count =
      (group_thread_count >> wave_lanes_log2) +
      ((group_thread_count & (((uint32_t)1 << wave_lanes_log2) - 1u)) != 0);

   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_0288E8_SQ_LDS_ALLOC);
   *packet++ = shader->stage.cs.sq_lds_alloc_size | ((uint32_t)group_wave_count << 14);

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_sq_ring_itemsize_lstmp(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!command_writer->hw_config_compute.modified_.sq_ring_itemsize_lstmp) {
      return;
   }
   command_writer->hw_config_compute.modified_.sq_ring_itemsize_lstmp = false;

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028830_SQ_LSTMP_RING_ITEMSIZE);
   *packet++ = command_writer->hw_config_compute.sq_ring_itemsize_lstmp_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_compute_emit_cb_immed(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);
   uint64_t const uav_bytes_per_element_log2 =
      command_writer->hw_config_compute.cb_immed_.uav_bytes_per_element_log2;
   unsigned modified_remaining = command_writer->hw_config_compute.modified_.cb_immed;
   assert(!(modified_remaining & ~BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)));
   while (modified_remaining) {
      int range_start, range_length;
      u_bit_scan_consecutive_range(&modified_remaining, &range_start, &range_length);
      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + range_length, 1,
         range_length, 0);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, range_length, 0) | TERAKAN_PACKET3_COMPUTE;
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028B9C_CB_IMMED0_BASE) + range_start;
      uint32_t * const packet_bases = packet;
      for (unsigned range_uav_index = 0; range_uav_index < (unsigned)range_length;
           ++range_uav_index) {
         *packet++ = device->uav_immediate_va_shr8[(uav_bytes_per_element_log2 >>
                                                    (3 * (range_start + range_uav_index))) &
                                                   0b111];
      }
      /* BO references can be added only after `terakan_gfx_command_writer_emit_with_bo`. */
      uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, device->uav_immediate_bo, false, true,
         TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);
      for (unsigned range_uav_index = 0; range_uav_index < (unsigned)range_length;
           ++range_uav_index) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, &packet_bases[range_uav_index], packet_bases[range_uav_index],
            TERASCALE_WDDM_PATCH_IDS_CB_IMMED_BASE | (range_start + range_uav_index), bo_reference);
      }
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
   command_writer->hw_config_compute.modified_.cb_immed = 0b0;
}

static void
terakan_hw_config_compute_emit_cb_color(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_compute * const config = &command_writer->hw_config_compute;

   /* DRM Radeon requires `ATTRIB` relocations regardless of `RADEON_CS_KEEP_TILING_FLAGS`. */
   bool const need_attrib_relocation =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->submission_info_gfx.base.relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP;

   assert(!(config->modified_.cb_color & ~BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)));
   while (config->modified_.cb_color) {
      unsigned const color_index = (unsigned)(ffs(config->modified_.cb_color) - 1);
      config->modified_.cb_color &= ~(uint16_t)BITFIELD_BIT(color_index);

      bool const has_meta = color_index < TERAKAN_COLOR_HW_RTV_COUNT;
      uint32_t const register_offset_dwords =
         has_meta
            ? (R_028C9C_CB_COLOR1_BASE - R_028C60_CB_COLOR0_BASE) / sizeof(uint32_t) * color_index
            : (R_028E40_CB_COLOR8_BASE - R_028C60_CB_COLOR0_BASE) / sizeof(uint32_t) +
                 (R_028E5C_CB_COLOR9_BASE - R_028E40_CB_COLOR8_BASE) / sizeof(uint32_t) *
                    (color_index - TERAKAN_COLOR_HW_RTV_COUNT);

      struct terakan_bo_and_color_descriptor const * const uav =
         &config->cb_color_.uav[color_index];

      if (!terakan_color_descriptor_is_bound(uav->bo, &uav->color)) {
         /* Emit the `INFO` register with `SOURCE_FORMAT` (dual-source blending needs RTV 0's export
          * format in the target 1).
          */
         uint32_t * packet = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
         if (unlikely(packet == NULL)) {
            return;
         }
         *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | TERAKAN_PACKET3_COMPUTE;
         *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C70_CB_COLOR0_INFO) + register_offset_dwords;
         *packet++ = S_028C70_SOURCE_FORMAT(V_028C70_EXPORT_4C_16BPC);
         terakan_gfx_command_writer_emit_done(command_writer, packet);
         continue;
      }

      unsigned const register_count =
         sizeof(struct terakan_color_descriptor) / sizeof(uint32_t) +
         (has_meta ? sizeof(struct terakan_color_meta_descriptor) / sizeof(uint32_t) : 0);

      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + register_count, 1,
         1 + (need_attrib_relocation ? 1 : 0) + (has_meta ? 2 : 0), 0);
      if (unlikely(packet == NULL)) {
         return;
      }

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, register_count, 0) | TERAKAN_PACKET3_COMPUTE;
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C60_CB_COLOR0_BASE) + register_offset_dwords;

      uint32_t * const packet_descriptor = packet;
      memcpy(packet_descriptor, &uav->color, sizeof(struct terakan_color_descriptor));
      if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
         packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)] &=
            C_028C74_NUM_SAMPLES & C_028C74_NUM_FRAGMENTS;
      }
      packet += sizeof(struct terakan_color_descriptor) / sizeof(uint32_t);
      uint32_t * const packet_meta_descriptor = packet;
      if (has_meta) {
         struct terakan_color_meta_descriptor const meta =
            terakan_color_meta_descriptor_create_disabled(&uav->color);
         memcpy(packet_meta_descriptor, &meta, sizeof(struct terakan_color_meta_descriptor));
         packet += sizeof(struct terakan_color_meta_descriptor) / sizeof(uint32_t);
      }

      /* TODO(Triang3l): Possibly accept the priority explicitly, at least for buffers. */
      uint32_t const bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, uav->bo, true, true,
         G_028C70_RESOURCE_TYPE(uav->color.info) == V_028C70_BUFFER
            ? TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER
            : TERAKAN_BO_PRIORITY_SHADER_RW_IMAGE);
      terakan_gfx_command_writer_add_relocation(
         command_writer, &packet,
         &packet_descriptor[offsetof(struct terakan_color_descriptor, base) / sizeof(uint32_t)],
         packet_descriptor[offsetof(struct terakan_color_descriptor, base) / sizeof(uint32_t)],
         TERASCALE_WDDM_PATCH_IDS_CB_COLOR_BASE | color_index, bo_reference);
      if (need_attrib_relocation) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)],
            packet_descriptor[offsetof(struct terakan_color_descriptor, attrib) / sizeof(uint32_t)],
            0, bo_reference);
      }
      if (has_meta) {
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, cmask) /
                                    sizeof(uint32_t)],
            packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, cmask) /
                                   sizeof(uint32_t)],
            TERASCALE_WDDM_PATCH_IDS_CB_COLOR_CMASK | color_index, bo_reference);
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet,
            &packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, fmask) /
                                    sizeof(uint32_t)],
            packet_meta_descriptor[offsetof(struct terakan_color_meta_descriptor, fmask) /
                                   sizeof(uint32_t)],
            TERASCALE_WDDM_PATCH_IDS_CB_COLOR_BASE | color_index, bo_reference);
      }

      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}

void
terakan_hw_config_compute_emit_modified(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_compute_emit_sq_pgm_ls(command_writer);

   terakan_hw_config_compute_emit_sq_ring_itemsize_lstmp(command_writer);

   terakan_hw_config_compute_emit_cb_immed(command_writer);

   terakan_hw_config_compute_emit_cb_color(command_writer);
}

void
terakan_hw_config_compute_reset(struct terakan_hw_config_compute * const config)
{
   /* Modification flags are initialized based on the configuration after `CLEAR_STATE`, though
    * they're set just to avoid referencing uninitialized data in configuration setters.
    */

   config->sq_pgm_ls_ = NULL;
   config->modified_.sq_pgm_ls = true;

   config->sq_ring_itemsize_lstmp_ = 0;
   config->modified_.sq_ring_itemsize_lstmp = false;

   /* Expecting UAVs to have 4 bytes per element (such as storage buffers) frequently. */
   config->cb_immed_.uav_bytes_per_element_log2 = UINT64_C(0222222222222);
   config->modified_.cb_immed = BITFIELD_MASK(TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   memset(config->cb_color_.uav, 0, sizeof(config->cb_color_.uav));
   config->modified_.cb_color = 0b0;
}
