// input_map.hpp — Switch HID → Doom keycode mapping for UltraDoom.
//
// Twin-stick modern controls (Bethesda Doom Classic Switch port pattern):
//
//   MOVEMENT
//     L-stick up/down     → forward/back
//     L-stick left/right  → strafe left/right
//     R-stick left/right  → turn left/right
//
//   COMBAT
//     ZR                  → fire (primary trigger)
//     A                   → use + menu select
//     B                   → alt fire + menu back
//     ZL                  → run (hold)
//
//   WEAPONS (D-pad = weapon shortcuts)
//     D-up → '1', D-down → '5', D-left → '2', D-right → '3'
//     L bumper → '4' (chaingun), R bumper → '6' (plasma), Y → '7' (BFG)
//
//   UTILITY
//     X     → automap toggle
//     Plus  → Escape (menu)
//     Plus+Minus → quit overlay (handled in DoomGui::handleInput before dispatch)
//
// CRITICAL: use specific HidNpadButton_StickL* bits, NOT HidNpadButton_AnyUp
// etc. — "Any" bundles D-pad + all sticks into one bit, causing all three
// sticks to turn the camera simultaneously.
//
// Licensed under GPLv2.

#pragma once

#include <switch.h>

extern "C" {
void doomgeneric_switch_push_key(int pressed, unsigned char key);
void doomgeneric_switch_weapon_prev(void);  // reads player state, skips unowned
void doomgeneric_switch_weapon_next(void);
}

namespace doom_input {

struct ButtonMapping {
    u64           hid_bit;
    unsigned char doom_key;
};

constexpr ButtonMapping kButtonMap[] = {
    // L-stick — movement (twin-stick)
    { HidNpadButton_StickLUp,    0xad },  // KEY_UPARROW   — forward
    { HidNpadButton_StickLDown,  0xaf },  // KEY_DOWNARROW — back
    { HidNpadButton_StickLLeft,  0xa0 },  // KEY_STRAFE_L
    { HidNpadButton_StickLRight, 0xa1 },  // KEY_STRAFE_R
    // R-stick — turn
    { HidNpadButton_StickRLeft,  0xac },  // KEY_LEFTARROW
    { HidNpadButton_StickRRight, 0xae },  // KEY_RIGHTARROW

    // D-pad — arrow keys (menu nav in menus; movement/turn duplicate in gameplay)
    { HidNpadButton_Up,    0xad },  // KEY_UPARROW
    { HidNpadButton_Down,  0xaf },  // KEY_DOWNARROW
    { HidNpadButton_Left,  0xac },  // KEY_LEFTARROW — turn left
    { HidNpadButton_Right, 0xae },  // KEY_RIGHTARROW — turn right

    // Face buttons
    { HidNpadButton_A, 0xa2 },  // KEY_USE   + menu select
    { HidNpadButton_A, 13   },
    { HidNpadButton_B, 0xa3 },  // KEY_FIRE  + menu back
    { HidNpadButton_B, 0x7f },
    { HidNpadButton_X, 9    },  // KEY_TAB — automap
    // L/R bumpers handled in dispatch() via weapon_prev/next (not in table).

    // Triggers
    { HidNpadButton_ZL, static_cast<unsigned char>(0x80 + 0x36) },  // KEY_RSHIFT (run)
    { HidNpadButton_ZR, 0xa3 },  // KEY_FIRE — primary

    // System
    { HidNpadButton_Plus, 27 },  // KEY_ESCAPE — open menu
};

inline void dispatch(u64 keysDown, u64 keysUp) {
    for (const auto& m : kButtonMap) {
        if (keysDown & m.hid_bit) doomgeneric_switch_push_key(1, m.doom_key);
        if (keysUp   & m.hid_bit) doomgeneric_switch_push_key(0, m.doom_key);
    }
    // Bumpers — state-aware weapon cycle (skips unowned weapons).
    if (keysDown & HidNpadButton_L) doomgeneric_switch_weapon_prev();
    if (keysDown & HidNpadButton_R) doomgeneric_switch_weapon_next();
}

}  // namespace doom_input
