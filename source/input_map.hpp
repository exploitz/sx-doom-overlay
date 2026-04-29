// input_map.hpp — Switch HID → Doom keycode mapping for sx-doom-overlay.
//
// libnx HID provides keysDown (newly pressed this frame) and keysUp (newly
// released this frame). We translate each to a Doom keycode and push into
// the engine's key queue via doomgeneric_switch_push_key (defined in
// doomgeneric_switch.c). Doom's input system handles auto-repeat itself
// for held movement keys, so we only forward DOWN and UP edges.
//
// Modern Switch-Doom controls — every button has a UNIQUE, useful purpose.
// Modeled on the Bethesda Doom Classic Switch port + standard twin-stick FPS.
//
//   ESSENTIALS — interact + kill
//     A                      → KEY_USE   (doors, switches, secrets)  + ENTER (menu select)
//     ZR                     → KEY_FIRE  (primary trigger — the kill button)
//
//   MOVEMENT — twin-stick (Duke Nukem / modern FPS)
//     L-stick up/down/L/R    → forward/back/strafe-L/strafe-R
//     R-stick L/R            → turn left/right (Doom has no vertical aim)
//
//   COMBAT EXTRAS
//     B                      → KEY_FIRE alt + BACKSPACE (back-of-thumb fire / menu back)
//     ZL                     → KEY_RSHIFT (RUN modifier — hold to sprint)
//
//   WEAPONS
//     L bumper               → previous owned weapon (cycle, skips unowned)
//     R bumper               → next owned weapon     (cycle, skips unowned)
//
//   D-PAD — arrow keys (rationale: menu navigation. Users naturally reach
//   for D-pad in menus, and binding it to KEY_UPARROW/DOWN/LEFT/RIGHT means
//   menu nav works identically whether the player uses L-stick or D-pad. In
//   gameplay this duplicates L-stick movement+turn — that's intentional, not
//   a waste: it's an alternate input device for users who prefer the cross.
//
//   UTILITY / SYSTEM
//     X                      → KEY_TAB (toggle automap)
//     Y                      → unmapped (free for future quicksave w/ auto-confirm)
//     Plus                   → KEY_ESCAPE (open Doom in-game menu)
//     Minus                  → (no Doom binding; Plus+Minus combo = quit overlay)
//
// MENU NAV: L-stick U/D = nav, R-stick L/R = nav L/R (changes settings),
// A = select, B = back, Plus = close menu. D-pad sends digit keys which menu
// ignores. L/R bumpers cycle weapons (harmless in menu).
//
// CRITICAL: bind specific stick/D-pad bits, NOT HidNpadButton_AnyXxx — "Any"
// bundles D-pad + L-stick + R-stick into one bit which made all three sticks
// turn the camera in earlier builds.
//   L                        → KEY_STRAFE_L
//   R                        → KEY_STRAFE_R
//   ZL                       → KEY_RSHIFT (RUN modifier — hold to sprint)
//   ZR                       → KEY_FIRE   (primary fire on trigger)
//   Plus                     → KEY_ESCAPE (open in-game menu)
//   Minus                    → KEY_TAB    (toggle automap)
//
// A and B intentionally each push two events per press: one for in-game state
// (USE) and one for menu state (ENTER/BACKSPACE). Doom's M_Responder consumes
// menu keys only when a menu is open; G_Responder ignores menu keys during
// gameplay. They don't collide.
//
// Stick handling will land in a follow-up; D-pad covers basic walk + turn.
//
// Licensed under GPLv2.

#pragma once

#include <switch.h>

extern "C" {
// In doomgeneric_switch.c.
void doomgeneric_switch_push_key(int pressed, unsigned char key);
void doomgeneric_switch_weapon_prev(void);  // reads engine state, skips unowned
void doomgeneric_switch_weapon_next(void);
// In main.cpp.
void doom_trace(const char* msg);
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
    // Sticks — twin-stick movement
    { HidNpadButton_StickLUp,    /*KEY_UPARROW    */ 0xad },  // forward
    { HidNpadButton_StickLDown,  /*KEY_DOWNARROW  */ 0xaf },  // back
    { HidNpadButton_StickLLeft,  /*KEY_STRAFE_L   */ 0xa0 },  // strafe L
    { HidNpadButton_StickLRight, /*KEY_STRAFE_R   */ 0xa1 },  // strafe R
    { HidNpadButton_StickRLeft,  /*KEY_LEFTARROW  */ 0xac },  // turn L
    { HidNpadButton_StickRRight, /*KEY_RIGHTARROW */ 0xae },  // turn R

    // D-pad — arrow keys (menu nav identical to L-stick; in-game = movement/turn)
    { HidNpadButton_Up,    /*KEY_UPARROW    */ 0xad },
    { HidNpadButton_Down,  /*KEY_DOWNARROW  */ 0xaf },
    { HidNpadButton_Left,  /*KEY_LEFTARROW  */ 0xac },  // turn left  (classic D-pad Doom)
    { HidNpadButton_Right, /*KEY_RIGHTARROW */ 0xae },  // turn right

    // Face buttons — essentials + menu nav. NO 'y'/'n' bindings: those polluted
    // save names with "Y", "YY"... patches/0004 instead binds menu_confirm to
    // ENTER and menu_abort to ESCAPE, so A (sends ENTER) confirms dialogs and
    // B (sends BACKSPACE) backs out. Save dialogs auto-name slots — no typing.
    { HidNpadButton_A,        /*KEY_USE        */ 0xa2 },  // INTERACT (doors etc)
    { HidNpadButton_A,        /*KEY_ENTER      */ 13   },  //    + menu select / dialog confirm
    { HidNpadButton_B,        /*KEY_FIRE       */ 0xa3 },  // alt fire (face button)
    { HidNpadButton_B,        /*KEY_BACKSPACE  */ 0x7f },  //    + menu back / dialog abort
    { HidNpadButton_X,        /*KEY_TAB        */ 9    },  // toggle automap
    // Y is intentionally unmapped in the static table — handled below for the
    // weapon cycle? No, L/R do that. Y is genuinely free for now; could host
    // quicksave once we ship the auto-confirm save patch. Leave unbound rather
    // than arbitrary.

    // Triggers — primary combat
    { HidNpadButton_ZL,       /*KEY_RSHIFT     */ static_cast<unsigned char>(0x80 + 0x36) },  // RUN hold
    { HidNpadButton_ZR,       /*KEY_FIRE       */ 0xa3 },  // PRIMARY KILL — trigger

    // System (Minus intentionally unmapped — Plus+Minus combo = quit overlay,
    // handled in main.cpp DoomGui::handleInput before dispatch.)
    { HidNpadButton_Plus,     /*KEY_ESCAPE     */ 27   },
};

// Forward keysDown / keysUp events into the engine's key queue. Called once
// per libtesla update() callback from DoomGui::handleInput.
inline void dispatch(u64 keysDown, u64 keysUp) {
    for (const auto& m : kButtonMap) {
        if (keysDown & m.hid_bit) doomgeneric_switch_push_key(1, m.doom_key);
        if (keysUp   & m.hid_bit) doomgeneric_switch_push_key(0, m.doom_key);
    }
    // L/R bumpers — weapon cycle (state-aware, skips unowned weapons).
    if (keysDown & HidNpadButton_L) {
        doom_trace("input: L bumper -> weapon_prev");
        doomgeneric_switch_weapon_prev();
    }
    if (keysDown & HidNpadButton_R) {
        doom_trace("input: R bumper -> weapon_next");
        doomgeneric_switch_weapon_next();
    }
}

}  // namespace doom_input
