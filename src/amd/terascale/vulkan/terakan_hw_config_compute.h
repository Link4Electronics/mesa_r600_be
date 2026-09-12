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

#ifndef TERAKAN_HW_CONFIG_COMPUTE_H
#define TERAKAN_HW_CONFIG_COMPUTE_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_shader.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bottom-level emission of packets setting compute context registers.
 *
 * For all configuration referencing BOs, it is expected that #MemoryIntegrity has been ensured
 * before calling the setter.
 */

struct terakan_hw_config_compute {
   struct {
      bool sq_pgm_ls;
      /* Rarely used, and not a part of `terakan_shader_static`, so modification is tracked
       * separately.
       */
      bool sq_ring_itemsize_lstmp;
      uint16_t cb_immed;
      uint16_t cb_color;
   } modified_;

   struct terakan_shader_static const * sq_pgm_ls_;

   uint16_t sq_ring_itemsize_lstmp_;

   struct {
      /* 3 bits with a value in the [0, 4] interval for each UAV. */
      uint64_t uav_bytes_per_element_log2;
   } cb_immed_;

   struct {
      struct terakan_bo_and_color_descriptor uav[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
   } cb_color_;
};

/* Binding a NULL shader causes `TERAKAN_META_SHADER_DUMMY_EMPTY_CS` to be used. */
static inline void
terakan_hw_config_compute_set_sq_pgm_ls(struct terakan_hw_config_compute * const config,
                                        struct terakan_shader_static const * const value)
{
   if (config->sq_pgm_ls_ == value) {
      return;
   }
   config->sq_pgm_ls_ = value;
   config->modified_.sq_pgm_ls = true;
}

static inline void
terakan_hw_config_compute_set_sq_ring_itemsize_lstmp_dwords(
   struct terakan_hw_config_compute * const config, uint16_t const value)
{
   if (config->sq_ring_itemsize_lstmp_ == value) {
      return;
   }
   config->sq_ring_itemsize_lstmp_ = value;
   config->modified_.sq_ring_itemsize_lstmp = true;
}

static inline void
terakan_hw_config_compute_set_cb_immed(struct terakan_hw_config_compute * const config,
                                       unsigned const uav_index,
                                       unsigned const uav_bytes_per_element_log2)
{
   assert(uav_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);
   assert(uav_bytes_per_element_log2 <= 4u);
   uint64_t const old_uavs_bytes_per_element_log2 = config->cb_immed_.uav_bytes_per_element_log2;
   unsigned const uav_shift = 3u * uav_index;
   config->cb_immed_.uav_bytes_per_element_log2 =
      (old_uavs_bytes_per_element_log2 & ~((uint64_t)0b111 << uav_shift)) |
      ((uint64_t)uav_bytes_per_element_log2 << uav_shift);
   if (config->cb_immed_.uav_bytes_per_element_log2 != old_uavs_bytes_per_element_log2) {
      config->modified_.cb_immed |= BITFIELD_BIT(uav_index);
   }
}

/* If `terakan_color_descriptor_is_bound` for the provided arguments is false, the UAV is unbound.
 */
void terakan_hw_config_compute_set_cb_color(struct terakan_hw_config_compute * config,
                                            unsigned color_index, struct terakan_bo const * bo,
                                            struct terakan_color_descriptor const * color);

struct terakan_gfx_command_writer;

/* Updates modification tracking to be consistent with the result of a `CLEAR_STATE` packet for
 * compute.
 */
void terakan_hw_config_compute_state_cleared(struct terakan_hw_config_compute * config);

/* Emits packets setting registers that never change within the compute context. */
void terakan_hw_config_compute_emit_constant(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_compute_emit_modified(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_compute_reset(struct terakan_hw_config_compute * config);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_COMPUTE_H */
