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

#ifndef TERAKAN_APP_CONFIG_COMPUTE_H
#define TERAKAN_APP_CONFIG_COMPUTE_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_shader.h"

#include "util/bitset.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `terakan_app_config_compute` is a layer on top of `terakan_hw_config` that specifically tracks
 * the compute state configuration desired by the application (as opposed to the internal meta
 * shaders).
 *
 * It's not reapplied when switching to a new indirect buffer - `terakan_hw_config` is re-emitted
 * instead, for both application's work and internal draws / dispatches. Thus, no register writes
 * must be done directly from `terakan_app_config_compute` application - it must first go to
 * `terakan_hw_config`.
 *
 * It also can modify `terakan_push_constants_state`.
 */

struct terakan_app_config_compute {
   uint32_t vgt_compute_start_[3];

   struct terakan_shader_impl const * sq_pgm_ls_;

   struct {
      /* For descriptor set binding simplicity, storing UAV descriptors in the configuration at the
       * same indices as the read-only resources corresponding to them, but to calculate UAV
       * indices, compacting the indices of the resources that have corresponding UAVs.
       *
       * The UAV array is large, so it's not initialized. Rather, if `uav_bound` doesn't have the
       * corresponding bit set for a UAV, the BO pointer and the color target descriptor are
       * undefined.
       */
      BITSET_DECLARE(uav_bound, TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL);
      struct terakan_bo_and_color_descriptor uav[TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL];
   } cb_color_;
};

static inline void
terakan_app_config_compute_set_vgt_compute_start(struct terakan_app_config_compute * const config,
                                                 uint32_t const x, uint32_t const y,
                                                 uint32_t const z)
{
   config->vgt_compute_start_[0] = x;
   config->vgt_compute_start_[1] = y;
   config->vgt_compute_start_[2] = z;
}

static inline void
terakan_app_config_compute_set_sq_pgm_ls(struct terakan_app_config_compute * const config,
                                         struct terakan_shader_impl const * const value)
{
   config->sq_pgm_ls_ = value;
}

/* If `terakan_color_descriptor_is_bound` for the provided arguments is false, the UAV is unbound.
 * #MemoryIntegrity is expected to be ensured before calling.
 */
void terakan_app_config_compute_set_cb_color(struct terakan_app_config_compute * config,
                                             unsigned mutable_resource_index,
                                             struct terakan_bo const * bo,
                                             struct terakan_color_descriptor const * color);

struct terakan_gfx_command_writer;

void terakan_app_config_compute_apply(struct terakan_gfx_command_writer * command_writer);

void terakan_app_config_compute_reset(struct terakan_app_config_compute * config);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_APP_CONFIG_COMPUTE_H */
