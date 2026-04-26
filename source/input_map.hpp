// input_map.hpp — Switch HID → Doom keycode mapping for sx-doom-overlay.
//
// libnx HID provides keysDown (newly pressed this frame) and keysUp (newly
// released this frame). We translate each to a Doom keycode and push into
// the engine's key queue via doomgeneric_switch_push_key (defined in
// doomgeneric_switch.c). Doom's input system handles auto-repeat itself
// for held movement keys, so we only forward DOWN and UP edges.
//
// Button mapping (PRD §Flow 2):
//   D-pad up/down/left/right → KEY_UPARROW/DOWN/LEFT/RIGHT
//   A                        → KEY_FIRE
//   B                        → KEY_USE
//   X                        → KEY_FIRE  (alt for trigger comfort)
//   Y                        → KEY_TAB   (automap toggle)
//   L                        → KEY_STRAFE_L
//   R                        → KEY_STRAFE_R
//   ZL                       → KEY_RSHIFT (run modifier)
//   ZR                       → KEY_FIRE  (alt for trigger comfort)
//   Plus                     → KEY_ESCAPE (Doom in-game menu)
//   Minus                    → reserved for our settings (Task 11)
//
// Stick handling will land in a follow-up; D-pad covers basic walk + turn.
//
// Licensed under GPLv2.

#pragma once

#include <switch.h>

extern "C" {
// In doomgeneric_switch.c.
void doomgeneric_switch_push_key(int pressed, unsigned char key);
}

namespace doom_input {

// HID bit → Doom keycode pairs. Maintained as a contiguous array so the
// translation loop is just a small fixed scan; libnx emits a maximum of
// ~25 simultaneous buttons, far more than we'll ever map.
struct ButtonMapping {
    u64           hid_bit;
    unsigned char doom_key;
};

constexpr ButtonMapping kButtonMap[] = {
    { HidNpadButton_AnyUp,    /*KEY_UPARROW   */ 0xad },
    { HidNpadButton_AnyDown,  /*KEY_DOWNARROW */ 0xaf },
    { HidNpadButton_AnyLeft,  /*KEY_LEFTARROW */ 0xac },
    { HidNpadButton_AnyRight, /*KEY_RIGHTARROW*/ 0xae },
    { HidNpadButton_A,        /*KEY_FIRE      */ 0xa3 },
    { HidNpadButton_B,        /*KEY_USE       */ 0xa2 },
    { HidNpadButton_X,        /*KEY_FIRE alt  */ 0xa3 },
    { HidNpadButton_Y,        /*KEY_TAB       */ 9    },
    { HidNpadButton_L,        /*KEY_STRAFE_L  */ 0xa0 },
    { HidNpadButton_R,        /*KEY_STRAFE_R  */ 0xa1 },
    { HidNpadButton_ZL,       /*KEY_RSHIFT    */ static_cast<unsigned char>(0x80 + 0x36) },
    { HidNpadButton_ZR,       /*KEY_FIRE alt  */ 0xa3 },
    { HidNpadButton_Plus,     /*KEY_ESCAPE    */ 27   },
    // Minus reserved for our settings menu (Task 11) — NOT mapped to Doom
};

// Forward keysDown / keysUp events into the engine's key queue. Called once
// per libtesla update() callback from DoomGui::handleInput.
inline void dispatch(u64 keysDown, u64 keysUp) {
    for (const auto& m : kButtonMap) {
        if (keysDown & m.hid_bit) doomgeneric_switch_push_key(1, m.doom_key);
        if (keysUp   & m.hid_bit) doomgeneric_switch_push_key(0, m.doom_key);
    }
}

}  // namespace doom_input
