# Mesa r600g BE (ppc64) fixes

## Context
Fixing big-endian support in the r600 Gallium driver (R5 230 / Caicos, TeraScale 2). Target: GL 4.6, working DRI3 display on ppc64 BE.

## Build & Test Environment
- **Agent host**: x86_64 — compiles only, never runs piglit.
- **Test target**: ppc64 BE (ELFv2, kernel 7.0.11, 64 KiB pages). User compiles and reports logs.
- **Piglit**: Arch PowerPC piglit-git from https://github.com/Link4Electronics/archpower-packages/raw/refs/heads/main/extrappc/powerpc64/piglit-git-r12277.ea22807ea-1-powerpc64.pkg.tar.zst

## Current Status

### Border color results
`piglit run gpu -t 'bordercolor' -t 'texwrap'`

| Test group | Round 16 | Round 17 | Delta |
|------------|:--------:|:--------:|:-----:|
| `texwrap formats bordercolor` (non-swizzled) | all non-int + 8/16-bit int **PASS**; 32-bit int **FAIL** | same | same |
| `texwrap formats bordercolor-swizzled` | all non-int + 8/16-bit int **PASS**; rgb10_a2ui, all 32-bit int **FAIL** | same | same |
| `texwrap formats-int bordercolor` | 8/16-bit **PASS**; 32-bit **FAIL** | same | same |
| `texwrap formats-int bordercolor-swizzled` | 8/16-bit **PASS**; 32-bit **FAIL** | same | same |
| `!opengl 1.1@texwrap formats bordercolor-swizzled` | rgb4, rgb5, rgb10, rgb10_a2 **FAIL** | rgb4, rgb5 **FAIL**; rgb10, rgb10_a2 **PASS** | **FIXED** #2 |
| `arb_texture_rgb10_a2ui@texwrap formats bordercolor[-swizzled]` | **FAIL** (both) | **PASS** (both) | **FIXED** #1+#2 |
| `ext_texture_integer@texwrap formats bordercolor[-swizzled]` | **FAIL** (alpha/L/I/LA 8/16/32, RGB32, RGBA32) | same | same |
| `texwrap formats-float bordercolor[-swizzled]` | **PASS** | **PASS** | same |
| `texwrap formats-s3tc bordercolor[-swizzled]` | **PASS** | **PASS** | same |
| `arb_depth_buffer_float@texwrap formats bordercolor[-swizzled]` | **PASS** | **PASS** | same |
| `arb_depth_texture@texwrap formats bordercolor[-swizzled]` | **PASS** | **PASS** | same |

## Key technical facts

### `do_endian_swap`
- Always false on LE. Set from `!rtex->db_compatible` — true for non-depth textures on BE.
- Activates `ENDIAN_8IN16`/`ENDIAN_8IN32` byte swap in sampler/CB for packed bitmask formats.
- Does NOT affect DMA (raw byte copy — no swap).

### BE bitmask format reversal
- `u_format_parse.py` reverses channels+swizzles for `is_bitmask() && !is_array()` on BE → `desc->swizzle` describes pre-byte-swap (BE memory) order.
- After GPU byte swap, channels are in LE bit order — BE swizzle is WRONG for r600 HW.
- Fix: apply inverse reversal to restore LE order (X↔W, Y↔Z for 4ch; X↔Z for 3ch; X↔Y for 2ch).
- Guards: `desc->layout == UTIL_FORMAT_LAYOUT_PLAIN` (non-bitmask formats like R11G11B10_FLOAT, compressed formats do NOT get BE reversal).

### Packed float/shared exponent
- `R11G11B10_FLOAT` and `R9G9B9E5_FLOAT` pack functions use `util_cpu_to_le32()` — data is always LE in memory regardless of CPU endianness.
- `ENDIAN_NONE` is correct for these (do NOT add ENDIAN_8IN16/32).

### Compressed textures
- All compressors (S3TC/DXT/BPTC/RGTC) output LE bytes explicitly via byte-by-byte stores. Sampler reads with `ENDIAN_NONE` — correct.
- **Do NOT add ENDIAN_8IN16/32 for FMT_BC1–FMT_BC7** (would break sampling).
- NPOT generatemipmap failures (10 tests) have unexplained B↔G swap — likely CPU fallback issue.
- ETC1/ETC2 miptree failures (26 tests) — piglit KTX loader bug on BE (header declares BE data).

### DMA
- DMA copy is raw byte copy — no byte swap. `do_endian_swap` only affects sampler/CB.

### Array formats (`is_array=true`) and ENDIAN_SWAP
- Formats with `is_array=true` (including all `R8G8B8A8_UNORM`, `R16G16_UNORM`, `R32_FLOAT`, etc.) return `ENDIAN_NONE` in `r600_colorformat_endian_swap` even when `do_endian_swap=true`.
- Rationale: array format channels are individually addressable; byte-swapping per channel does not make sense because each channel is its own scalar element.
- Practical consequence: `GL_RGBA8` texture uploads on BE do NOT get sampler byte-swap correction.
- **App portability pitfall**: Games that store pixels as native `uint32_t` (e.g. `(r << 0) | (g << 8) | (b << 16) | (a << 24)`) and upload with `GL_RGBA + GL_UNSIGNED_BYTE` get wrong colors on BE:
  - The `uint32_t` representation in memory on BE is `[A, B, G, R]` bytes (MSB first), not `[R, G, B, A]`.
  - `GL_UNSIGNED_BYTE` reads memory bytes sequentially → gets ABGR → colors wrong (blue→yellow).
  - `GL_UNSIGNED_INT_8_8_8_8_REV` reads the 32-bit word correctly as R=LSB, A=MSB → gets RGBA on both LE and BE.
  - This is NOT an r600 driver bug — it is standard GL portability (app should use `GL_UNSIGNED_INT_8_8_8_8_REV` or store pixels byte-by-byte).
  - Related: `u_format_parse.py` only reverses channels for `is_bitmask() && !is_array()`. Array formats (`is_array=true`) are NOT reversed on BE because their `desc->swizzle` stays identity.

### `cayman_convert_border_color`
- NOT modified. No scaled integer path. Integer border clamping via `cayman_sint8`/`cayman_sint16` only.

## Fixes applied

### Round 1 (pre-session)
1. **`r600_translate_colorformat/texformat`** — Added `HAS_SIZE(2,10,10,10)` / `HAS_SIZE(1,5,5,5)` for BE-reversed channel sizes.
2. **`r600_pipe.c`** — `prefer_real_buffer_in_constbuf0 = !UTIL_ARCH_BIG_ENDIAN`
3. **`loader_dri_helper.c`** — fourcc BE alias override.

### Round 2
4. **`r600_translate_colorswap`** — Guarded `do_endian_swap` conditions with `desc->layout == PLAIN`.
5. **`r600_translate_texformat`** — Guarded inverse BE reversal with `PLAIN`; L4A4 uses `swizzle_yyyx` on BE.
6. **`evergreen_convert_border_color`** — Integer channel size/type lookup uses `d->swizzle[i]` (format swizzle) instead of `swizzle[i]` (view swizzle) on BE. Fixes `rgb10_a2ui`.

### Round 3
7. **`r600_colorformat_endian_swap`** — Reverted `FMT_5_9_9_9_SHAREDEXP` returning `ENDIAN_8IN32`. Pack uses `util_cpu_to_le32` → `ENDIAN_NONE` is correct.
8. **`evergreen_convert_border_color`** — `is_lai` check uses `d->channel[d->swizzle[0]].size` instead of `d->channel[0].size` on BE (avoids false positive for bitmask formats).
9. **`r600_translate_texformat`** — Removed unused `swizzle_xxxy`.

### Round 4
10. **`r600_colorformat_endian_swap`** — Changed `COLOR_8_24`, `COLOR_24_8`, `COLOR_32_FLOAT` from `ENDIAN_8IN16` to `ENDIAN_8IN32` on BE. 8+24/24+8 pack stores depth in 3 bytes across dword boundary — `ENDIAN_8IN16` corrupts; `ENDIAN_8IN32` correctly reverses all 4 bytes.

### Round 5
11. **`r600_colorformat_endian_swap`** — Moved `COLOR_32` (FMT_32) from `ENDIAN_8IN16` to `ENDIAN_8IN32`. 32-bit single-channel formats need full dword swap. Fixes `arb_texture_rg@texwrap formats bordercolor`.
12. **`evergreen_convert_border_color`** — Added `PIPE_SWIZZLE_0`/`PIPE_SWIZZLE_1` guard after `d->swizzle[i]` override in integer path.

### Round 6
13. **`evergreen_convert_border_color`** — Neutralize view swizzle on BE for all formats (GPU applies view swizzle to border color registers, baking it into register + GPU applying = double).
14. **`evergreen_convert_border_color`** — Skip 1/2-channel `border_swizzle` handlers on BE.

### Round 7
15. **`evergreen_convert_border_color`** — `adj` computation maps format channel to user component via `border_color_adj_uc()` (matches GL semantics for L/A/LA/I).
16. **`evergreen_convert_border_color`** — Integer BE adj uses `d->swizzle[v]` for channel description lookup (BE reverses channel array indices).

### Round 8
17. **`evergreen_convert_border_color`** — Non-integer BE adj uses identity mapping for non-legacy formats (border color registers map to logical RGBA, not format decoder X/Y/Z). `eff_fmt` inverse-BE-reversal is wrong for border color. Fixes `!opengl 1.1@texwrap formats bordercolor` (R3G3B2→B5G6R5, RGB4, RGB5).

### Round 9 (reverted — did not fix)
18. **`evergreen_convert_border_color`** — Attempted byte-swap of depth float border color (`Z32_FLOAT`, `Z32_FLOAT_S8X24_UINT`) on BE. **Result**: still fails — root cause is NOT an ENDIAN register swap for border color. The issue is in the depth pipeline data path, not the border color register setup.

### Round 10
19. **`r600_colorformat_endian_swap`** — Gated ALL unconditional `ENDIAN_8IN16`/`ENDIAN_8IN32` returns on `do_endian_swap`. Previously, 32/64/128-bit format cases ignored `do_endian_swap`, so depth textures (`db_compatible=true`, `do_endian_swap=false`) got byte-swapped even though depth data is already LE in memory. Z16_UNORM (`COLOR_16` case) already checked `do_endian_swap` correctly — fix extends the same logic to all packed/multi-byte formats. Fixes `arb_depth_buffer_float@texwrap formats bordercolor[+swizzled]` and `arb_depth_buffer_float@texwrap formats/offset` — Z32_FLOAT, Z24S8, S8Z24, Z32_S8X24 depth formats.

### Round 11
20. **`evergreen_convert_border_color`** — Gated BE non-integer adj path on `is_lai`. For non-L/AI formats with non-identity view swizzle, the adj path indexes `adj[swizzle[i]]` in format-channel space, which misses pre-swizzled input values for channels beyond `nr_channels`. Falling back to the LE path (`in->f[input_swz[i]]`) handles non-identity swizzles correctly.

### Round 12
21. **`evergreen_convert_border_color`** — Removed `!UTIL_ARCH_BIG_ENDIAN` gate from `border_swizzle` 1/2-channel handlers (line 2822). LE has special handlers for 1/2-channel formats that rearrange register contents to correctly handle non-identity view swizzles. BE was skipping these entirely, causing all 1/2-channel swizzled formats to fail. Fixes `arb_texture_rg@texwrap formats bordercolor-swizzled`, `formats-float bordercolor-swizzled`, `ext_texture_snorm@texwrap formats bordercolor-swizzled`, `ext_texture_compression_rgtc@texwrap formats bordercolor-swizzled`.

### Round 13
22. **`evergreen_convert_border_color`** — Changed `ch0_idx` to always be 0 (removed BE-specific `d->swizzle[0]` lookup). On BE, for 4-channel reversed formats like `rgb10_a2ui`, `d->swizzle[0] = 3` points to the 2-bit alpha channel instead of the 10-bit R channel, incorrectly making `is_lai = true`. This neutralized the view swizzle, breaking swizzled output. Using 0 directly matches LE behavior.

### Round 14 (reverted — did not fix)
23. **`evergreen_convert_border_color`** — Fixed integer BE adj path to write to `out->f[output_swz[i]]` using `adj[input_swz[i]]` instead of `out->f[eff_fmt[swz]]` using `adj[swz]`. The `border_swizzle` handler changes `output_swz` for 1/2-channel formats, but the integer BE adj path was ignoring it, writing to the format-channel-mapped position instead.

### Round 14 (corrected — `revert ch0_idx=0, keep adj[input_swz], fix ci = eff_fmt[v]`)
24. **`evergreen_convert_border_color`** — Reverted Round 13 ch0_idx back to `UTIL_ARCH_BIG_ENDIAN ? d->swizzle[0] : 0`. Fixed integer BE adj path: `ci = eff_fmt[v]` (was `d->swizzle[v]`) for correct LE channel size lookup; write target `adj[input_swz[i]]` (was `adj[swz]`) to respect border_swizzle input_swz rearrangement.

### Round 15
25. **`evergreen_convert_border_color`** — Fixed integer LE path for **array integer formats on BE** (R8I..RG32UI, is_array=true). On BE, the swz redirect at line 2859 (`swz = d->swizzle[input_swz[i]]`) was applied to ALL formats, but for array formats `d->swizzle` is NOT BE-reversed (same as LE). The redirect mapped view-swizzle indices (0/1/2/3) through the format swizzle, producing PIPE_SWIZZLE_0(4)/PIPE_SWIZZLE_1(5) for non-existent channels, which triggered the early continue (writing 0/1) instead of the correct channel lookup. This changed the border color register values, breaking the border_swizzle compensation designed for LE. Fix: skip the redirect + PIPE_SWIZZLE_0/1 checks for `d->is_array && d->layout == PLAIN` formats on BE, letting `swz = swizzle[i]` retain the view-swizzle index for natural channel[] lookup (VOID type → val=0).
26. **`evergreen_convert_border_color`** — Changed `ch0_idx` to always be `0` (removed BE-specific `d->swizzle[0]` lookup). On BE, for 4-channel reversed formats like `rgb10_a2ui`, `d->swizzle[0] = 3` (W=alpha=2bit) made `is_lai = true`, neutralizing the view swizzle and breaking swizzled output. Using `channel[0].size` (R channel) directly matches LE semantics for the legacy-format check.

### Round 16 (this session)
27. **`evergreen_convert_border_color`** — Changed BE non-integer adj path gate from `is_lai` to `is_luminance_or_alpha || util_format_is_intensity(format)`. Small-channel packed bitmask formats (rgb4/rgb5, B4G4R4X4/B5G5R5X1) are `is_lai=true` due to `channel[0].size < 8` but are NOT legacy formats — they have 3-4 channels with their own channel mapping. The adj path's `f = v < nr_channels ? v : d->swizzle[v]` fallback was designed for legacy single-channel expansion and produces wrong values for void/X channels when `d->swizzle[3]` maps to `PIPE_SWIZZLE_1`. Falling through to the LE path (`in->f[input_swz[i]]`) preserves the input values correctly. However, rgb4/rgb5 still fail — root cause is the `is_lai=true` swizzle neutralization preventing the undo of state tracker pre-swizzle, which is identical on LE and BE, so the actual issue is format/DST_SEL interaction with the X padding channel on BE.

### Round 17 (this session)
28. **`evergreen_convert_border_color`** — Fixed `is_lai` check for packed bitmask integer formats on BE (e.g., R10G10B10A2_UINT). On BE, `d->channel[0]` is the FIRST channel after BE reversal (2-bit alpha for R10G10B10A2), making `ch0_size = 2 < 8` and incorrectly setting `is_lai = true`. This neutralizes the view swizzle, preventing the driver from undoing the state tracker's pre-swizzle, which causes a double-swizzle for swizzled views and wrong normalizer constants for non-swizzled views. Fix: use `d->channel[d->swizzle[ch0_idx]].size` on BE for non-array PLAIN formats to get the LE channel[0]'s size.
29. **`evergreen_convert_border_color`** — Fixed channel size lookup in integer BE adj path. `ci = eff_fmt[v]` looked up channel size using the LE swizzle index as a BE channel index, which is wrong because `d->channel[]` is ordered for BE (reversed channel array). Changed to `ci = d->swizzle[v]` which correctly maps BE position `v` to the right channel index. Fixes both non-swizzled and swizzled border colors for packed bitmask integer formats (rgb10_a2ui) and all non-integer 10-bit packed formats (rgb10/rgb10_a2 UNORM swizzled).

### Round 18 (this session)
30. **`r600_texture.c`** — Fixed Z32_FLOAT_S8X24_UINT stencil readback on BE (zero stencil in glReadPixels). Root cause: CB writes/reads with ENDIAN_8IN32 (byte-swaps dword1) but the stencil component within dword1 ends up at the wrong byte position for BE. After ENDIAN_8IN32, dword1 = [stencil, X, X, X] (stencil at bit[31] of the swapped dword = byte[4] in memory). The unpack_s_8uint reads byte[4..7] as uint32 and truncates to uint8 — on BE this gives `stencil << 24` → 0. Fix: bswap32 the second dword of each pixel in the staging buffer, converting [stencil, X, X, X] to [X, X, X, stencil], so the unpack function reads stencil from byte[7] (LSB of BE uint32) = stencil correctly. Applied in both READ path (map function, after CB write) and WRITE path (unmap function, before CB read). Guarded by `UTIL_ARCH_BIG_ENDIAN` and `blocksize == 8` (catches any 64-bit ZS format).

## Remaining failures analysis

### 32-bit integer precision (2 tests, 44 subtests)
`arb_texture_rg@texwrap formats-int bordercolor` + `ext_texture_integer@texwrap formats bordercolor`.
- R32I/R32UI/RG32I/RG32UI/RGB32I/RGBA32I/RGB32UI/RGBA32UI with values > 2^24 can't round-trip through `float border = val / (2^32-1)`. Likely piglit tolerance issue.

### Integer swizzled (2 tests)
`arb_texture_rg@texwrap formats-int bordercolor-swizzled` (8/16-bit PASS, 32-bit fail: R32I..RGBA32UI), `arb_texture_rgb10_a2ui@texwrap formats bordercolor-swizzled`.
- 8/16-bit array integer formats (R8I..RG16UI, is_array=true) now PASS due to Round 15 fix (skip swz redirect on BE for array formats).
- 32-bit array integer (R32I/R32UI/RG32I/RG32UI, RGB32I/UI, RGBA32I/UI): values > 2^24 can't round-trip through `float border = val / (2^32-1)`. Likely piglit tolerance issue.
- Non-array integer (rgb10_a2ui, 10+10+10+2 packed) uses the BE adj path. Register values match LE — root cause is NOT in the conversion function.
- Both LE and BE produce identical register values for all, yet BE fails. Suspect: border color register byte-swap by `ENDIAN_8IN16`/`ENDIAN_8IN32` also affects the border color registers on BE (they are written via PKT3_SET_CONFIG_REG and might be subject to the same HW auto-swap as config registers). Or: the sampler's format swizzle treatment for border color differs on BE due to different format+endian settings in the texture resource descriptor.

### rgb10/rgb10_a2 swizzled (1 test, 2 subtests)
`!opengl 1.1@texwrap formats bordercolor-swizzled` — `gl_rgb10`, `gl_rgb10_a2`.
- These are UNORM 10-bit packed formats (not legacy). `channel[0].size >= 8`, `nr_channels >= 3`. Not is_lai. The Round 16 gate change does NOT affect them (same code path as before).
- On BE, `is_lai` was incorrectly TRUE (channel[0]=UP2 after BE reversal < 8). Neutralized swizzle prevented the undo of state tracker pre-swizzle. Round 17 fix #2 corrects this.
- **Expected to PASS** once Round 17 fix #2 is applied.

### ext_texture_integer failures
`ext_texture_integer@texwrap formats bordercolor[-swizzled]` — alpha/luminance/intensity/luminance_alpha integer formats.
- These include BOTH 8/16-bit AND 32-bit variants failing.
- alpha/luminance/intensity/LA are legacy 1/2-channel formats with `is_array=true` for the 8/16-bit variants.
- The 32-bit failures are the same precision issue as R32I etc.
- The 8/16-bit failures: likely swz neutralization issue on BE for legacy formats, or border_swizzle handler interaction.

### rgb4/rgb5 swizzled (1 test, 2 subtests) — **STILL FAIL**
`!opengl 1.1@texwrap formats bordercolor-swizzled` — `gl_rgb4`, `gl_rgb5` only.
- Small-channel packed bitmask formats with non-void X channel. `d->swizzle[3]` maps to `PIPE_SWIZZLE_1` causing `adj[3] = 1.0f` instead of the correct input value. Round 16 fix gated the non-integer BE adj path on true legacy status, skipping this for rgb4/rgb5.
- However, is_lai is still TRUE on both LE and BE (channel[0].size = 4 or 5 < 8), so swizzle is neutralized. The state tracker pre-swizzles the border color, but the driver doesn't undo it (because is_lai neutralized the view swizzle). The hardware applies DST_SEL (which contains the view swizzle), causing a double-swizzle. On LE the same neutralization occurs, so this is NOT the root cause — the actual issue is likely in the format swizzle / DST_SEL interaction with the X (padding) channel on BE.

### Misc
- `!opengl 1.1@texwrap formats` / `offset` — basic texwrap (93 pass, 11 fail — many now passing with R10/R11)
- `fbo-generatemipmap-formats` NPOT compressed (10 tests) — B↔G swap in NPOT.
- ETC1/ETC2 miptree (26 tests) — piglit KTX loader bug.
- `depth32f_stencil8` stencil draw/read/blit (12 fail) — stencil always zero. Root cause: `COLOR_X24_8_32_FLOAT` + `ENDIAN_8IN32` moves stencil from byte[7] to byte[4] MSB; unpack reads lowest byte of BE uint32 → 0. Fix: `__builtin_bswap32` on staging dword1. The WRITE path (blitter `R16G16B16A16_UINT` + `ENDIAN_8IN16`) correctly handles stencil from byte[4] without any fix.

### Round 19 (this session)
31. --REPLACED BY Round 19 fix Round 3--
32. --REPLACED BY Round 19 fix Round 3--

### Round 19 fix Round 3 (this session)
33. **`r600_state_common.c:r600_translate_colorformat`** — Added `HAS_SIZE(32,24,8,0)` case alongside `HAS_SIZE(32,8,24,0)`. On BE, `Z32_FLOAT_S8X24_UINT` has channels `[F32(32), X24(24), UP8(8)]` (BE YAML reorder). The LE channel size check `HAS_SIZE(32,8,24,0)` fails because `channel[1].size=24≠8`, causing `~0U` return → format 63 in CB_COLOR_INFO → kernel CS rejection. Fixes all `fbo-depthstencil-gl_depth32f_stencil8-*`, `depth-stencil-blit`, `texsubimage-depth-formats`, and `arb_copy_image` CS rejections.
34. **`r600_state_common.c:r600_translate_colorformat`** — Added direct `PIPE_FORMAT_Z32_FLOAT_S8X24_UINT` / `PIPE_FORMAT_X32_S8X24_UINT` checks with hardcoded `V_0280A0_COLOR_X24_8_32_FLOAT` return, before the `layout != PLAIN` early-return. The `HAS_SIZE(32,24,8,0)` workaround (Round 19 fix Round 3) reads `util_format_channel_description` bitfields (`size:9`, `shift:16`), whose bit-level packing on BE GCC differs from LE. The direct format check bypasses the bitfield struct entirely and is authoritative.
35. **`r600_texture.c:r600_translate_colorswap`** — Guarded 3-channel `do_endian_swap` COMP_SWAP reversal with `desc->colorspace != UTIL_FORMAT_COLORSPACE_ZS`. Depth formats (Z32_FLOAT_S8X24_UINT) have fixed component roles (depth, stencil) across both endiannesses — `STD_REV` would incorrectly swap depth↔stencil dwords after `ENDIAN_8IN32` byte swap. Fixes potential wrong depth/stencil values in CB blit path.
36. **`r600_texture.c:r600_texture_transfer_map`** — Replaced `>>= 16` with `__builtin_bswap32` for READ path 64-bit ZS stencil fixup on BE. The DB→CB decompress uses `COLOR_X24_8_32_FLOAT` + `ENDIAN_8IN32`. The CB format puts stencil at word1 byte[3] (= absolute byte[7]). `ENDIAN_8IN32` byte-swaps the word, moving stencil from byte[7] to byte[4] (MSB of BE uint32). `unpack_s_8uint` reads byte[4] as BE uint32 `0xSS000000` and truncates to uint8 → `0x00`. `__builtin_bswap32` reverses to `0x000000SS` → unpack correctly reads stencil `0xSS`.
37. **`r600_texture.c:r600_texture_transfer_unmap`** — Removed the WRITE path stencil fixup entirely (`<<= 16` was incorrect: `0xSS000000 << 16 = 0` on uint32). `pack_s_8uint` writes stencil at byte[4] (MSB of BE uint32). The blitter reinterprets 64-bit ZS as `R16G16B16A16_UINT` with `ENDIAN_8IN16`. The B channel reads bytes[4..5] = `[SS, 0x00]`, byte-swapped → `[0x00, SS]` → LE uint16 `0x00SS` = stencil. The CB writes stencil to byte[7] of the depth texture (`COLOR_X24_8_32_FLOAT` + `ENDIAN_NONE`). The READ fix (item 36) correctly recovers stencil from byte[7] on readback.

### Round 20 (this session)
38. **`r600_blit.c:r600_generate_mipmap`** — Implemented driver-level `generate_mipmap` for compressed textures on BE. `util_gen_mipmap` returns false for compressed textures (no `PIPE_BIND_RENDER_TARGET` support), forcing CPU fallback via `_mesa_generate_mipmap` → `_mesa_GetTexSubImage_sw`, which writes to an intermediate Mesa internal format buffer with wrong byte order on BE, causing B↔G swap in re-compressed output. Fix: create temporary RGBA8 texture (is_array=true, correct byte order on BE), GPU blit decompress compressed level 0 to temp, GPU mipmap generation on temp via `util_blitter_generate_mipmap`, then CPU re-compress each level via `util_format_translate` (struct-based pack/unpack on RGBA8 is endian-safe; DXT pack functions write bytes explicitly in LE order). Enabled `caps->generate_mipmap = true` in `r600_init_screen_caps` and set `generate_mipmap` function pointer in `r600_init_blit_functions`. Expected to fix `fbo-generatemipmap-formats` NPOT compressed (10 tests with B↔G swap).

## Relevant Files
- `src/gallium/drivers/r600/r600_texture.c`:r600_translate_colorswap (R2: PLAIN guard)
- `src/gallium/drivers/r600/r600_state_common.c`:r600_translate_texformat (R2: BE swizzle fix, L4A4)
- `src/gallium/drivers/r600/r600_state_common.c`:r600_colorformat_endian_swap (R3: FMT_5_9_9_9 revert; R4/5: ENDIAN_8IN32)
- `src/gallium/drivers/r600/r600_state_common.c`:r600_translate_colorformat (R1: BE channel size checks)
- `src/gallium/drivers/r600/evergreen_state.c`:evergreen_convert_border_color (R2–R9: all border color fixes)
- `src/gallium/drivers/r600/evergreen_state.c`:cayman_convert_border_color — NOT modified
- `src/gallium/drivers/r600/evergreen_state.c`:evergreen_dma_copy, evergreen_dma_copy_tile — DMA raw copy
- `src/util/format/u_format_other.c` — R11G11B10/R9G9B9E5 pack (uses `util_cpu_to_le32`)
- `src/util/format/u_format.yaml` — format definitions (BE reversal only for `layout: plain`)
- `src/gallium/drivers/r600/r600_texture.c`:r600_texture_transfer_map, r600_texture_transfer_unmap (R19r3: 64-bit ZS dword1 bswap32 READ, removed WRITE)
- `src/gallium/drivers/r600/r600_blit.c`:r600_generate_mipmap (R20: compressed mipmap GPU override)
- `src/gallium/drivers/r600/r600_pipe.c`:r600_init_screen_caps (R20: caps->generate_mipmap = true)
