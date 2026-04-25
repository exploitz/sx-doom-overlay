// blit.hpp — palette → RGBA4444 → integer-scale upscaler for sx-doom-overlay
//
// doomgeneric writes 320×200 8-bit indexed pixels into DG_ScreenBuffer (when
// built with CMAP256). libtesla's overlay framebuffer expects RGBA4444 at
// runtime-configurable dimensions — typically 320×200 (scale=1), 640×400
// (scale=2), or 960×600 (scale=3). This module is the seam.
//
// Performance: scalar reference implementation. Doom's 35 Hz × 320×200 base
// resolution × scale² = at most 20 megapixels/sec at scale=3. Trivial for
// Tegra X1's NEON-capable A57; NEON intrinsics are a future stretch goal.
//
// Palette switching (damage flash, powerup tint, end-game fade): doomgeneric's
// I_SetPalette writes new RGB values into `extern struct color colors[256]`
// (lib/doomgeneric/doomgeneric/i_video.h:171) and sets `extern bool
// palette_changed` (line 170). This module exposes rebuild_palette_lut to
// pick those up at frame boundaries.
//
// Compiled under -fno-exceptions -fno-rtti. Pure scalar C++. No allocs after
// initial palette setup.
//
// Licensed under GPLv2.

#pragma once

#include <cstdint>
#include <cstddef>

namespace doom_blit {

// 256-entry RGBA4444 palette LUT. Each entry is a 16-bit value with
// 4 bits per channel: 0xRGBA where R is bits 12-15, G 8-11, B 4-7, A 0-3.
// Alpha is always 0xF (opaque) for Doom output.
using PaletteLut = std::uint16_t[256];

// Convert an RGB888 palette (256 entries × 3 bytes) to an RGBA4444 LUT.
// Truncates each 8-bit channel to 4 bits via right-shift by 4.
//   in_rgb888: 768-byte buffer (R, G, B, R, G, B, ...)
//   out_lut:   destination 256-entry RGBA4444 LUT
void build_palette_lut(const std::uint8_t* in_rgb888, PaletteLut& out_lut);

// Convert a `struct color colors[256]` array (doomgeneric's gamma-corrected
// internal palette, ARGB-byte struct from i_video.c) into an RGBA4444 LUT.
// Equivalent to build_palette_lut but reads doomgeneric's struct layout
// directly. The struct has fields {a, r, g, b} per i_video.c:80-84.
//
//   in_colors_argb: 1024-byte buffer (a, r, g, b, ...) — 256 × 4 bytes
//   out_lut:        destination LUT
void build_palette_lut_from_argb_struct(const std::uint8_t* in_colors_argb,
                                        PaletteLut& out_lut);

// Blit an 8-bit indexed source into an RGBA4444 destination at integer scale.
//
//   src:     source buffer, src_w × src_h bytes, palette indices
//   src_w:   source width in pixels (e.g., 320)
//   src_h:   source height in pixels (e.g., 200)
//   lut:     active 256-entry RGBA4444 palette LUT
//   dst:     destination buffer, must hold (src_w * scale) * (src_h * scale)
//            uint16_t values
//   scale:   integer scale factor; valid values 1, 2, 3
//
// Each source pixel is replicated into a scale×scale block in dst. No
// filtering, no rounding — pure integer nearest-neighbor.
void blit_indexed_to_rgba4444(const std::uint8_t* src,
                              std::size_t src_w,
                              std::size_t src_h,
                              const PaletteLut& lut,
                              std::uint16_t* dst,
                              int scale);

}  // namespace doom_blit
