/********************************************************************************
 * File: doom_input.hpp
 * Description:
 *   Switch HID button → Doom key-code mapping table.
 *
 *   DoomGui::handleInput() writes raw keysDown/keysHeld/keysUp each frame into
 *   g_hid_keys_* atomics (doom_globals.hpp).  The nx port's I_GetEvent()
 *   replacement reads those atomics and synthesises Doom ev_keydown/ev_keyup
 *   events for D_ProcessEvents().
 *
 *   Doom key codes are copied from chocolate-doom/src/doomkeys.h with a
 *   DOOM_KEY_ prefix to avoid collision with libnx's KEY_* constants.
 *
 *  Licensed under GPLv2
 ********************************************************************************/

#pragma once
#include <cstdint>

// libnx defines KEY_* when building with devkitPro.
// Fallback constants let IDE tools analyse this file without the Switch SDK.
#ifndef KEY_A
static constexpr uint64_t KEY_A     = 1ULL << 0;
static constexpr uint64_t KEY_B     = 1ULL << 1;
static constexpr uint64_t KEY_X     = 1ULL << 2;
static constexpr uint64_t KEY_Y     = 1ULL << 3;
static constexpr uint64_t KEY_L     = 1ULL << 6;
static constexpr uint64_t KEY_R     = 1ULL << 7;
static constexpr uint64_t KEY_ZL    = 1ULL << 8;
static constexpr uint64_t KEY_ZR    = 1ULL << 9;
static constexpr uint64_t KEY_PLUS  = 1ULL << 10;
static constexpr uint64_t KEY_MINUS = 1ULL << 11;
static constexpr uint64_t KEY_LEFT  = 1ULL << 12;
static constexpr uint64_t KEY_UP    = 1ULL << 13;
static constexpr uint64_t KEY_RIGHT = 1ULL << 14;
static constexpr uint64_t KEY_DOWN  = 1ULL << 15;
#endif

// ── Doom key codes (from doomkeys.h) ─────────────────────────────────────────
namespace doom_key {
    static constexpr int RIGHT  = 0xae;
    static constexpr int LEFT   = 0xac;
    static constexpr int UP     = 0xad;
    static constexpr int DOWN   = 0xaf;
    static constexpr int ESCAPE = 27;
    static constexpr int ENTER  = 13;
    static constexpr int TAB    = 9;
    static constexpr int BACKSP = 0x7f;
    static constexpr int RSHIFT = (0x80 + 0x36);
    static constexpr int RCTRL  = (0x80 + 0x1d);
    static constexpr int SPACE  = ' ';
    static constexpr int F1     = (0x80 + 0x3b);
    static constexpr int F2     = (0x80 + 0x3c);
    static constexpr int F5     = (0x80 + 0x3f);
    static constexpr int PAUSE  = 0xff;
    static constexpr int COMMA  = ',';
    static constexpr int PERIOD = '.';
    static constexpr int MINUS  = '-';
    static constexpr int EQUALS = '=';
}

// ── HID → Doom mapping table ──────────────────────────────────────────────────
// Used by I_GetEvent() (nx port) to translate physical button state into Doom
// key events.  KEY_* constants are libnx/Tesla HID bitmasks.
struct DoomKeyMap {
    uint64_t hid_bit;
    int      doom_key;
};

static constexpr DoomKeyMap DOOM_KEY_MAP[] = {
    // ── Movement — primary: D-pad ─────────────────────────────────────────
    { KEY_UP,    doom_key::UP    },   // move forward
    { KEY_DOWN,  doom_key::DOWN  },   // move backward
    { KEY_LEFT,  doom_key::LEFT  },   // turn left
    { KEY_RIGHT, doom_key::RIGHT },   // turn right

    // ── Actions ───────────────────────────────────────────────────────────
    { KEY_A,    doom_key::RCTRL  },   // fire
    { KEY_ZR,   doom_key::RCTRL  },   // fire (alternate)
    { KEY_B,    doom_key::SPACE  },   // use / open door
    { KEY_Y,    doom_key::RSHIFT },   // run

    // ── Strafe ────────────────────────────────────────────────────────────
    { KEY_L,    doom_key::COMMA  },   // strafe left
    { KEY_R,    doom_key::PERIOD },   // strafe right
    { KEY_ZL,   doom_key::COMMA  },   // strafe left (alternate)

    // ── Menu / UI ─────────────────────────────────────────────────────────
    { KEY_PLUS,  doom_key::ESCAPE },  // open menu / pause
    { KEY_MINUS, doom_key::ENTER  },  // confirm / select
    { KEY_X,     doom_key::TAB    },  // automap toggle

    // ── Weapon switching: left/right on face buttons (optional) ───────────
    // KEY_L -> prev weapon, KEY_R -> next weapon handled via COMMA/PERIOD above.
    // Add F-key equivalents here if needed.
};

static constexpr int DOOM_KEY_MAP_LEN =
    static_cast<int>(sizeof(DOOM_KEY_MAP) / sizeof(DOOM_KEY_MAP[0]));
