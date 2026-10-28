# AGENTS.md

Guidance for AI agents working in this repository.

## What this tree is

This is a Mesa fork for AMD TeraScale GPUs (R600, R700, Evergreen/R8xx and
Northern Islands/R9xx) with first-class support for big-endian hosts, primarily
ppc64 PowerMac/POWER machines.

Two graphics stacks are relevant:

- **r600g (Gallium/OpenGL)** — the current, working stack. It contains a large
  set of big-endian fixes that are not upstream.
- **Terakan (Vulkan)** — an experimental Vulkan driver written by Vitaliy
  "Triang3l" Kuzmin. It lives on the `Terakan` branch of
  `https://gitlab.freedesktop.org/Triang3l/mesa`. Upstream Terakan is developed
  and validated mostly on little-endian hosts and is not Vulkan-conformant.

The goal of this fork is to keep r600g correct on big-endian and to integrate
Terakan in a way that works on both little-endian and big-endian machines.

## Branches and important commits

- `terakan-be` (current integration branch) — Mesa 26.2.3 + r600g big-endian
  fixes + the X11 8888 presentation fix. It does **not** contain the Terakan
  driver sources yet.
- `rebase-26.2.3` — reference branch with the same 26.2.3 base and big-endian
  fixes. Useful for comparing/porting patches.
- `main`, `backup/main-pre-rebase` — older baselines. Do not use as a base for
  new work.
- `refs/remotes/triang3l/Terakan` — fetched upstream Terakan branch:
  `VERSION` is 27.0.0-devel, main is merged up to mid-October 2026, and the
  newest Terakan commit is 2026-10-24. Its merge base with 26.2.3 is the 26.2
  branch point (2026-07-03), so a merge into `terakan` drags in 6108 commits.
  Treat it as read-only upstream.

Important commits to preserve when rebasing or porting:

- `eff168a504c` — `dri: restore big-endian aliases for the 8888 DRM formats`.
  Without this, X11 depth-24/32 windows have red and green swapped on
  big-endian. It fixes `src/gallium/frontends/dri/dri_helpers.c`,
  `src/loader/loader_dri_helper.c` and `src/loader/loader_dri_helper.h`.
- `aac6ae1835d` — r600g big-endian query, trace and SSBO fixes.
- `452b7099404` — r600g little-endian format view for packed formats.
- Earlier commits on `terakan` contain the rest of the r600g big-endian
  bring-up (UBO/constant-buffer pre-swap, border colors, NPOT compressed
  mipmaps, surface tiling, etc.).

## Hardware and test hosts

### Big-endian (authoritative for this fork)

- ppc64 PowerMac G5, Arch POWER, Linux 7.2.x.
- AMD CAICOS / Radeon R5 230 (TeraScale 2, Evergreen).
- Xorg with `xf86-video-ati` and glamor, XFCE, 1920x1080 HDMI.
- Remote access is provided by the user out of band. Do not commit passwords,
  SSH keys or other credentials to this repository.

The user can only test on this big-endian machine. All runtime validation is
therefore done there.

### Little-endian

- The development host is x86_64 (little-endian) but has no TeraScale GPU.
- It can be used for little-endian compile tests, static analysis and
  `meson`/`ninja` coverage. Do not claim little-endian runtime validation
  unless a real little-endian TeraScale machine was used.

## Ground rules

1. **Do not break big-endian r600g.** It is the working graphics stack for the
   user's desktop.
2. **Do not assume little-endian.** Any `memcpy`, pointer cast or GPU buffer
   write involving multi-byte values must be checked for byte order.
3. **Keep changes focused.** Separate endianness fixes from Terakan integration
   changes and from unrelated Mesa updates.
4. **Use worktrees.** Do exploratory Terakan work in a separate git worktree and
   a separate Meson build directory. Never reuse the user's build directory
   for experiments.
5. **Ask before installing or rebooting.** The machine is in active use. Test
   with `VK_DRIVER_FILES`/`meson devenv` first, and only install with the user's
   approval.
6. **Do not make Terakan the system default.** It is experimental and
   non-conformant. Select it per process with `VK_DRIVER_FILES`.

## Mesa upstream AI policy

Upstream Mesa ships its own `AGENTS.md` (it is present on the Terakan branch
and was deleted from this fork's older branch). If any of this work is ever
proposed upstream, follow it:

- Never contribute to GitLab directly: no merge requests, no comments, no
  replies on the user's behalf.
- Label AI work in the commit message: `Generated-by: LLM` when the agent wrote
  the code, `Assisted-by: LLM` when it only helped a human.
- Do not put AI-generated prose, code comments, commit messages or MR comments
  into the public Mesa community.
- Do not modify `src/asahi`, `src/gallium/drivers/asahi`,
  `src/gallium/drivers/nouveau`, `src/nouveau`, `src/intel/compiler/jay`,
  `src/panfrost`, `src/gallium/drivers/panfrost` or `src/imagination`.

Local development in this fork does not go through Mesa review, but keeping the
trailers and the restricted-directory rule makes the patches upstreamable.

## Building

### r600g / OpenGL (current branch)

On the big-endian machine:

```sh
meson setup builddir \
  -Dgallium-drivers=r600 \
  -Dvulkan-drivers= \
  -Dplatforms=x11,wayland \
  -Dglx=dri \
  -Dllvm=disabled \
  -Dbuildtype=debugoptimized
ninja -C builddir
```

Install only when explicitly requested:

```sh
sudo ninja -C builddir install
```

### Terakan (from the upstream Terakan branch)

Terakan's Meson integration is in `src/amd/terascale`; it is enabled through
the `vulkan-drivers` option value `amd_terascale`.

```sh
meson setup builddir-terakan \
  -Dvulkan-drivers=amd_terascale \
  -Dgallium-drivers=r600 \
  -Dplatforms=x11,wayland \
  -Dglx=dri \
  -Dllvm=disabled \
  -Dbuildtype=debugoptimized \
  -Dwerror=false
ninja -C builddir-terakan
```

Do not install Terakan system-wide for testing. Use the development ICD
generated in the build directory:

```sh
VK_DRIVER_FILES="$PWD/builddir-terakan/src/amd/terascale/vulkan/terascale_devenv_icd.$(uname -m).json" \
  vulkaninfo --summary
```

For an application:

```sh
VK_DRIVER_FILES=/path/to/terascale_devenv_icd.ppc64.json \
  DISPLAY=:0 some-vulkan-app
```

## Testing

### OpenGL / X11 color correctness

The canonical regression test for the X11 presentation path is the GLX color
grid:

```sh
DISPLAY=:0 /tmp/glx_grid &   # 4x4 grid, window at 100,100, 400x300
sleep 1.5
DISPLAY=:0 import -window root /tmp/glx-grid.png
```

Compare the on-screen pixels with the `glReadPixels` output printed by the
program. On a correct big-endian presentation, the screen must show exactly
the same colors as the readback (after accounting for the vertical flip).

Also take an XFCE screenshot and verify that the wallpaper and icons have the
expected colors. A useful quick check: in `xfce-shapes.svg`, the mouse is cyan
(around `(0,170,222)`), not red.

### Terakan / Vulkan

Start with device enumeration, then presentation, then compute and rendering:

```sh
VK_DRIVER_FILES=... vulkaninfo --summary
VK_DRIVER_FILES=... DISPLAY=:0 vkcube
```

Useful follow-ups: `vkcube --c 10000`, a trivial compute shader, buffer/image
copies, queries, then real applications (vkQuake3, DXVK-Sarek). Terakan is
experimental; GPU hangs are possible. Keep a reboot path and watch `dmesg`.

Do not run Vulkan tests while the user is in the middle of a game or other
GPU-heavy work.

## Big-endian invariants and known fixes

These are hard-won rules; violating them has produced real color swaps and
rendering artifacts.

### 1. X11 DRI 8888 formats

On big-endian, X11 depth-24/32 visuals use the native
`X8R8G8B8/A8R8G8B8/X8B8G8R8/A8B8G8R8` layouts. In Mesa these are the endian
aliases `BGRX8888/BGRA8888/RGBX8888/RGBA8888`, not the concrete
`B8G8R8X8/B8G8R8A8/R8G8B8X8/R8G8B8A8` formats. Use the aliases in
`dri2_format_table` and keep the big-endian reverse mappings in
`loader_pipe_format_to_fourcc()`. The concrete formats cause
`(R,G,B) -> (G,R,255)` presentation on big-endian. This is what commit
`eff168a504c` fixes.

### 2. r600g packed formats

On big-endian, Mesa's `util_format_description` reverses the channel order of
packed formats (565, 4444, 5551, 2101010, etc.). The hardware reads the word
after the 8IN16/8IN32 swap as a little-endian word, so r600g must use
`r600_packed_le_view()` in the format translation paths. Formats made of whole
bytes (8888) are already in memory order and must not be reversed. Depth/stencil
formats keep fixed component roles on both endiannesses.

### 3. Shader bytecode

The GPU consumes little-endian shader instruction words. Upload shader
bytecode with `util_memcpy_cpu_to_le32()` (or an equivalent conversion), never
with a plain `memcpy` of native `uint32_t` words. Terakan already does this in
its pipeline upload paths.

### 4. Constant buffers / UBOs

The r600 ALU constant cache (KC0/KC1) reads raw little-endian data. On
big-endian, user constant buffers must be pre-swapped to little-endian at
upload time (`r600_buffer_subdata`, `r600_set_constant_buffer`), and the
corresponding resource descriptors must use `ENDIAN_NONE` after the pre-swap.
Do not pre-swap and then also set `ENDIAN_8IN32`; that double-swaps.

### 5. Queries and fences

GPU-written query results and trace counters are little-endian dwords. Read
them with `util_le32_to_cpu()`/`util_bswap32()` on big-endian.

### 6. Vertex and index data

Set `VGT_DMA_SWAP_16_BIT` / `VGT_DMA_SWAP_32_BIT` for big-endian index and
vertex fetch paths as appropriate. `TERASCALE_ENDIAN_SWAP_*` in resource
descriptors does not replace the VGT DMA swap.

### 7. Terakan resource descriptors

Terakan has explicit big-endian handling in:

- `src/amd/terascale/common/terascale_format.c` —
  `terascale_format_big_endian_swap[]`.
- `src/amd/terascale/vulkan/terakan_format.c`,
  `terakan_image.c`, `terakan_vk_buffer.c`,
  `terakan_vk_descriptor_set.c` — descriptor endian fields.
- `src/amd/terascale/vulkan/terakan_vertex_input.c` — VGT DMA swap and shader
  program upload.
- `src/amd/terascale/vulkan/meta/` — clear/query meta shaders and
  descriptors.
- `src/amd/terascale/vulkan/terakan_device.c` — internal meta shaders.

Verify these paths before adding new format or descriptor code.

### 8. Command buffers and CP DMA

r600g submits native packet dwords through the radeon DRM command stream and
works on big-endian. Keep Terakan's submission path consistent with r600g
instead of adding ad-hoc per-dword swaps. However, `terakan_cp_dma.c` has an
open question about byte order for CP DMA payloads; validate a buffer-to-buffer
copy before relying on it.

## Terakan integration status and plan

### Why it is feasible

- Terakan already contains deliberate big-endian code in formats, image
  descriptors, vertex input, queries, meta shaders and shader upload.
- It reuses the r600g SFN shader compiler and register headers, so the
  big-endian work already done for r600g is directly relevant.
- It supports Evergreen CAICOS (the test machine's GPU) through the DRM radeon
  platform, which is the same kernel driver used by r600g.
- The upstream branch builds against Mesa 27.0-devel with `amd_terascale` as a
  Vulkan driver option.

### Main risks

- The local tree is Mesa 26.2.3; the Terakan branch is main@2026-10-24 plus
  339 `src/amd/terascale` commits. The branches diverged at the 26.2 branch
  point (2026-07-03): 6108 commits on the Terakan side, 554 on the 26.2.3
  side. Merging Terakan into 26.2.3 would pull in a large amount of unrelated
  churn.
- Terakan is experimental and not Vulkan-conformant; it uses the software WSI
  path because implicit-sync presentation was removed from the common WSI.
- Its big-endian paths have not been validated on this hardware. Expect
  descriptor/format/CS issues.
- The user cannot test little-endian at runtime, so little-endian support must
  be kept by construction and compile-tested on x86_64.

### Recommended strategy

Prefer **rebasing the big-endian work onto upstream Terakan**, rather than
merging 27.0 into 26.2.3:

1. Create a worktree from `refs/remotes/triang3l/Terakan`.
2. Cherry-pick the local r600g big-endian commits (`aac6ae1835d`,
   `452b7099404`, and the earlier BE commits from the `terakan` branch).
3. Cherry-pick or re-apply `eff168a504c` (the X11 DRI fix). The Terakan
   branch still contains the concrete-format change from `c5b79163126`
   (backported to 26.2.3 as `74bfbe63b7a`), which breaks big-endian X11
   presentation.
4. Resolve conflicts in shared code (`src/gallium/drivers/r600`,
   `src/amd/terascale`, `src/loader`, `src/gallium/frontends/dri`).
5. Build and test r600g first, then Terakan.

If a 27.0 base is unacceptable, the alternative is to vendor only
`src/amd/terascale` into 26.2.3 and adapt it to the older Vulkan runtime API.
This is likely more work because the driver uses `vk_command_buffer`,
`vk_sync`, `vk_wsi` and other runtime interfaces that changed between 26.2 and
27.0.

### Measured port size and conflicts (2026-10-27)

The local big-endian series is small: 664 insertions / 148 deletions in 20
source files between `upstream-mesa-26.2.3` and `terakan` (excluding
AGENTS.md). Each commit was test-cherry-picked onto
`refs/remotes/triang3l/Terakan` (`e50b6665620`):

- clean: `e0182b2cc6a` (`r600_state_common.c`), `4936886b9c0`
  (`r600_shader.c`, xash3d stripes).
- `eff168a504c` (X11 DRI fix): `dri_helpers.c` and `loader_dri_helper.h`
  apply cleanly; `loader_dri_helper.c` conflicts only because upstream gained
  formats in the table — re-add the big-endian `switch` at the top of
  `loader_pipe_format_to_fourcc()`.
- conflicts caused by upstream refactors since the 2026-07-03 branch point
  (mechanical: same logic, new surrounding code):
  - `r600_blit.c` — `cc5d33c1e78`, `59f39a7ffaa`, `c279dc134b5`
  - `r600_state_common.c` — `938fff11684`, `fe9b965fd54`, `452b7099404`
  - `sfn/sfn_instr_mem.cpp` — `aac6ae1835d`
- every commit that touched the old local `AGENTS.md` conflicts on that file;
  drop those hunks and keep this document.

### Validated so far

- **x86_64 (LE) compile test** — `meson setup` with
  `-Dvulkan-drivers=amd_terascale -Dgallium-drivers=r600 -Dplatforms=x11
  -Dglx=dri -Dllvm=disabled` builds cleanly (1273 targets, no errors) and
  produces `libvulkan_terascale.so` plus
  `terascale_devenv_icd.x86_64.json`. Running `vulkaninfo --summary` with that
  ICD loads the driver and reports zero physical devices, which is correct:
  the LE development host has no TeraScale GPU.
- **ppc64 (BE) compile test** — same configuration; see the bring-up notes in
  the session that added this file.
- **Runtime on CAICOS** — not yet validated for Terakan; r600g is the only
  stack tested on the big-endian machine so far.

### Bring-up order

1. **Compile** Terakan on ppc64 with `amd_terascale` enabled.
2. **Enumerate** the CAICOS device with `vulkaninfo --summary`; check the
   reported device name, API version and memory heaps.
3. **Present** a `vkcube` window; verify colors and page flips/copies.
4. **Compute and copy**: buffer/image copies, fills, queries, then a simple
   compute shader.
5. **Draw**: triangle, textured quad, then a real application.
6. **Port/validate r600g big-endian fixes** in the Terakan base as issues
   appear.
7. **Little-endian regression**: build on x86_64; do not claim runtime
   validation.

## Style and commit guidelines

- Follow Mesa style (`clang-format` where configured, 3-space indent, 80-column
  comments).
- Commit messages should state the endianness impact and the hardware/test
  coverage. Use `Fixes:` tags when a specific commit regressed.
- Do not mix Terakan integration and r600g fixes in one commit.
- Keep AGENTS.md current when a major branch, build option or test procedure
  changes.
