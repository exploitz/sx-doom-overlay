// test_sound.c — Task 4 unit tests for source/audio_mixer.c via WAV backend.
//
// Drives the mixer with synthesized inputs (square wave, sine, simultaneous
// channels), pipes the output through the WAV backend, then re-reads the
// resulting WAV and asserts properties of the samples.
//
// Why synthesized inputs and not real Doom DSXXXX lumps:
//   - Real-Doom-lump testing requires the engine to be running, which is
//     Task 9 territory. Task 4 verifies the mixer math, the backend
//     interface, and the volume scaling in isolation.
//   - Synthesized inputs let us assert exact expected outputs (a 100%
//     volume sine produces samples of known amplitude) instead of
//     comparing against fragile golden binaries.
//
// Tests:
//   1. silent: no channels active -> all-zero output
//   2. single_channel_full_volume: square wave -> samples match input
//   3. master_volume_50pct: half master volume -> half amplitude
//   4. multi_channel_no_clip: 3 simultaneous max-amplitude squares -> no clip overflow
//   5. wav_round_trip: write a known buffer, re-read, bytes match
//
// Licensed under GPLv2.

#include "../../source/audio_mixer.h"
#include "../../source/audio_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

static void fail(const char* test_name, const char* msg) {
    fprintf(stderr, "  FAIL [%s]: %s\n", test_name, msg);
    ++g_failures;
}

static void pass(const char* test_name) {
    fprintf(stderr, "  pass [%s]\n", test_name);
}

#define ASSERT_EQ_INT(test, actual, expected) do { \
    if ((int)(actual) != (int)(expected)) { \
        char _msg[128]; \
        snprintf(_msg, sizeof(_msg), "actual=%d expected=%d (line %d)", \
                 (int)(actual), (int)(expected), __LINE__); \
        fail(test, _msg); return 1; \
    } \
} while (0)

#define ASSERT_TRUE(test, cond, why) do { \
    if (!(cond)) { fail(test, why); return 1; } \
} while (0)

// -----------------------------------------------------------------------------

static int test_silent() {
    const char* T = "silent";
    audio_mixer_t m;
    audio_mixer_init(&m);

    int16_t out[256] = {0};
    audio_mixer_mix(&m, out, 128);

    for (int i = 0; i < 256; ++i) {
        if (out[i] != 0) {
            fail(T, "output not all-zero");
            return 1;
        }
    }
    pass(T);
    return 0;
}

static int test_single_channel_full_volume() {
    const char* T = "single_channel_full_volume";
    audio_mixer_t m;
    audio_mixer_init(&m);

    // Square wave: alternating +10000 / -10000 for 16 frames
    int16_t pcm[16];
    for (int i = 0; i < 16; ++i) pcm[i] = (i & 1) ? -10000 : 10000;

    audio_mixer_play(&m, 0, pcm, 16, 255, 255);

    int16_t out[16 * 2] = {0};
    audio_mixer_mix(&m, out, 16);

    // At full volume (255/255 = 1.0 ratio scaled by 255 master/255), we
    // expect output to equal input on both L and R.
    for (int i = 0; i < 16; ++i) {
        ASSERT_EQ_INT(T, out[i*2 + 0], pcm[i]);
        ASSERT_EQ_INT(T, out[i*2 + 1], pcm[i]);
    }
    ASSERT_TRUE(T, !audio_mixer_is_playing(&m, 0), "channel still playing after exhausted");
    pass(T);
    return 0;
}

static int test_master_volume_50pct() {
    const char* T = "master_volume_50pct";
    audio_mixer_t m;
    audio_mixer_init(&m);
    m.master_vol = 128;  // ~50%

    int16_t pcm[8] = { 10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000 };
    audio_mixer_play(&m, 0, pcm, 8, 255, 255);

    int16_t out[16] = {0};
    audio_mixer_mix(&m, out, 8);

    // Expected: 10000 * 255 / 255 * 128 / 255 = 10000 * 128 / 255 = 5019
    int32_t expected = (10000 * 128) / 255;
    for (int i = 0; i < 16; ++i) {
        if (out[i] < expected - 2 || out[i] > expected + 2) {
            char m2[160];
            snprintf(m2, sizeof(m2), "out[%d]=%d not within ±2 of %d", i, out[i], (int)expected);
            fail(T, m2);
            return 1;
        }
    }
    pass(T);
    return 0;
}

static int test_multi_channel_no_clip() {
    const char* T = "multi_channel_no_clip";
    audio_mixer_t m;
    audio_mixer_init(&m);

    // 3 channels, each at moderate amplitude. Sum should not clip.
    int16_t pcm1[8] = { 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000 };
    int16_t pcm2[8] = { 7000, 7000, 7000, 7000, 7000, 7000, 7000, 7000 };
    int16_t pcm3[8] = { 6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000 };
    audio_mixer_play(&m, 0, pcm1, 8, 255, 255);
    audio_mixer_play(&m, 1, pcm2, 8, 255, 255);
    audio_mixer_play(&m, 2, pcm3, 8, 255, 255);

    int16_t out[16] = {0};
    audio_mixer_mix(&m, out, 8);

    // Sum ≈ 8000+7000+6000 = 21000, well under 32767
    for (int i = 0; i < 16; ++i) {
        if (out[i] != 21000) {
            char m2[160];
            snprintf(m2, sizeof(m2), "out[%d]=%d expected 21000", i, out[i]);
            fail(T, m2);
            return 1;
        }
    }
    pass(T);
    return 0;
}

static int test_clipping() {
    const char* T = "clipping";
    audio_mixer_t m;
    audio_mixer_init(&m);

    // 2 max-amplitude channels: 32000 + 32000 = 64000 -> clamped to 32767
    int16_t pcm1[4] = { 32000, 32000, 32000, 32000 };
    int16_t pcm2[4] = { 32000, 32000, 32000, 32000 };
    audio_mixer_play(&m, 0, pcm1, 4, 255, 255);
    audio_mixer_play(&m, 1, pcm2, 4, 255, 255);

    int16_t out[8] = {0};
    audio_mixer_mix(&m, out, 4);

    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ_INT(T, out[i], 32767);
    }
    pass(T);
    return 0;
}

static int test_wav_round_trip() {
    const char* T = "wav_round_trip";

    audio_backend_t* be = NULL;
    audio_backend_status_t s = audio_backend_init("out/test_sound_roundtrip.wav", &be);
    if (s != AUDIO_BACKEND_OK || !be) {
        fail(T, "audio_backend_init failed");
        return 1;
    }

    int16_t pcm[16] = {
        100, -100, 200, -200, 300, -300, 400, -400,
        500, -500, 600, -600, 700, -700, 800, -800
    };
    if (!audio_backend_submit(be, pcm, 8)) {
        fail(T, "submit failed");
        audio_backend_shutdown(be);
        return 1;
    }
    audio_backend_shutdown(be);

    // Re-open the WAV, skip 44-byte header, verify samples.
    FILE* f = fopen("out/test_sound_roundtrip.wav", "rb");
    if (!f) {
        fail(T, "could not reopen wav");
        return 1;
    }
    fseek(f, 44, SEEK_SET);
    int16_t read_back[16] = {0};
    size_t got = fread(read_back, sizeof(int16_t), 16, f);
    fclose(f);
    if (got != 16) {
        fail(T, "fread returned fewer than 16 samples");
        return 1;
    }

    for (int i = 0; i < 16; ++i) {
        ASSERT_EQ_INT(T, read_back[i], pcm[i]);
    }
    pass(T);
    return 0;
}

// -----------------------------------------------------------------------------

int main(void) {
    fprintf(stderr, "test_sound — sx-doom-overlay\n");
    int r = 0;
    r |= test_silent();
    r |= test_single_channel_full_volume();
    r |= test_master_volume_50pct();
    r |= test_multi_channel_no_clip();
    r |= test_clipping();
    r |= test_wav_round_trip();

    fprintf(stderr, "\n");
    if (g_failures == 0) {
        fprintf(stderr, "OK — all tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAILURES: %d\n", g_failures);
    return 1;
}
