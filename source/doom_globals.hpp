/********************************************************************************
 * File: doom_globals.hpp
 * Description:
 *   Shared constants and state for the UltraDoom overlay.
 *   CMAP256 single-threaded architecture:
 *     - DG_ScreenBuffer is the engine's framebuffer (uint8_t*, 320x200 indices)
 *     - draw() reads DG_ScreenBuffer directly (no shared FB, no mutex)
 *     - palette_changed / colors[] externs from doomgeneric's i_video.h
 *     - Active RGBA4444 LUT rebuilt on palette_changed each frame
 *
 *  Licensed under GPLv2
 ********************************************************************************/

#pragma once

#include <cstdint>
#include <cstdio>
#include <atomic>

// ── Doom native resolution ────────────────────────────────────────────────────
static constexpr int DOOM_W = 320;
static constexpr int DOOM_H = 200;

// ── Tesla overlay framebuffer ─────────────────────────────────────────────────
static constexpr int FB_W = 448;
static constexpr int FB_H = 720;

// ── Render scale (1, 2, or 3). Read from INI in initServices. ─────────────────
// Viewport dimensions: DOOM_W*scale x DOOM_H*scale, centered in FB.
// VP_X/VP_Y/VP_W/VP_H computed at runtime from g_render_scale.
static constexpr int DEFAULT_SCALE = 2;
static int g_render_scale = DEFAULT_SCALE;

// ── Active RGBA4444 palette LUT ───────────────────────────────────────────────
// Built from doomgeneric's extern colors[256] by rebuild_palette_lut() in blit.cpp.
// Rebuilt whenever palette_changed is set by the engine (damage flash, powerup, etc.)
static uint16_t g_active_lut[256] = {};

// ── Pre-cached PLAYPAL LUTs (14 banks, 7 KB total) ───────────────────────────
// Built once after doomgeneric_Create() returns by scanning the PLAYPAL lump.
// Allows instant palette bank switch without recomputing from RGB triples.
static uint16_t g_playpal_luts[14][256] = {};

// ── Engine state ──────────────────────────────────────────────────────────────
// g_doom_initialized: set to true in DoomGui::update() after first doomgeneric_Tick().
static std::atomic<bool> g_doom_initialized{false};
// g_doom_quit: set in exitServices() to break the tick loop in update().
static std::atomic<bool> g_doom_quit{false};

// ── Error message from doom_nx_fatal_error() ──────────────────────────────────
// Written by doom_nx_fatal_error() in doomgeneric_nx.c on I_Error.
// Displayed by DoomElement::draw() instead of the game view.
static char g_doom_error[512] = {};

// ── Filesystem paths ──────────────────────────────────────────────────────────
static constexpr const char* DOOM_CONFIG_DIR  = "sdmc:/config/ultradoom/";
static constexpr const char* DOOM_WAD_PATH    = "sdmc:/roms/doom/doom.wad";
static constexpr const char* DOOM_PERF_LOG    = "sdmc:/config/ultradoom/perflog.txt";
static constexpr const char* DOOM_ERROR_LOG   = "sdmc:/config/ultradoom/error.log";

// ── Overlay close combo ───────────────────────────────────────────────────────
static bool combo_pressed(u64 keysDown, u64 keysHeld) {
    return (keysDown & tsl::cfg::launchCombo) &&
           (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo);
}
