/*
 * Copyright © 2026 Terakan contributors
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

#include "terakan_nir.h"

/* Terakan reports a subgroup size of 1 because SFN has no wave-level subgroup operations, and
 * because TeraScale compute invocations only need to agree with the reported size for the
 * operations guaranteed by `VK_SUBGROUP_FEATURE_BASIC_BIT`. With a size of 1, every subgroup
 * contains exactly one invocation, numbered 0, so all subgroup operations reduce to local values.
 *
 * The lowering covers every NIR subgroup intrinsic that can reach SFN, including those behind
 * capabilities that are not advertised yet. Advertising a capability still requires testing it on
 * hardware, but leaving an intrinsic behind would make shader compilation fail rather than produce
 * a well-defined singleton result.
 */

bool terakan_nir_lower_subgroups(nir_shader * nir);

static bool
terakan_nir_lower_subgroups_filter(nir_instr const * const instr, UNUSED void const * const data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }

   switch (nir_instr_as_intrinsic(instr)->intrinsic) {
   case nir_intrinsic_load_subgroup_size:
   case nir_intrinsic_load_subgroup_invocation:
   case nir_intrinsic_load_subgroup_id:
   case nir_intrinsic_load_num_subgroups:
   case nir_intrinsic_load_local_invocation_index:
   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_ge_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
   case nir_intrinsic_first_invocation:
   case nir_intrinsic_elect:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_xor:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_rotate:
   case nir_intrinsic_ballot:
   case nir_intrinsic_ballot_relaxed:
   case nir_intrinsic_inverse_ballot:
   case nir_intrinsic_ballot_bitfield_extract:
   case nir_intrinsic_ballot_bit_count_reduce:
   case nir_intrinsic_ballot_bit_count_inclusive:
   case nir_intrinsic_ballot_bit_count_exclusive:
   case nir_intrinsic_ballot_find_lsb:
   case nir_intrinsic_ballot_find_msb:
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
      return true;
   default:
      return false;
   }
}

static nir_def *
terakan_nir_singleton_ballot(nir_builder * const builder, nir_def const * const def,
                             nir_def * const bit)
{
   nir_def * components[NIR_MAX_VEC_COMPONENTS];
   components[0] = nir_b2iN(builder, bit, def->bit_size);
   for (unsigned component = 1; component < def->num_components; ++component) {
      components[component] = nir_imm_intN_t(builder, 0, def->bit_size);
   }
   return nir_vec(builder, components, def->num_components);
}

static nir_def *
terakan_nir_singleton_ballot_bit(nir_builder * const builder, nir_def * const ballot)
{
   return nir_i2b(builder, nir_iand_imm(builder, nir_channel(builder, ballot, 0), 1));
}

static nir_def *
terakan_nir_subgroup_identity(nir_builder * const builder,
                              nir_intrinsic_instr const * const intrinsic)
{
   nir_op const reduction_op = (nir_op)nir_intrinsic_reduction_op(intrinsic);
   nir_const_value const identity = nir_alu_binop_identity(reduction_op, intrinsic->def.bit_size);
   /* One value per component: `nir_build_imm` reads as many as it is told there are, so handing it
    * a single one for a vector result reads past it. The identity is the same in every component.
    */
   nir_const_value identity_components[NIR_MAX_VEC_COMPONENTS];
   for (unsigned component = 0; component < intrinsic->def.num_components; ++component) {
      identity_components[component] = identity;
   }
   return nir_build_imm(builder, intrinsic->def.num_components, intrinsic->def.bit_size,
                        identity_components);
}

/* Linear index of the invocation within the workgroup, used as the subgroup ID, and as the number
 * of subgroups, when every subgroup contains one invocation. Does not use
 * `nir_load_workgroup_size()` where the workgroup size is known at compile time because Terakan
 * does not populate SFN's buffer-info constants.
 */
static nir_def *
terakan_nir_compute_local_invocation_index(nir_builder * const builder)
{
   assert(builder->shader->info.stage == MESA_SHADER_COMPUTE);

   nir_def * const local_id = nir_load_local_invocation_id(builder);
   nir_def * const x = nir_channel(builder, local_id, 0);

   if (!builder->shader->info.workgroup_size_variable) {
      unsigned const size_x = builder->shader->info.workgroup_size[0];
      unsigned const size_y = builder->shader->info.workgroup_size[1];
      if (size_y == 1 && builder->shader->info.workgroup_size[2] == 1) {
         return x;
      }
      nir_def * const y = nir_channel(builder, local_id, 1);
      if (builder->shader->info.workgroup_size[2] == 1) {
         return nir_iadd(builder, x, nir_imul_imm(builder, y, size_x));
      }
      nir_def * const z = nir_channel(builder, local_id, 2);
      return nir_iadd(builder, nir_iadd(builder, x, nir_imul_imm(builder, y, size_x)),
                      nir_imul_imm(builder, z, size_x * size_y));
   }

   nir_def * const size = nir_load_workgroup_size(builder);
   return nir_iadd(builder,
                   nir_iadd(builder, x,
                            nir_imul(builder, nir_channel(builder, local_id, 1),
                                     nir_channel(builder, size, 0))),
                   nir_imul(builder, nir_channel(builder, local_id, 2),
                            nir_imul(builder, nir_channel(builder, size, 0),
                                     nir_channel(builder, size, 1))));
}

static nir_def *
terakan_nir_compute_num_subgroups(nir_builder * const builder)
{
   assert(builder->shader->info.stage == MESA_SHADER_COMPUTE);

   if (!builder->shader->info.workgroup_size_variable) {
      return nir_imm_int(builder, builder->shader->info.workgroup_size[0] *
                                     builder->shader->info.workgroup_size[1] *
                                     builder->shader->info.workgroup_size[2]);
   }

   nir_def * const size = nir_load_workgroup_size(builder);
   return nir_imul(builder,
                   nir_imul(builder, nir_channel(builder, size, 0),
                            nir_channel(builder, size, 1)),
                   nir_channel(builder, size, 2));
}

static nir_def *
terakan_nir_lower_subgroups_impl(nir_builder * const builder, nir_instr * const instr,
                                 UNUSED void * const data)
{
   nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
   nir_shader * const shader = builder->shader;

   switch (intrinsic->intrinsic) {
   case nir_intrinsic_load_subgroup_size:
      return nir_imm_int(builder, 1);

   case nir_intrinsic_load_subgroup_invocation:
   case nir_intrinsic_first_invocation:
      return nir_imm_int(builder, 0);

   case nir_intrinsic_load_subgroup_id:
      if (shader->info.stage != MESA_SHADER_COMPUTE) {
         return nir_imm_int(builder, 0);
      }
      return terakan_nir_compute_local_invocation_index(builder);

   case nir_intrinsic_load_num_subgroups:
      if (shader->info.stage != MESA_SHADER_COMPUTE) {
         return nir_imm_int(builder, 1);
      }
      return terakan_nir_compute_num_subgroups(builder);

   case nir_intrinsic_load_local_invocation_index:
      if (shader->info.stage != MESA_SHADER_COMPUTE) {
         return nir_imm_int(builder, 0);
      }
      return terakan_nir_compute_local_invocation_index(builder);

   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_ge_mask:
   case nir_intrinsic_load_subgroup_le_mask:
      return terakan_nir_singleton_ballot(builder, &intrinsic->def, nir_imm_true(builder));
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
      return terakan_nir_singleton_ballot(builder, &intrinsic->def, nir_imm_false(builder));

   case nir_intrinsic_elect:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq:
      return nir_imm_true(builder);

   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_xor:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_rotate:
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
      return intrinsic->src[0].ssa;

   case nir_intrinsic_ballot:
   case nir_intrinsic_ballot_relaxed:
      return terakan_nir_singleton_ballot(builder, &intrinsic->def, intrinsic->src[0].ssa);
   case nir_intrinsic_inverse_ballot:
      return terakan_nir_singleton_ballot_bit(builder, intrinsic->src[0].ssa);
   case nir_intrinsic_ballot_bit_count_reduce:
   case nir_intrinsic_ballot_bit_count_inclusive:
      return nir_b2iN(builder, terakan_nir_singleton_ballot_bit(builder, intrinsic->src[0].ssa),
                      intrinsic->def.bit_size);
   case nir_intrinsic_ballot_bitfield_extract:
      return terakan_nir_singleton_ballot_bit(builder, intrinsic->src[0].ssa);
   case nir_intrinsic_ballot_bit_count_exclusive:
   case nir_intrinsic_ballot_find_lsb:
   case nir_intrinsic_ballot_find_msb:
      return nir_imm_intN_t(builder, 0, intrinsic->def.bit_size);

   case nir_intrinsic_exclusive_scan:
      return terakan_nir_subgroup_identity(builder, intrinsic);

   default:
      UNREACHABLE("Unexpected subgroup intrinsic");
   }
}

bool
terakan_nir_lower_subgroups(nir_shader * const nir)
{
   return nir_shader_lower_instructions(nir, terakan_nir_lower_subgroups_filter,
                                        terakan_nir_lower_subgroups_impl, NULL);
}
