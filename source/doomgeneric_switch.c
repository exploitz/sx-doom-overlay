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
#include "doomdef.h"   // weapontype_t enum + NUMWEAPONS
#include "d_player.h"  // player_t struct (readyweapon, weaponowned[])

// players[] is defined in g_game.c, declared in doomstat.h. We re-declare here
// to keep the dependency local; including doomstat.h pulls the world.
extern player_t players[MAXPLAYERS];

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

static u64 s_tick_anchor    = 0;
static u64 s_last_query_raw = 0;  // last raw armGetSystemTick() seen by DG_GetTicksMs

uint32_t DG_GetTicksMs(void) {
    u64 now = armGetSystemTick();
    if (s_tick_anchor == 0) {
        s_tick_anchor = now;
    }
    s_last_query_raw = now;
    u64 elapsed_ticks = now - s_tick_anchor;
    // Tegra X1 system tick = 19.2 MHz. ms = ticks / 19200.
    return (uint32_t)(elapsed_ticks / 19200ULL);
}

void DG_SleepMs(uint32_t ms) {
    svcSleepThread((u64)ms * 1000000ULL);  // ms → ns
}

// Resume-from-dismiss clock recovery — called by DoomGui::onShow().
//
// Engine's d_loop.c keeps a static `lasttime` (line 201) that holds the LAST
// I_GetTime() result. After dismissal, the next call to I_GetTime sees a huge
// jump, which makes `newtics = nowtime - lasttime` enormous and the engine
// either (a) tries to catch up across the entire dismissal duration (multi-
// second freeze) or (b) underflows to negative and runs zero tics ("stuck").
//
// We can't reset the engine's internal lasttime without patching it, but we
// CAN make DG_GetTicksMs hide the dismissal interval from the engine: advance
// our anchor by however much real time passed while no one was querying it.
// From the engine's perspective time pauses during dismissal and resumes
// smoothly — like a cooperative game pause.
void doomgeneric_switch_reanchor_clock(void) {
    if (s_last_query_raw == 0) return;  // engine never queried — nothing to hide
    u64 now = armGetSystemTick();
    u64 gap = now - s_last_query_raw;
    s_tick_anchor += gap;  // hide the gap from engine
    s_last_query_raw = now;
}

// ---------------------------------------------------------------------
// Key queue
// ---------------------------------------------------------------------
//
// DoomGui::handleInput pushes synthesized key events here. The engine
// pulls them via DG_GetKey. Vanilla doomgeneric pattern (cf. its
// platform shim _soso.c, _xlib.c).

#define KEYQUEUE_SIZE 32

// gametic is the engine's master tic counter (incremented in G_Ticker).
// Used to gate when a queued event becomes visible to DG_GetKey, which is
// how we make push_digit's DOWN+UP straddle two tics so gamekeydown[]
// stays set across G_BuildTiccmd's read.
extern int gametic;

static struct {
    uint8_t pressed;       // 1 = down, 0 = up
    uint8_t key;           // doomkeys.h KEY_*
    int     eligible_tic;  // earliest gametic this event may surface
} s_key_queue[KEYQUEUE_SIZE];

static unsigned s_kq_write = 0;
static unsigned s_kq_read  = 0;

static void enqueue_key(int pressed, unsigned char key, int eligible_tic) {
    unsigned next = (s_kq_write + 1) % KEYQUEUE_SIZE;
    if (next == s_kq_read) {
        // Queue full — drop the oldest event so newest input is preserved.
        s_kq_read = (s_kq_read + 1) % KEYQUEUE_SIZE;
    }
    s_key_queue[s_kq_write].pressed      = pressed ? 1 : 0;
    s_key_queue[s_kq_write].key          = key;
    s_key_queue[s_kq_write].eligible_tic = eligible_tic;
    s_kq_write = next;
}

// Called from DoomGui::handleInput in main.cpp. Default gating: event is
// eligible immediately (tic=gametic). For digit keys synthesised by
// push_digit() below, we explicitly stagger the release.
void doomgeneric_switch_push_key(int pressed, unsigned char key) {
    enqueue_key(pressed, key, gametic);
}

// Weapon cycle — reads engine player state to skip unowned weapons.
//
// Vanilla Doom has no keyboard prev/next; only direct '1'-'8' digit keys.
// The engine cycles fist↔chainsaw on '1' and shotgun↔supershotgun on '3' when
// the player owns both. We exploit that to navigate via single digit pushes.
//
// Cycle order = vanilla number-key order with chainsaw/supershotgun threaded
// in at their digit-twin's position so users get a consistent low-to-high tour.

static const weapontype_t kCycleOrder[] = {
    wp_fist, wp_chainsaw,
    wp_pistol,
    wp_shotgun, wp_supershotgun,
    wp_chaingun,
    wp_missile, wp_plasma, wp_bfg,
};
#define CYCLE_LEN ((int)(sizeof(kCycleOrder)/sizeof(kCycleOrder[0])))

static const unsigned char kWeaponDigit[NUMWEAPONS] = {
    '1',  // wp_fist
    '2',  // wp_pistol
    '3',  // wp_shotgun
    '4',  // wp_chaingun
    '5',  // wp_missile (rocket launcher)
    '6',  // wp_plasma
    '7',  // wp_bfg
    '1',  // wp_chainsaw — alternates with fist on engine side
    '3',  // wp_supershotgun — alternates with shotgun (Doom 2 only)
};

// Synthesise a DOWN+UP keypress that straddles two tics. If we pushed both
// in the same tic, I_GetEvent's drain-the-queue loop applies them within
// one tic and gamekeydown[k] toggles 1→0 before G_BuildTiccmd reads it,
// dropping the input. Tag the UP with eligible_tic = gametic+1 so it only
// surfaces on the NEXT tic — gamekeydown[k] stays 1 across the whole
// current tic, weapon switch fires.
static void push_digit(unsigned char d) {
    enqueue_key(1, d, gametic);
    enqueue_key(0, d, gametic + 1);
}

static void cycle_weapon(int direction) {
    extern void doom_trace(const char*);
    weapontype_t cur = players[0].readyweapon;

    // Find current weapon's position in cycle.
    int cur_idx = 0;
    for (int i = 0; i < CYCLE_LEN; i++) {
        if (kCycleOrder[i] == cur) { cur_idx = i; break; }
    }

    // Walk through cycle in chosen direction; skip unowned weapons.
    for (int step = 1; step <= CYCLE_LEN; step++) {
        int next_idx = ((cur_idx + direction * step) % CYCLE_LEN + CYCLE_LEN) % CYCLE_LEN;
        weapontype_t w = kCycleOrder[next_idx];
        if (players[0].weaponowned[w]) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "cycle_weapon: dir=%d cur=%d -> w=%d digit='%c'",
                     direction, (int)cur, (int)w, kWeaponDigit[w]);
            doom_trace(buf);
            push_digit(kWeaponDigit[w]);
            return;
        }
    }
    doom_trace("cycle_weapon: no other owned weapon, no-op");
}

void doomgeneric_switch_weapon_prev(void) { cycle_weapon(-1); }
void doomgeneric_switch_weapon_next(void) { cycle_weapon(+1); }

int DG_GetKey(int* pressed, unsigned char* key) {
    if (s_kq_read == s_kq_write) return 0;  // queue empty
    // Eligibility gate: if the head event was tagged for a future tic
    // (push_digit's UP, eligible at gametic+1), report empty for THIS tic
    // so I_GetEvent's drain loop stops here. The event surfaces next tic.
    if (s_key_queue[s_kq_read].eligible_tic > gametic) return 0;
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
