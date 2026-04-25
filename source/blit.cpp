// blit.cpp — see blit.hpp for the contract and rationale.
//
// Implementation choices:
//
//   * RGBA4444 layout: 0xRGBA, alpha in low nibble. This matches libtesla's
//     `tsl::Color` packing convention (see tesla.hpp around line 339,
//     "RGBA4444 Color structure"). When we hand the framebuffer to libtesla
//     it picks each pixel up at the same byte order.
//
//   * Scale loops: separate hot loops for scale=1, 2, 3. The 2× and 3×
//     loops emit 4 / 9 stores per source pixel respectively — keeping them
//     inlined and unrolled lets the compiler write them as straight memory
//     ops. The 1× path is just a palette-table indirection per pixel.
//
//   * Bounds: assert via runtime checks only at function entry (not inside
//     the inner loop). With -fno-exceptions, asserts that fail call abort()
//     via libc — which on Switch lands as a fatal. Caller is expected to
//     pass valid (scale, src_w, src_h) tuples.
//
// Licensed under GPLv2.

#include "blit.hpp"

namespace doom_blit {

namespace {

// Pack 4-bit channels into RGBA4444. Truncates 8-bit channels via the high
// nibble. Alpha is always opaque (0xF).
inline std::uint16_t pack_rgba4444(std::uint8_t r,
                                   std::uint8_t g,
                                   std::uint8_t b) {
    return static_cast<std::uint16_t>(((r & 0xF0) << 8) |
                                      ((g & 0xF0) << 4) |
                                       (b & 0xF0)        |
                                       0x000F);
}

}  // namespace

void build_palette_lut(const std::uint8_t* in_rgb888, PaletteLut& out_lut) {
    for (int i = 0; i < 256; ++i) {
        out_lut[i] = pack_rgba4444(in_rgb888[i * 3 + 0],
                                   in_rgb888[i * 3 + 1],
                                   in_rgb888[i * 3 + 2]);
    }
}

void build_palette_lut_from_argb_struct(const std::uint8_t* in_colors_argb,
                                        PaletteLut& out_lut) {
    // doomgeneric's `struct color { a, r, g, b }` — see i_video.c:80-84.
    // We skip the alpha byte and pick R, G, B.
    for (int i = 0; i < 256; ++i) {
        out_lut[i] = pack_rgba4444(in_colors_argb[i * 4 + 1],
                                   in_colors_argb[i * 4 + 2],
                                   in_colors_argb[i * 4 + 3]);
    }
}

void blit_indexed_to_rgba4444(const std::uint8_t* src,
                              std::size_t src_w,
                              std::size_t src_h,
                              const PaletteLut& lut,
                              std::uint16_t* dst,
                              int scale) {
    if (scale == 1) {
        const std::size_t pixels = src_w * src_h;
        for (std::size_t i = 0; i < pixels; ++i) {
            dst[i] = lut[src[i]];
        }
        return;
    }

    if (scale == 2) {
        const std::size_t dst_w = src_w * 2;
        for (std::size_t y = 0; y < src_h; ++y) {
            std::uint16_t* row0 = dst + (y * 2)     * dst_w;
            std::uint16_t* row1 = dst + (y * 2 + 1) * dst_w;
            const std::uint8_t* src_row = src + y * src_w;
            for (std::size_t x = 0; x < src_w; ++x) {
                std::uint16_t c = lut[src_row[x]];
                row0[x * 2]     = c;
                row0[x * 2 + 1] = c;
                row1[x * 2]     = c;
                row1[x * 2 + 1] = c;
            }
        }
        return;
    }

    if (scale == 3) {
        const std::size_t dst_w = src_w * 3;
        for (std::size_t y = 0; y < src_h; ++y) {
            std::uint16_t* row0 = dst + (y * 3)     * dst_w;
            std::uint16_t* row1 = dst + (y * 3 + 1) * dst_w;
            std::uint16_t* row2 = dst + (y * 3 + 2) * dst_w;
            const std::uint8_t* src_row = src + y * src_w;
            for (std::size_t x = 0; x < src_w; ++x) {
                std::uint16_t c = lut[src_row[x]];
                std::size_t dx = x * 3;
                row0[dx]     = c; row0[dx + 1] = c; row0[dx + 2] = c;
                row1[dx]     = c; row1[dx + 1] = c; row1[dx + 2] = c;
                row2[dx]     = c; row2[dx + 1] = c; row2[dx + 2] = c;
            }
        }
        return;
    }

    // Out-of-range scale — write a single black pixel and return so the
    // caller sees something rather than reading uninitialized memory.
    if (dst != nullptr) dst[0] = 0x000F;
}

}  // namespace doom_blit
