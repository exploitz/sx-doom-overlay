// doomgeneric_switch.c — libnx platform shim for doomgeneric.
//
// Implements the six functions doomgeneric expects (declared in
// lib/doomgeneric/doomgeneric/doomgeneric.h:38-43) plus the longjmp
// recovery target referenced by patches/0002 (i_system.c's patched
// exit() sites longjmp to g_doom_error_jmp instead of terminating).
//
// Threading model (Plan v2 §Architecture):
//   - Engine tick + render is single-threaded inside libtesla's
//     update()/draw() callbacks. DG_DrawFrame is a no-op here; the
//     overlay's DoomElement::draw blits DG_ScreenBuffer into libtesla's
//     framebuffer directly (UltraGB pattern).
//   - Audio runs on a separate libnx Thread (Task 9 wires it up).
//
// Key queue is filled by DoomGui::handleInput on the main thread.
// DG_GetKey pulls from it — single-producer / single-consumer; no lock
// needed because both ends run on the libtesla event-loop thread.
//
// Time source: ARM Generic Timer via armGetSystemTick (19.2 MHz on
// Tegra X1, cf. <switch/arm/counter.h>).
//
// Licensed under GPLv2.

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include <switch.h>

// doomgeneric / doomkeys headers — available because Makefile adds
// lib/doomgeneric/doomgeneric to INCLUDES.
#include "doomgeneric.h"
#include "doomkeys.h"

// ---------------------------------------------------------------------
// Longjmp recovery target — referenced by patches/0002 in i_system.c
// ---------------------------------------------------------------------
//
// I_Error / I_Quit etc. used to call exit() which would terminate the
// entire nx-ovlloader sysmodule. Patch 0002 replaced those with
// longjmp(g_doom_error_jmp, code). DoomGui::initServices() in main.cpp
// runs setjmp(g_doom_error_jmp) before doomgeneric_Create(); a non-zero
// return means the engine errored and the shim should report + recover.
jmp_buf g_doom_error_jmp;

// ---------------------------------------------------------------------
// Tick clock
// ---------------------------------------------------------------------

static u64 s_tick_anchor = 0;

uint32_t DG_GetTicksMs(void) {
    if (s_tick_anchor == 0) {
        s_tick_anchor = armGetSystemTick();
    }
    u64 elapsed_ticks = armGetSystemTick() - s_tick_anchor;
    // Tegra X1 system tick = 19.2 MHz. ms = ticks / 19200.
    return (uint32_t)(elapsed_ticks / 19200ULL);
}

void DG_SleepMs(uint32_t ms) {
    svcSleepThread((u64)ms * 1000000ULL);  // ms → ns
}

// Re-anchor the clock — called by DoomGui::onShow() after dismissal so
// the engine doesn't see a giant time jump from being paused.
void doomgeneric_switch_reanchor_clock(void) {
    s_tick_anchor = armGetSystemTick();
}

// ---------------------------------------------------------------------
// Key queue
// ---------------------------------------------------------------------
//
// DoomGui::handleInput pushes synthesized key events here. The engine
// pulls them via DG_GetKey. Vanilla doomgeneric pattern (cf. its
// platform shim _soso.c, _xlib.c).

#define KEYQUEUE_SIZE 32

static struct {
    uint8_t pressed;  // 1 = down, 0 = up
    uint8_t key;      // doomkeys.h KEY_*
} s_key_queue[KEYQUEUE_SIZE];

static unsigned s_kq_write = 0;
static unsigned s_kq_read  = 0;

// Called from DoomGui::handleInput in main.cpp.
void doomgeneric_switch_push_key(int pressed, unsigned char key) {
    unsigned next = (s_kq_write + 1) % KEYQUEUE_SIZE;
    if (next == s_kq_read) {
        // Queue full — drop the oldest event so newest input is preserved.
        s_kq_read = (s_kq_read + 1) % KEYQUEUE_SIZE;
    }
    s_key_queue[s_kq_write].pressed = pressed ? 1 : 0;
    s_key_queue[s_kq_write].key     = key;
    s_kq_write = next;
}

int DG_GetKey(int* pressed, unsigned char* key) {
    if (s_kq_read == s_kq_write) return 0;  // queue empty
    *pressed = s_key_queue[s_kq_read].pressed;
    *key     = s_key_queue[s_kq_read].key;
    s_kq_read = (s_kq_read + 1) % KEYQUEUE_SIZE;
    return 1;
}

// ---------------------------------------------------------------------
// Frame / window stubs
// ---------------------------------------------------------------------
//
// DG_DrawFrame is a no-op in our single-threaded model. The overlay's
// DoomElement::draw runs once per libtesla composite and reads
// DG_ScreenBuffer directly to perform the palette LUT + scale blit
// into libtesla's framebuffer. doomgeneric_Tick still calls
// DG_DrawFrame at the end of each frame — we just don't need to do
// anything here because the actual display is driven by the libtesla
// callback cadence, not the engine's tick rate.

void DG_Init(void) {
    // Engine has already allocated DG_ScreenBuffer (see lib/doomgeneric/
    // doomgeneric/doomgeneric.c:21). Nothing else to set up here.
}

void DG_DrawFrame(void) {
    // Intentionally no-op. See comment block above.
}

void DG_SetWindowTitle(const char* title) {
    (void)title;  // Switch overlay has no window title
}
