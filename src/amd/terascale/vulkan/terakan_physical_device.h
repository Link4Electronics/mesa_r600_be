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

#ifndef TERAKAN_PHYSICAL_DEVICE_H
#define TERAKAN_PHYSICAL_DEVICE_H

#include "terakan_instance.h"
#include "terakan_queue.h"
#include "wsi_common.h"

#include "gallium/drivers/r600/r600_isa.h"
#include "amd_family.h"
#include "nir.h"
#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_sync_binary.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_PHYSICAL_DEVICE_VENDOR_ID_ATI 0x1002

struct terakan_physical_device_pci_device_id {
   uint16_t device;
   uint16_t subsystem_vendor;
   uint16_t subsystem_device;
};

/* Accepts a 32-bit device ID since this is also used for filtering unsupported devices in
 * platform-specific physical device enumeration.
 */
enum radeon_family terakan_physical_device_get_chip_family(uint32_t pci_device_id);

static inline bool
terakan_physical_device_is_chip_family_supported(enum radeon_family const chip_family)
{
   return chip_family >= CHIP_CEDAR && chip_family <= CHIP_ARUBA;
}

struct terakan_physical_device_chip_info {
   struct terakan_physical_device_pci_device_id pci_device_id;

   enum radeon_family chip_family;

   bool is_r9xx;

   bool has_dedicated_vram;

   bool has_vertex_cache;
   /* 1 shader engine if false, up to 2 shader engines if true. */
   bool two_shader_engines_max;
   /* In the AMD Accelerated Parallel Processing OpenCL Programming Guide rev2.3 (July 2012) device
    * parameters table, this is "Max Wavefronts / GPU" divided by the number of shader engines (and
    * for Hemlock, also divided by the number of GPUs), and divided by the allocation granularity on
    * R8xx (8 wavefronts).
    * https://web.archive.org/web/20131111194717/http://developer.amd.com/wordpress/media/2012/10/AMD_Accelerated_Parallel_Processing_OpenCL_Programming_Guide4.pdf
    */
   uint32_t sq_max_threads_shr3;
   /* [Tessellation stages enabled][geometry stage enabled][register]. */
   uint32_t sq_thread_resource_mgmt_ts_gs_r8xx[2][2][2];
   uint32_t sq_max_stack_entries;
   /* Log2 of "Max Work-Items / GPU" divided by "Max Wavefronts / GPU" in the device parameters
    * table.
    */
   unsigned wave_lanes_log2;
   /* `SQ_PSTMP_RING_SIZE` increment per `SQ_PSTMP_RING_ITEMSIZE` preferred for the
    * `SQ_THREAD_RESOURCE_MGMT` configuration from this structure.
    */
   uint32_t sq_pstmp_ring_bytes_per_item_dword_shr8;
   /* Lanes * waves * shader engines. */
   uint32_t uav_immediate_size_elements;

   unsigned max_render_backends_log2;
};

void
terakan_physical_device_chip_info_init(struct terakan_physical_device_pci_device_id pci_device_id,
                                       struct terakan_physical_device_chip_info * chip_info_out);

struct terakan_physical_device_tiling_info {
   uint8_t pipes_log2;
   uint8_t banks_log2;
   uint8_t pipe_interleave_bytes_log2;
   uint8_t bank_interleave_log2;
   uint8_t row_bytes_log2;
};

static inline bool
terakan_physical_device_tiling_info_equal(
   struct terakan_physical_device_tiling_info const * const a,
   struct terakan_physical_device_tiling_info const * const b)
{
   return a->pipes_log2 == b->pipes_log2 && a->banks_log2 == b->banks_log2 &&
          a->pipe_interleave_bytes_log2 == b->pipe_interleave_bytes_log2 &&
          a->row_bytes_log2 == b->row_bytes_log2;
}

struct terakan_physical_device_submission_info {
   enum terakan_queue_relocation_type relocation_type;
   /* Maximum amount of additional data that may be added to submissions in the queue `submit`
    * function by the platform-specific implementation, but that doesn't need to be reserved inside
    * the `submit` arguments themselves.
    */
   struct terakan_queue_submission_size submission_outer_reserved_amount;
};

struct terakan_physical_device_submission_info_gfx {
   struct terakan_physical_device_submission_info base;

   /* Whether submissions referencing kcache buffers need to reset the constants mode to DX10 using
    * the MODE_CONTROL packet beforehand.
    */
   bool need_sq_alu_const_mode_control;
};

struct terakan_physical_device;
struct terakan_device;

struct terakan_physical_device_platform_fn {
   /* Called before `vk_physical_device_init` to get capabilities of additional extensions offered
    * by the platform, as well as the device UUID. It's assumed that before the call, the fields
    * this function is supposed to set are zero in the capability structures.
    */
   void (*get_platform_extensions)(struct terakan_physical_device const * device,
                                   struct vk_device_extension_table * extensions,
                                   struct vk_features * features,
                                   struct vk_properties * properties);

   VkResult (*create_device)(struct terakan_physical_device * physical_device,
                             VkDeviceCreateInfo const * create_info,
                             VkAllocationCallbacks const * allocator,
                             struct terakan_device ** device_out);

   void (*destroy)(struct terakan_physical_device * device);
};

/* Partially implemented by the platform specialization. */
struct terakan_physical_device {
   struct vk_physical_device vk;

   /* Private, use wrappers externally. */
   struct terakan_physical_device_platform_fn const * platform_fn;

   struct terakan_physical_device_chip_info chip_info;

   VkDeviceSize max_memory_allocation_size;

   struct terakan_physical_device_tiling_info tiling_info;
   VkDeviceSize buffer_image_bo_alignment;

   struct terakan_physical_device_submission_info_gfx submission_info_gfx;

   /* `nir_shader_compiler_options`'s lifetime must be at least as long as that of any NIR shader.
    */
   nir_shader_compiler_options nir_options;
   struct r600_isa * isa;

   VkPhysicalDeviceMemoryProperties memory_properties;

   struct wsi_device wsi_device;
};

VK_DEFINE_HANDLE_CASTS(terakan_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkExternalMemoryHandleTypeFlags terakan_physical_device_supported_external_memory_types(
   struct terakan_physical_device const * device);

void terakan_physical_device_finish(struct terakan_physical_device * device);

void terakan_physical_device_destroy(struct vk_physical_device * device);

struct terakan_physical_device_init_info {
   /* `*_static` pointers must point to objects whose lifetime is not shorter than that of the
    * physical device.
    */

   struct terakan_instance * instance;

   struct terakan_physical_device_platform_fn const * platform_fn_static;

   struct terakan_physical_device_pci_device_id pci_device_id;

   uint32_t gtt_allocation_granularity;

   uint64_t gtt_size;
   uint64_t vram_size;
   /* Included in `vram_size`. */
   uint64_t vram_visible;

   uint64_t max_memory_allocation_size;

   uint32_t min_memory_map_alignment;

   struct terakan_physical_device_tiling_info tiling_info;

   /* Can be 0 if not available, in which case timestamp queries will be disabled. */
   uint32_t clock_crystal_frequency_hz;

   struct terakan_physical_device_submission_info_gfx submission_info_gfx;

   struct vk_sync_type const * const * supported_sync_types_static;
};

VkResult terakan_physical_device_init(struct terakan_physical_device * device,
                                      struct terakan_physical_device_init_info const * init_info);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PHYSICAL_DEVICE_H */
