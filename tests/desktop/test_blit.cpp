// test_blit.cpp — Task 3 unit tests for source/blit.cpp.
//
// Hand-rolled test harness (no gtest dependency). Each test is a function
// that returns int (0 == pass, non-zero == fail). main() runs them all and
// reports.
//
// Tests cover:
//   1. pack_rgba4444 math via build_palette_lut on synthetic RGB888 input
//   2. ARGB-struct palette conversion (mirrors doomgeneric's colors[256])
//   3. blit at scale=1 — pure palette indirection, identity geometry
//   4. blit at scale=2 — every source pixel becomes a 2x2 block
//   5. blit at scale=3 — every source pixel becomes a 3x3 block
//   6. palette switching: changing the LUT changes the blitted output
//
// Licensed under GPLv2.

#include "../../source/blit.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using doom_blit::PaletteLut;

namespace {

int g_failures = 0;

void fail(const char* test_name, const char* msg) {
    std::fprintf(stderr, "  FAIL [%s]: %s\n", test_name, msg);
    ++g_failures;
}

void pass(const char* test_name) {
    std::fprintf(stderr, "  pass [%s]\n", test_name);
}

#define ASSERT_EQ_HEX(test, actual, expected) do { \
    if ((actual) != (expected)) { \
        char _msg[160]; \
        std::snprintf(_msg, sizeof(_msg), \
                "actual=0x%X expected=0x%X (line %d)", \
                static_cast<unsigned>(actual), \
                static_cast<unsigned>(expected), __LINE__); \
        fail(test, _msg); return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(test, actual, expected) do { \
    if ((actual) != (expected)) { \
        char _msg[160]; \
        std::snprintf(_msg, sizeof(_msg), \
                "actual=%d expected=%d (line %d)", \
                static_cast<int>(actual), \
                static_cast<int>(expected), __LINE__); \
        fail(test, _msg); return 1; \
    } \
} while (0)

// -----------------------------------------------------------------------------

int test_pack_rgba4444_via_lut() {
    const char* T = "pack_rgba4444";

    // RGB888 input: 4 entries, hand-picked to verify nibble truncation.
    // libtesla RGBA4444 layout: r=bits 0-3, g=4-7, b=8-11, a=12-15.
    //   idx 0: pure white (0xFF, 0xFF, 0xFF) -> RGBA4444 = 0xFFFF
    //   idx 1: pure black (0x00, 0x00, 0x00) -> RGBA4444 = 0xF000 (alpha-only in high nibble)
    //   idx 2: pure red   (0xFF, 0x00, 0x00) -> RGBA4444 = 0xF00F (alpha high + red low)
    //   idx 3: half gray  (0x80, 0x80, 0x80) -> RGBA4444 = 0xF888
    std::uint8_t rgb[256 * 3] = {0};
    rgb[0]=0xFF; rgb[1]=0xFF; rgb[2]=0xFF;  // white
    rgb[3]=0x00; rgb[4]=0x00; rgb[5]=0x00;  // black
    rgb[6]=0xFF; rgb[7]=0x00; rgb[8]=0x00;  // red
    rgb[9]=0x80; rgb[10]=0x80; rgb[11]=0x80;  // gray

    PaletteLut lut;
    doom_blit::build_palette_lut(rgb, lut);

    ASSERT_EQ_HEX(T, lut[0], 0xFFFF);
    ASSERT_EQ_HEX(T, lut[1], 0xF000);
    ASSERT_EQ_HEX(T, lut[2], 0xF00F);
    ASSERT_EQ_HEX(T, lut[3], 0xF888);

    pass(T);
    return 0;
}

int test_argb_struct_conversion() {
    const char* T = "argb_struct_conversion";

    // 1024 bytes (256 × {b, g, r, a}). Layout matches doomgeneric's
    // `struct color colors[256]` — bit-fields b:8, g:8, r:8, a:8 in
    // i_video.h:141-145, which on little-endian = BGRA byte order.
    std::uint8_t argb[256 * 4] = {0};
    // idx 0: b=0xFF, g=0xFF, r=0xFF, a=0 (white, alpha ignored)
    argb[0]=0xFF; argb[1]=0xFF; argb[2]=0xFF; argb[3]=0x00;
    // idx 1: b=0xC0, g=0x80, r=0x10, a=0 (verifies channel ordering)
    argb[4]=0xC0; argb[5]=0x80; argb[6]=0x10; argb[7]=0x00;

    PaletteLut lut;
    doom_blit::build_palette_lut_from_argb_struct(argb, lut);

    ASSERT_EQ_HEX(T, lut[0], 0xFFFF);
    // r=0x10>>4=1 (bits 0-3); g=0x80>>4=8 (bits 4-7);
    // b=0xC0>>4=C (bits 8-11); alpha=F (bits 12-15). Packed: 0xFC81.
    ASSERT_EQ_HEX(T, lut[1], 0xFC81);

    pass(T);
    return 0;
}

int test_blit_scale_1() {
    const char* T = "blit_scale_1";

    // Build a known LUT so we can predict outputs.
    std::uint8_t rgb[256 * 3] = {0};
    rgb[0*3+0]=0xFF; rgb[0*3+1]=0xFF; rgb[0*3+2]=0xFF;  // idx 0 -> white
    rgb[1*3+0]=0xFF; rgb[1*3+1]=0x00; rgb[1*3+2]=0x00;  // idx 1 -> red
    rgb[2*3+0]=0x00; rgb[2*3+1]=0xFF; rgb[2*3+2]=0x00;  // idx 2 -> green
    PaletteLut lut;
    doom_blit::build_palette_lut(rgb, lut);

    // 2x2 source: { 0, 1, 2, 0 }
    std::uint8_t src[4] = { 0, 1, 2, 0 };
    std::uint16_t dst[4] = { 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
    doom_blit::blit_indexed_to_rgba4444(src, 2, 2, lut, dst, 1);

    ASSERT_EQ_HEX(T, dst[0], 0xFFFF);
    ASSERT_EQ_HEX(T, dst[1], 0xF00F);
    ASSERT_EQ_HEX(T, dst[2], 0xF0F0);
    ASSERT_EQ_HEX(T, dst[3], 0xFFFF);

    pass(T);
    return 0;
}

int test_blit_scale_2() {
    const char* T = "blit_scale_2";

    std::uint8_t rgb[256 * 3] = {0};
    rgb[0*3+0]=0xFF; rgb[0*3+1]=0x00; rgb[0*3+2]=0x00;  // idx 0 -> red
    rgb[1*3+0]=0x00; rgb[1*3+1]=0xFF; rgb[1*3+2]=0x00;  // idx 1 -> green
    PaletteLut lut;
    doom_blit::build_palette_lut(rgb, lut);

    // 1x1 source { 0 } -> 2x2 dst all red
    {
        std::uint8_t src[1] = { 0 };
        std::uint16_t dst[4] = { 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
        doom_blit::blit_indexed_to_rgba4444(src, 1, 1, lut, dst, 2);
        for (int i = 0; i < 4; ++i) ASSERT_EQ_HEX(T, dst[i], 0xF00F);
    }

    // 2x1 source { 0, 1 } -> 4x2 dst
    //   row0: red red green green
    //   row1: red red green green
    {
        std::uint8_t src[2] = { 0, 1 };
        std::uint16_t dst[8] = {0};
        doom_blit::blit_indexed_to_rgba4444(src, 2, 1, lut, dst, 2);
        ASSERT_EQ_HEX(T, dst[0], 0xF00F); ASSERT_EQ_HEX(T, dst[1], 0xF00F);
        ASSERT_EQ_HEX(T, dst[2], 0xF0F0); ASSERT_EQ_HEX(T, dst[3], 0xF0F0);
        ASSERT_EQ_HEX(T, dst[4], 0xF00F); ASSERT_EQ_HEX(T, dst[5], 0xF00F);
        ASSERT_EQ_HEX(T, dst[6], 0xF0F0); ASSERT_EQ_HEX(T, dst[7], 0xF0F0);
    }

    pass(T);
    return 0;
}

int test_blit_scale_3() {
    const char* T = "blit_scale_3";

    std::uint8_t rgb[256 * 3] = {0};
    rgb[0*3+0]=0xFF; rgb[0*3+1]=0xFF; rgb[0*3+2]=0xFF;  // idx 0 -> white
    PaletteLut lut;
    doom_blit::build_palette_lut(rgb, lut);

    // 1x1 source { 0 } at scale=3 -> 9 white pixels
    std::uint8_t src[1] = { 0 };
    std::uint16_t dst[9] = {0};
    doom_blit::blit_indexed_to_rgba4444(src, 1, 1, lut, dst, 3);
    for (int i = 0; i < 9; ++i) ASSERT_EQ_HEX(T, dst[i], 0xFFFF);

    pass(T);
    return 0;
}

int test_palette_switching() {
    const char* T = "palette_switching";

    // Same source, two different palettes — output should differ.
    std::uint8_t rgb_a[256 * 3] = {0};
    rgb_a[0*3+0]=0xFF; rgb_a[0*3+1]=0xFF; rgb_a[0*3+2]=0xFF;  // white

    std::uint8_t rgb_b[256 * 3] = {0};
    rgb_b[0*3+0]=0xFF; rgb_b[0*3+1]=0x00; rgb_b[0*3+2]=0x00;  // red (damage flash)

    PaletteLut lut_a, lut_b;
    doom_blit::build_palette_lut(rgb_a, lut_a);
    doom_blit::build_palette_lut(rgb_b, lut_b);

    std::uint8_t src[4] = { 0, 0, 0, 0 };
    std::uint16_t dst_a[4] = {0};
    std::uint16_t dst_b[4] = {0};

    doom_blit::blit_indexed_to_rgba4444(src, 2, 2, lut_a, dst_a, 1);
    doom_blit::blit_indexed_to_rgba4444(src, 2, 2, lut_b, dst_b, 1);

    // Output A should be all white; output B should be all red. They MUST differ.
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ_HEX(T, dst_a[i], 0xFFFF);
        ASSERT_EQ_HEX(T, dst_b[i], 0xF00F);
    }
    if (std::memcmp(dst_a, dst_b, sizeof(dst_a)) == 0) {
        fail(T, "outputs are identical between palette A and palette B");
        return 1;
    }

    pass(T);
    return 0;
}

int test_scale_invalid() {
    const char* T = "blit_scale_invalid";

    // scale=0 or scale=4 — out of supported range. Should not segfault.
    PaletteLut lut = {0};
    std::uint8_t src[1] = { 0 };
    std::uint16_t dst[16] = { 0xDEAD };

    doom_blit::blit_indexed_to_rgba4444(src, 1, 1, lut, dst, 0);
    // Expect either no-op or single black pixel — not undefined behavior.
    // We don't assert specific output for this safety case; just survival.
    doom_blit::blit_indexed_to_rgba4444(src, 1, 1, lut, dst, 4);

    pass(T);
    return 0;
}

}  // namespace

int main() {
    std::fprintf(stderr, "test_blit — sx-doom-overlay\n");
    int result = 0;
    result |= test_pack_rgba4444_via_lut();
    result |= test_argb_struct_conversion();
    result |= test_blit_scale_1();
    result |= test_blit_scale_2();
    result |= test_blit_scale_3();
    result |= test_palette_switching();
    result |= test_scale_invalid();

    std::fprintf(stderr, "\n");
    if (g_failures == 0) {
        std::fprintf(stderr, "OK — all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "FAILURES: %d\n", g_failures);
    return 1;
}
