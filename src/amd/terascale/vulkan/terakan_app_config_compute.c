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

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void
terakan_app_config_compute_set_cb_color(struct terakan_app_config_compute * const config,
                                        unsigned const mutable_srv_index,
                                        struct terakan_bo const * const bo,
                                        struct terakan_color_descriptor const * const color)
{
   assert(mutable_srv_index < TERAKAN_SRV_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL);

   if (!terakan_color_descriptor_is_bound(bo, color)) {
      BITSET_CLEAR(config->cb_color_.uav_bound, mutable_srv_index);
      return;
   }

   assert(G_028C70_RAT(color->info));
   struct terakan_bo_and_color_descriptor * const uav = &config->cb_color_.uav[mutable_srv_index];
   uav->bo = bo;
   uav->color = *color;
   BITSET_SET(config->cb_color_.uav_bound, mutable_srv_index);
}

void
terakan_app_config_compute_apply(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_app_config_compute * const config = &command_writer->app_config_compute;
   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   /* Base. */

   terakan_hw_config_shared_compute_set_vgt_compute_start(
      &command_writer->hw_config_shared, config->vgt_compute_start_[0],
      config->vgt_compute_start_[1], config->vgt_compute_start_[2]);

   /* Shader. */

   struct terakan_shader_impl const * const sq_pgm_ls = config->sq_pgm_ls_;

   uint32_t const * const spi_compute_num_thread =
      sq_pgm_ls != NULL ? sq_pgm_ls->static_state.stage.cs.spi_compute_num_thread
                        : terakan_gfx_command_writer_device(command_writer)
                             ->meta_shaders[TERAKAN_META_SHADER_DUMMY_EMPTY_CS]
                             .stage.cs.spi_compute_num_thread;
   terakan_hw_config_shared_compute_set_vgt_compute_thread_group_size(
      &command_writer->hw_config_shared,
      spi_compute_num_thread[0] * spi_compute_num_thread[1] * spi_compute_num_thread[2]);

   if (sq_pgm_ls == NULL) {
      terakan_hw_config_compute_set_sq_pgm_ls(&command_writer->hw_config_compute, NULL);

      terakan_hw_config_shared_compute_set_sq_ring_usage_lstmp(&command_writer->hw_config_shared,
                                                               false);
      terakan_hw_config_compute_set_sq_ring_itemsize_lstmp_dwords(
         &command_writer->hw_config_compute, 0);

      terakan_hw_config_sqk_set_usage_cs(&command_writer->hw_config_sqk, NULL);

      return;
   }

   terakan_hw_config_compute_set_sq_pgm_ls(&command_writer->hw_config_compute,
                                           &sq_pgm_ls->static_state);

   bool const lstmp_ring_used = sq_pgm_ls->scratch_item_size_dwords != 0;
   terakan_hw_config_shared_compute_set_sq_ring_usage_lstmp(&command_writer->hw_config_shared,
                                                            lstmp_ring_used);
   terakan_hw_config_compute_set_sq_ring_itemsize_lstmp_dwords(&command_writer->hw_config_compute,
                                                               sq_pgm_ls->scratch_item_size_dwords);
   if (lstmp_ring_used) {
      uint32_t const lstmp_ring_bytes_needed_shr8 =
         sq_pgm_ls->scratch_item_size_dwords *
         (uint32_t)terakan_gfx_command_writer_physical_device(command_writer)
            ->chip_info.sq_cstmp_ring_bytes_per_item_dword_shr8;
      uint32_t * const command_buffer_lstmp_ring_bytes_needed_shr8 =
         &command_writer->base.command_buffer
             ->shader_ring_bytes_needed_for_se_shr8[TERAKAN_SHADER_RING_INDEX_LSTMP];
      *command_buffer_lstmp_ring_bytes_needed_shr8 =
         MAX2(lstmp_ring_bytes_needed_shr8, *command_buffer_lstmp_ring_bytes_needed_shr8);
   }

   terakan_hw_config_sqk_set_usage_cs(&command_writer->hw_config_sqk, &sq_pgm_ls->sqk_usage);

   /* UAVs. */

   unsigned uav_count = 0;

   unsigned uav_uncompacted_index;
   BITSET_FOREACH_SET (uav_uncompacted_index, sq_pgm_ls->uav_usage.uavs_for_mutable_srvs_needed,
                       TERAKAN_SRV_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL) {
      assert(uav_count < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

      unsigned const uav_immediate_srv_index =
         TERAKAN_SRV_RANGE_UAV_IMMEDIATE_BASE_COMPUTE + uav_count;

      if (BITSET_SET(config->cb_color_.uav_bound, uav_uncompacted_index)) {
         struct terakan_bo_and_color_descriptor const * const uav =
            &config->cb_color_.uav[uav_uncompacted_index];

         if (G_028C70_RESOURCE_TYPE(uav->color.info) == V_028C70_BUFFER) {
            uint32_t * const uav_alignment_offset_constant =
               &command_writer->push_constants_state.driver_constants
                   .buffer_uav_alignment_offset[uav_count];
            if (*uav_alignment_offset_constant != uav->color.view) {
               *uav_alignment_offset_constant = uav->color.view;
               command_writer->push_constants_state.driver_constants_modified |=
                  BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_ALIGNMENT_OFFSET);
               /* The UAV alignment offset constants are set by both `app_config_compute` and
                * `app_config_draw`.
                */
               terakan_app_config_draw_set_pending(
                  &command_writer->app_config_draw,
                  TERAKAN_APP_CONFIG_DRAW_ENTRY_CB_COLOR_UAV_AND_UNUSED_MRT);
            }
            /* The driver-internal `view` field of the descriptor will be eliminated by `hw_config`.
             */
         }

         terakan_hw_config_compute_set_cb_color(&command_writer->hw_config_compute, uav_count,
                                                uav->bo, &uav->color);

         terakan_hw_config_compute_set_cb_immed(
            &command_writer->hw_config_compute, uav_count,
            util_logbase2(terascale_format_bytes_per_block[G_028C70_FORMAT(uav->color.info)]));
         struct terakan_srv_descriptor const uav_immediate_srv =
            terakan_color_descriptor_info_to_uav_immediate_srv(device, uav->color.info);
         terakan_hw_config_sqk_set_srv_cs(&command_writer->hw_config_sqk, uav_immediate_srv_index,
                                          device->uav_immediate_bo, &uav_immediate_srv);
      } else {
         terakan_hw_config_compute_set_cb_color(&command_writer->hw_config_compute, uav_count, NULL,
                                                NULL);

         /* Make 0 the return value of all operations using the unbound UAV. */
         terakan_hw_config_sqk_set_srv_cs(&command_writer->hw_config_sqk, uav_immediate_srv_index,
                                          NULL, NULL);
      }

      ++uav_count;
   }
}

void
terakan_app_config_compute_reset(struct terakan_app_config_compute * const config)
{
   memset(config->vgt_compute_start_, 0, sizeof(config->vgt_compute_start_));

   config->sq_pgm_ls_ = NULL;

   BITSET_ZERO(config->cb_color_.uav_bound);
}
