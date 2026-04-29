// doomgeneric_nx.c — libnx platform shim for doomgeneric.
//
// Implements the six functions doomgeneric expects (doomgeneric.h:38-43)
// plus the longjmp recovery target for patches/0002 (I_Error sites).
//
// Threading: single-threaded. Engine tick + render run inside libtesla's
// update()/draw() callbacks. DG_DrawFrame is a no-op; DoomElement::draw
// blits DG_ScreenBuffer into libtesla's framebuffer directly.
//
// Key queue: filled by DoomGui::handleInput, consumed by DG_GetKey.
// Single-producer / single-consumer on the libtesla event-loop thread — no lock.
//
// Time: ARM Generic Timer via armGetSystemTick (19.2 MHz on Tegra X1).
//
// Licensed under GPLv2.

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <string.h>
#include <switch.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomdef.h"   // weapontype_t + NUMWEAPONS
#include "d_player.h"  // player_t (readyweapon, weaponowned[])

// players[] defined in g_game.c; re-declared here to keep dependency local.
extern player_t players[MAXPLAYERS];

// ---------------------------------------------------------------------
// Longjmp recovery — patches/0002 replaces exit(-1) in I_Error with
// longjmp(g_doom_error_jmp, code). main.cpp::try_init_engine runs
// setjmp(g_doom_error_jmp) before doomgeneric_Create(); non-zero return
// means the engine errored — recover and report.
// ---------------------------------------------------------------------
jmp_buf g_doom_error_jmp;

// ---------------------------------------------------------------------
// Tick clock
// ---------------------------------------------------------------------

static u64 s_tick_anchor    = 0;
static u64 s_last_query_raw = 0;

uint32_t DG_GetTicksMs(void) {
    u64 now = armGetSystemTick();
    if (s_tick_anchor == 0) s_tick_anchor = now;
    s_last_query_raw = now;
    u64 elapsed = now - s_tick_anchor;
    return (uint32_t)(elapsed / 19200ULL);  // Tegra X1: 19.2 MHz tick
}

void DG_SleepMs(uint32_t ms) {
    svcSleepThread((u64)ms * 1000000ULL);
}

// Re-anchor clock after overlay is dismissed + resumed (onShow).
//
// Engine's d_loop.c keeps a static `lasttime`. After dismissal, the next
// I_GetTime call sees a huge jump: `newtics = nowtime - lasttime` goes huge,
// causing either a multi-second catch-up freeze or an underflow to zero.
// We hide the gap by advancing the anchor by however much real time passed
// while no one was querying — from the engine's perspective, time paused.
void doomgeneric_switch_reanchor_clock(void) {
    if (s_last_query_raw == 0) return;
    u64 now = armGetSystemTick();
    u64 gap = now - s_last_query_raw;
    s_tick_anchor += gap;
    s_last_query_raw = now;
}

// ---------------------------------------------------------------------
// Key queue (SPSC, capacity 32)
// ---------------------------------------------------------------------

#define KEYQUEUE_SIZE 32

static struct {
    uint8_t pressed;
    uint8_t key;
} s_key_queue[KEYQUEUE_SIZE];

static unsigned s_kq_write = 0;
static unsigned s_kq_read  = 0;

void doomgeneric_switch_push_key(int pressed, unsigned char key) {
    unsigned next = (s_kq_write + 1) % KEYQUEUE_SIZE;
    if (next == s_kq_read) {
        // Queue full — drop oldest to preserve newest input.
        s_kq_read = (s_kq_read + 1) % KEYQUEUE_SIZE;
    }
    s_key_queue[s_kq_write].pressed = pressed ? 1 : 0;
    s_key_queue[s_kq_write].key     = key;
    s_kq_write = next;
}

// Weapon cycle — reads engine player state to skip unowned weapons.
//
// Vanilla Doom has no keyboard prev/next; only direct '1'-'8' digit keys.
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
    '5',  // wp_missile
    '6',  // wp_plasma
    '7',  // wp_bfg
    '1',  // wp_chainsaw — alternates with fist on engine side
    '3',  // wp_supershotgun — alternates with shotgun (Doom 2 only)
};

static void push_digit(unsigned char d) {
    doomgeneric_switch_push_key(1, d);
    doomgeneric_switch_push_key(0, d);
}

static void cycle_weapon(int direction) {
    weapontype_t cur = players[0].readyweapon;

    int cur_idx = 0;
    for (int i = 0; i < CYCLE_LEN; i++) {
        if (kCycleOrder[i] == cur) { cur_idx = i; break; }
    }

    for (int step = 1; step <= CYCLE_LEN; step++) {
        int next_idx = ((cur_idx + direction * step) % CYCLE_LEN + CYCLE_LEN) % CYCLE_LEN;
        weapontype_t w = kCycleOrder[next_idx];
        if (players[0].weaponowned[w]) {
            push_digit(kWeaponDigit[w]);
            return;
        }
    }
    // No other owned weapon — silently no-op.
}

void doomgeneric_switch_weapon_prev(void) { cycle_weapon(-1); }
void doomgeneric_switch_weapon_next(void) { cycle_weapon(+1); }

int DG_GetKey(int* pressed, unsigned char* key) {
    if (s_kq_read == s_kq_write) return 0;
    *pressed = s_key_queue[s_kq_read].pressed;
    *key     = s_key_queue[s_kq_read].key;
    s_kq_read = (s_kq_read + 1) % KEYQUEUE_SIZE;
    return 1;
}

// ---------------------------------------------------------------------
// Frame / window stubs
// ---------------------------------------------------------------------

void DG_Init(void) {
    // DG_ScreenBuffer already allocated by doomgeneric.c:21.
}

void DG_DrawFrame(void) {
    // No-op: DoomElement::draw blits DG_ScreenBuffer each libtesla composite.
}

void DG_SetWindowTitle(const char* title) {
    (void)title;
}
