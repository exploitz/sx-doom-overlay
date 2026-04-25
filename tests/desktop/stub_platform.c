// Desktop stub platform shim for doomgeneric — Task 2 engine smoke harness.
//
// Implements the 6 DG_* functions doomgeneric expects, plus a main() that
// runs the engine for a configurable number of ticks and dumps every Nth
// frame as a PPM image. No SDL, no GPU, no audio. The point is to validate
// that:
//   1. The MIN_RAM=3 patch (patches/0001-lower-min-ram.patch) is in effect
//      and the engine runs without zone allocator failures.
//   2. The CMAP256 + 320x200 build configuration produces correct frames.
//   3. Freedoom 1's title / demo loop renders without crashing.
//
// Usage:
//   ./stub_platform <iwad-path> [num-ticks] [frame-dump-interval]
//   ./stub_platform ../../data/freedoom1.wad 200 10
//
// Output: out/frame_NNN.ppm files. Use `eog out/frame_000.ppm` or any
// PPM viewer to verify.
//
// Licensed under GPLv2.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>

#include "../../lib/doomgeneric/doomgeneric/doomgeneric.h"
#include "../../lib/doomgeneric/doomgeneric/doomkeys.h"
#include "../../lib/doomgeneric/doomgeneric/i_video.h"

/* sx-doom-overlay: longjmp recovery target referenced by i_system.c.
 * On the real overlay, the shim layer defines this and sets it via setjmp()
 * before invoking doomgeneric_Create. Here in the desktop smoke harness we
 * provide it so the patched engine links and runs. On any error path the
 * engine longjmps here; we report the value and exit cleanly. */
jmp_buf g_doom_error_jmp;

// --- Configuration (set in main from argv) -----------------------------------

static unsigned g_max_ticks = 200;
static unsigned g_frame_interval = 10;
static unsigned g_tick_count = 0;
static unsigned g_frame_count = 0;
static const char* g_out_dir = "out";

// --- Synthetic key queue -----------------------------------------------------
// We feed no input for the smoke test (engine runs attract demo loop).

#define KEY_QUEUE_SIZE 16
static unsigned short s_key_queue[KEY_QUEUE_SIZE];
static unsigned int s_key_q_write = 0;
static unsigned int s_key_q_read = 0;

// --- Tick clock --------------------------------------------------------------

static struct timespec s_t0;

static uint32_t monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(((t.tv_sec - s_t0.tv_sec) * 1000ULL) +
                      ((t.tv_nsec - s_t0.tv_nsec) / 1000000ULL));
}

// --- Palette resolution ------------------------------------------------------
// In CMAP256 mode, doomgeneric writes 8-bit indices to DG_ScreenBuffer.
// The palette comes from the engine's `colors[256]` extern (gamma-corrected,
// see lib/doomgeneric/doomgeneric/i_video.h:171).

static void resolve_index_to_rgb(uint8_t idx, uint8_t* r, uint8_t* g, uint8_t* b) {
    extern struct color colors[256];
    *r = colors[idx].r;
    *g = colors[idx].g;
    *b = colors[idx].b;
}

// --- PPM frame dump ----------------------------------------------------------

static void dump_frame_ppm(unsigned frame_idx) {
    char path[256];
    snprintf(path, sizeof(path), "%s/frame_%03u.ppm", g_out_dir, frame_idx);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[stub] could not open %s for write\n", path);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);

    const uint8_t* pixels = (const uint8_t*)DG_ScreenBuffer;
    uint8_t rgb[3];
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; ++i) {
        resolve_index_to_rgb(pixels[i], &rgb[0], &rgb[1], &rgb[2]);
        fwrite(rgb, 3, 1, f);
    }

    fclose(f);
    fprintf(stderr, "[stub] wrote %s\n", path);
}

// --- DG_* platform shim implementations --------------------------------------

void DG_Init(void) {
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    fprintf(stderr, "[stub] DG_Init — engine smoke harness ready\n");
    fprintf(stderr, "[stub]   resolution: %dx%d (CMAP256)\n",
            DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    fprintf(stderr, "[stub]   max_ticks: %u, frame_interval: %u\n",
            g_max_ticks, g_frame_interval);
}

void DG_DrawFrame(void) {
    g_tick_count++;
    if ((g_tick_count - 1) % g_frame_interval == 0) {
        dump_frame_ppm(g_frame_count++);
    }
    if (g_tick_count >= g_max_ticks) {
        fprintf(stderr, "[stub] reached max_ticks=%u, exiting cleanly\n", g_max_ticks);
        fprintf(stderr, "[stub] dumped %u frames, %u ticks total\n",
                g_frame_count, g_tick_count);
        // doomgeneric has no clean shutdown — exit() is the convention used by
        // every other doomgeneric_*.c platform shim too.
        exit(0);
    }
}

void DG_SleepMs(uint32_t ms) {
    usleep((useconds_t)ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    return monotonic_ms();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    if (s_key_q_read == s_key_q_write) {
        return 0;
    }
    unsigned short ev = s_key_queue[s_key_q_read];
    s_key_q_read = (s_key_q_read + 1) % KEY_QUEUE_SIZE;
    *pressed = (ev >> 8) & 0x1;
    *key = ev & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char* title) {
    fprintf(stderr, "[stub] DG_SetWindowTitle: %s\n", title);
}

// --- main --------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <iwad-path> [num-ticks=200] [frame-interval=10]\n"
                "  iwad-path:      path to freedoom1.wad / doom1.wad\n"
                "  num-ticks:      total ticks to run before exit\n"
                "  frame-interval: dump every Nth tick to out/frame_NNN.ppm\n",
                argv[0]);
        return 1;
    }

    if (argc >= 3) g_max_ticks = (unsigned)atoi(argv[2]);
    if (argc >= 4) g_frame_interval = (unsigned)atoi(argv[3]);

    // Ensure the output directory exists. We rely on `mkdir -p out` in the
    // Makefile target; here we just check.
    {
        FILE* probe = fopen("out/.probe", "w");
        if (!probe) {
            fprintf(stderr, "[stub] WARNING: out/ may not exist. Try `mkdir -p out`.\n");
        } else {
            fclose(probe);
            unlink("out/.probe");
        }
    }

    // doomgeneric_Create wants argv[0] = program, argv[1..] = engine args.
    // Build a synthesized argv: { "doom", "-iwad", iwad, "-mb", "3" }.
    char* engine_argv[] = {
        (char*)"doom",
        (char*)"-iwad", argv[1],
        (char*)"-mb",   (char*)"3",
        NULL
    };
    int engine_argc = 5;

    /* sx-doom-overlay: setjmp checkpoint for engine error recovery.
     * The patched i_system.c uses longjmp(g_doom_error_jmp, code) instead
     * of exit() — non-zero return from setjmp means the engine errored out
     * and we should bail cleanly rather than terminating the whole sysmodule
     * (which is what exit() did pre-patch). */
    int err = setjmp(g_doom_error_jmp);
    if (err != 0) {
        const char* reasons[] = {
            "(no error)",
            "(unused)",
            "clean quit (I_Quit)",
            "recursive I_Error",
            "DJGPP path",
            "I_Error tail (ORIGCODE)",
            "I_Error tail (our build path)"
        };
        const char* reason = (err >= 0 && err < 7) ? reasons[err] : "unknown";
        fprintf(stderr, "[stub] engine longjmp received (code=%d, %s) — exiting cleanly\n",
                err, reason);
        return (err == 2) ? 0 : err;
    }

    fprintf(stderr, "[stub] doomgeneric_Create (-iwad %s -mb 3)\n", argv[1]);
    doomgeneric_Create(engine_argc, engine_argv);

    fprintf(stderr, "[stub] entering tick loop\n");
    while (1) {
        doomgeneric_Tick();  // calls DG_DrawFrame internally; we exit() inside it
    }

    return 0;
}
