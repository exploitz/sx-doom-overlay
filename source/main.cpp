// DOOM — Doom inside an Ultrahand overlay
//
// Adapted from sx-doom-overlay (Chase Gober), which is the confirmed-working
// reference build. Architecture is identical:
//   - Single-threaded: engine tick + render in libtesla update()/draw().
//   - doomgeneric_Create() called lazily in first update() — after framebuffer ready.
//   - doomgeneric_Tick() called in 35 Hz accumulator in update().
//   - DoomElement::draw() blits DG_ScreenBuffer → libtesla setPixel().
//   - Palette LUT rebuilt from colors[256] on palette_changed each frame.
//   - Audio: Task 9 (stub in audio_backend_libnx.c returns INIT_FAILED).
//
// CRITICAL: libtesla's framebuffer is block-linear (Tegra GPU swizzled), NOT
// flat row-major. Writing to getCurrentFramebuffer() directly scribbles memory
// over libtesla state → Atmosphère crash. Use renderer->setPixel() which routes
// through the correct swizzle. Direct FB optimization is a future task.
//
// Licensed under GPLv2.

#define NDEBUG
#define TESLA_INIT_IMPL
#include <ultra.hpp>
#include <tesla.hpp>
#include "elm_ultradoomframe.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <setjmp.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <malloc.h>     // mallinfo for heap counter
#include <algorithm>
#include <string>
#include <vector>

#include "blit.hpp"
#include "input_map.hpp"

extern "C" {
#include "audio_backend.h"
#include "audio_glue.h"
#include "doomgeneric.h"
#include "i_video.h"  // extern colors[256], extern palette_changed
#include "music_ogg.h"  // music_ogg_set_iwad — per-WAD music dir lookup

extern jmp_buf g_doom_error_jmp;             // defined in doomgeneric_switch.c
extern void doomgeneric_switch_reanchor_clock(void);

void doomgeneric_Create(int argc, char** argv);
void doomgeneric_Tick(void);

// Doom engine internals we read for the heap counter / savestate semantics.
extern int Z_FreeMemory(void);    // z_zone.c — free bytes in Doom zone
extern int quickSaveSlot;         // m_menu.c — pre-set so F6 silently saves
extern int gamemission;           // doomstat.h GameMission_t: 0=doom (episodes), 1+=doom2-style
extern int gamestate;             // g_game.c — 0=GS_LEVEL (in-game)
char *D_SaveGameIWADName(int);    // d_iwad.c — maps gamemission → "doom.wad" etc. for save dir
void G_SaveGame(int slot, char *description);  // g_game.c — sets gameaction=ga_savegame
void G_LoadGame(char *name);                   // g_game.c — sets gameaction=ga_loadgame
void D_StartTitle(void);                       // d_main.c — resets to title screen / demo cycle

// Volume control — s_sound.c. Takes 0-127 (Doom internal scale).
// Our UI uses 0-15 (matching Doom's in-game menu), multiplied by 8 here.
void S_SetSfxVolume(int volume);
void S_SetMusicVolume(int volume);
}

namespace {

// libtesla's framebuffer is 448x720 with hardcoded block-linear swizzle for
// offsetWidthVar=112 (448/4). Pixels past x=447 land at wrong addresses.
// Stay at 448 wide. Doom source is 320x200 with non-square 5:6 pixels, so
// correct 4:3 display = 448x336. Nearest-neighbor scale at 35 Hz is fine.
constexpr int kFbWidth    = 448;
constexpr int kFbHeight   = 720;
constexpr int kDoomW      = 320;
constexpr int kDoomH      = 200;
constexpr int kScaledW    = 448;                           // fill full FB width
constexpr int kScaledH    = 336;                           // 448*(3/4) — correct 4:3 with Doom PAR
constexpr int kDoomOffsetX = 0;    // edge-to-edge horizontally
constexpr int kDoomOffsetY = 108;  // matches UltraGB VP_Y: 11px below header (activeHeaderHeight=97)

// F9 = quickload from quickSaveSlot. Shows a confirm prompt; we inject ENTER
// (key_menu_confirm = 13 per patch 0004) to auto-confirm.
constexpr unsigned char kDoomKeyF9     = 0x80 + 0x43;  // quickload
constexpr unsigned char kDoomKeyEnter  = 13;           // auto-confirm prompts

// WAD directories scanned in priority order; scan_wads() dedupes by basename.
//   - kWadDir         (primary): /config/doom/wads/ — canonical location
//   - kWadDirLegacy2:             /roms/doom/       — RetroArch / EmuDeck legacy
//   - kWadDirLegacy:              /switch/sx-doom-overlay/ — oldest layout
constexpr const char* kWadDir        = "sdmc:/config/doom/wads";
constexpr const char* kWadDirLegacy2 = "sdmc:/roms/doom";
constexpr const char* kWadDirLegacy  = "sdmc:/switch/sx-doom-overlay";
constexpr const char* kConfigDir     = "sdmc:/config/doom";
constexpr const char* kTraceLog      = "sdmc:/config/doom/trace.log";
constexpr const char* kConfigFile    = "sdmc:/config/doom/config.ini";
constexpr const char* kConfigSection = "doom";

// 0=Off, 1=Scanlines, 2=LCD Grid, 3=Dot Matrix
int g_lcd_effect = 0;

static constexpr const char* kLcdEffectNames[] = {
    "Off", "Scanlines", "LCD Grid", "Dot Matrix", "CRT",
    "Game Boy", "ChrAber", "Interlace", "Composite", "Trinitron", "Halftone"
};
static constexpr int kLcdEffectCount = 11;

// 0=Fine (1-px lines), 1=Coarse (2-px lines)
static int g_lcd_width = 0;
static constexpr const char* kLcdWidthNames[] = { "Fine", "Coarse" };

// 0=Off, 1=Green phosphor, 2=Amber phosphor, 3=White (grayscale), 4=Sepia, 5=Red phosphor
static int g_color_filter = 0;
static constexpr const char* kColorFilterNames[] = { "Off", "Green", "Amber", "White", "Sepia", "Red", "Blue", "Cyan", "Purple", "Night", "Invert", "VBoy", "Thermal" };
static constexpr int kColorFilterCount = 13;

static bool g_vignette = false;
static bool g_dither = false;
static bool g_show_fps = false;
static int  g_sfx_volume   = 8;   // 0-15
static int  g_music_volume = 8;   // 0-15
static int  g_gamma        = 8;   // 0-15, 8=normal (1×), linear brightness scale

// Mute state — session-only, not persisted. Pre-mute values restored on unmute.
static bool g_sfx_muted = false,  g_music_muted = false,  g_game_muted = false;
static int  g_sfx_pre_mute = 8,   g_music_pre_mute = 8,   g_game_pre_mute = 8;

// Per-process title volume — targets only the running game's audio process.
// Uses audouta (HOS < 11.0.0) or auda (>= 11.0.0), same as sys-tune / UltraGB.
// Lazy: open service, set vol, close immediately. NEVER called from lifecycle hooks.
static int   g_game_volume      = 15;   // 0-15 UI; 15 = 100% (no change)
static float g_sys_vol_baseline = 1.0f; // title's audproc vol captured on first use
static bool  g_auddev_ready     = false;
static bool  g_auddev_dirty     = false;
static u64   g_title_pid        = 0;

static Result s_audproc_open() {
    return hosversionBefore(11,0,0) ? audoutaInitialize() : audaInitialize();
}
static void s_audproc_close() {
    if (hosversionBefore(11,0,0)) audoutaExit(); else audaExit();
}
static Result s_audproc_get(u64 pid, float* out) {
    return hosversionBefore(11,0,0)
        ? audoutaGetProcessMasterVolume(pid, out)
        : audaGetAudioOutputProcessMasterVolume(pid, out);
}
static Result s_audproc_set(u64 pid, float vol) {
    if (hosversionBefore(11,0,0))
        return audoutaSetProcessMasterVolume(pid, 0, vol);
    Result rc = audaSetAudioOutputProcessMasterVolume(pid, 0, vol);
    if (R_SUCCEEDED(rc)) audaSetAudioInputProcessMasterVolume(pid, 0, vol);
    return rc;
}

static void title_vol_ensure_init() {
    if (g_auddev_ready) return;
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) {
        doom_trace("title_vol: pmdmnt failed — no title running?"); return;
    }
    if (R_FAILED(s_audproc_open())) {
        doom_trace("title_vol: audproc open failed"); return;
    }
    float baseline = 1.0f;
    s_audproc_get(pid, &baseline);
    s_audproc_close();
    char tb[80]; std::snprintf(tb, sizeof(tb), "title_vol: pid=%lu baseline=%.3f", pid, baseline);
    doom_trace(tb);
    g_title_pid        = pid;
    g_sys_vol_baseline = baseline;
    g_auddev_ready     = true;
}

static void title_vol_apply(int level) {
    title_vol_ensure_init();
    if (!g_auddev_ready) return;
    if (level >= 15) {
        if (g_auddev_dirty) {
            if (R_SUCCEEDED(s_audproc_open())) {
                s_audproc_set(g_title_pid, g_sys_vol_baseline);
                s_audproc_close();
            }
            g_auddev_dirty = false;
        }
        return;
    }
    if (R_FAILED(s_audproc_open())) return;
    float vol = g_sys_vol_baseline * (level / 15.0f);
    Result rc = s_audproc_set(g_title_pid, vol);
    s_audproc_close();
    char tb[80]; std::snprintf(tb, sizeof(tb), "title_vol: set level=%d vol=%.3f rc=0x%x", level, vol, rc);
    doom_trace(tb);
    g_auddev_dirty = true;
}

static void title_vol_restore() {
    if (!g_auddev_ready || !g_auddev_dirty) return;
    if (R_SUCCEEDED(s_audproc_open())) {
        s_audproc_set(g_title_pid, g_sys_vol_baseline);
        s_audproc_close();
    }
    g_auddev_dirty = false;
    g_auddev_ready = false;
}


static bool g_config_open     = false;
static int  g_config_selected = 0;
static int  g_config_mode     = 0;   // 0=settings, 1=cheats, 2=warp ep/map, 3=warp map (doom1)
static int  g_warp_episode    = 0;   // chosen Doom 1 episode (1-4); 0=not picked
static int  g_warp_scroll     = 0;   // scroll top index for warp list
static constexpr int kConfigItemCount = 14;  // 9 settings + Music Player + Game Vol + Cheats + Screensaver + Idle Timer
static constexpr int kWarpPageSize    = 9;   // max items visible in warp picker
static constexpr int kMusicPageSize   = 14;  // max items visible in music player
static int  g_deferred_close_ticks = 0;     // >0: close after this many update() frames (lets G_DoSaveGame run)
static int  g_touch_strip_btn      = -1;    // 0=Save,1=Load,2=Quit; -1=none pressed

// Screensaver — idle-triggered title-demo mode with auto-resume
static bool g_screensaver_enabled    = false;
static int  g_screensaver_minutes    = 5;    // 1-5 min idle threshold
static bool g_screensaver_active     = false; // currently showing title demo
static int  g_screensaver_save_ticks = 0;    // >0: waiting for save before D_StartTitle
static u64  g_last_input_ns          = 0;    // armTicksToNs of last keysDown event

struct CheatEntry { const char* label; const char* seq; };
static constexpr CheatEntry kCheats[] = {
    { "God Mode",        "iddqd"      },
    { "All Weapons",     "idkfa"      },
    { "Ammo + Armor",    "idfa"       },
    { "Berserk",         "idbeholds"  },
    { "Invisibility",    "idbeholdi"  },
    { "No Clip",         "idclip"     },
    { "Full Map",        "iddt"       },
    { "Invulnerability", "idbeholdv"  },
    { "Level Warp  \xe2\x96\xb6", ""  },
};
static constexpr int kCheatCount = 9;

// stdio_stubs.c overrides fprintf/fputs to no-ops (suppresses 220+ engine
// prints that crash on NULL stdout). libultrahand's setIniFile uses both, so
// any save via ult::setIniFileValue writes an empty file silently.
// Use fwrite/fread directly — neither is overridden by stdio_stubs.c.
static void save_config() {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    FILE* f = std::fopen(kConfigFile, "w");
    if (!f) return;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[doom]\nlcd_effect=%d\nlcd_width=%d\ncolor_filter=%d\nvignette=%d\ndither=%d\n"
                  "show_fps=%d\nsfx_volume=%d\nmusic_volume=%d\ngamma=%d\ngame_volume=%d\n"
                  "screensaver=%d\nscreensaver_minutes=%d\n",
                  g_lcd_effect, g_lcd_width, g_color_filter, (int)g_vignette, (int)g_dither,
                  (int)g_show_fps, g_sfx_volume, g_music_volume, g_gamma, g_game_volume,
                  (int)g_screensaver_enabled, g_screensaver_minutes);
    std::fwrite(line, 1, std::strlen(line), f);
    std::fclose(f);
}
static void load_config() {
    FILE* f = std::fopen(kConfigFile, "r");
    if (!f) return;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    const char* sv = std::strstr(buf, "sfx_volume=");
    if (sv) { int v = std::atoi(sv + 11); if (v >= 0 && v <= 15) g_sfx_volume = v; }
    const char* mv = std::strstr(buf, "music_volume=");
    if (mv) { int v = std::atoi(mv + 13); if (v >= 0 && v <= 15) g_music_volume = v; }
    const char* gv = std::strstr(buf, "game_volume=");
    if (gv) { int v = std::atoi(gv + 12); if (v >= 0 && v <= 15) g_game_volume = v; }
    const char* gm = std::strstr(buf, "gamma=");
    if (gm) { int v = std::atoi(gm + 6); if (v >= 0 && v <= 15) g_gamma = v; }
    if (std::strstr(buf, "lcd_grid=1")) { g_lcd_effect = 2; return; }
    const char* p = std::strstr(buf, "lcd_effect=");
    if (p) { int v = std::atoi(p + 11); if (v >= 0 && v < kLcdEffectCount) g_lcd_effect = v; }
    const char* w = std::strstr(buf, "lcd_width=");
    if (w) { int v = std::atoi(w + 10); if (v == 0 || v == 1) g_lcd_width = v; }
    const char* cf = std::strstr(buf, "color_filter=");
    if (cf) { int v = std::atoi(cf + 13); if (v >= 0 && v < kColorFilterCount) g_color_filter = v; }
    const char* vi = std::strstr(buf, "vignette=");
    if (vi) { g_vignette = std::atoi(vi + 9) != 0; }
    const char* di = std::strstr(buf, "dither=");
    if (di) { g_dither = std::atoi(di + 7) != 0; }
    const char* fp = std::strstr(buf, "show_fps=");
    if (fp) { g_show_fps = std::atoi(fp + 9) != 0; }
    const char* ss = std::strstr(buf, "screensaver=");
    if (ss) { g_screensaver_enabled = std::atoi(ss + 12) != 0; }
    const char* sm = std::strstr(buf, "screensaver_minutes=");
    if (sm) { int v = std::atoi(sm + 20); if (v >= 1 && v <= 5) g_screensaver_minutes = v; }
    }

// Engine state — protected by libtesla single-thread invariant.
bool             g_doom_initialized = false;
bool             g_doom_failed      = false;
char             g_doom_error_msg[128] = {0};
audio_backend_t* g_audio_backend    = nullptr;
doom_blit::PaletteLut g_palette_lut;

struct WadEntry {
    std::string filename;
    std::string fullpath;
    std::string display_name;
};

std::string friendly_wad_name(const std::string& fn) {
    auto eq = [&](const char* p) { return strcasecmp(fn.c_str(), p) == 0; };
    if (eq("doom.wad"))      return "Doom (Ultimate)";
    if (eq("doom2.wad"))     return "Doom II";
    if (eq("doom1.wad"))     return "Doom (shareware)";
    if (eq("tnt.wad"))       return "TNT: Evilution";
    if (eq("plutonia.wad"))  return "The Plutonia Experiment";
    if (eq("freedoom1.wad")) return "Freedoom Phase 1";
    if (eq("freedoom2.wad")) return "Freedoom Phase 2";
    if (eq("chex.wad"))      return "Chex Quest";
    if (eq("chex3.wad"))     return "Chex Quest 3";
    if (eq("hacx.wad"))      return "HacX";
    return fn;
}

// Scan one directory for *.wad files, append to `result`, dedupe against
// existing entries by case-insensitive basename so the legacy directory
// doesn't shadow a primary-directory WAD with the same name.
static void scan_wad_dir(const char* dir, std::vector<WadEntry>& result) {
    if (!dir || !dir[0]) return;
    mkdir(dir, 0777);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = std::strlen(name);
        if (len <= 4 || strcasecmp(name + len - 4, ".wad") != 0) continue;
        bool dupe = false;
        for (const auto& e : result) {
            if (strcasecmp(e.filename.c_str(), name) == 0) { dupe = true; break; }
        }
        if (dupe) continue;
        WadEntry e;
        e.filename     = name;
        e.fullpath     = std::string(dir) + "/" + name;
        e.display_name = friendly_wad_name(e.filename);
        result.push_back(std::move(e));
    }
    closedir(d);
}

std::vector<WadEntry> scan_wads() {
    std::vector<WadEntry> result;
    scan_wad_dir(kWadDir,        result);  // primary: /config/doom/wads
    scan_wad_dir(kWadDirLegacy2, result); // legacy: /roms/doom
    scan_wad_dir(kWadDirLegacy,  result); // legacy: /switch/sx-doom-overlay
    return result;
}

// Pick whichever trace log already exists on disk so multiple builds (Samurai's
// older fork at /config/sx-doom-overlay/, current main at /config/doom/) all
// append to the same file rather than each making its own. First call
// resolves the path; subsequent calls reuse it.
static const char* resolve_trace_log_path() {
    static const char* resolved = nullptr;
    if (resolved) return resolved;
    static const char* candidates[] = {
        "sdmc:/config/sx-doom-overlay/trace.log",  // legacy / Samurai's fork
        "sdmc:/config/doom/trace.log",             // current canonical
    };
    for (auto p : candidates) {
        FILE* f = std::fopen(p, "r");
        if (f) { std::fclose(f); resolved = p; return resolved; }
    }
    resolved = kTraceLog;  // default — gets created on first append
    return resolved;
}

extern "C" void doom_trace(const char* msg) {
    FILE* f = std::fopen(resolve_trace_log_path(), "a");
    if (f) {
        // snprintf+fwrite, NOT fprintf: stdio_stubs.c overrides fprintf to a
        // no-op (prevents 220+ unguarded engine prints crashing on NULL stdout).
        // Going through fprintf here would silently drop the line.
        char line[256];
        const unsigned ts_ms = static_cast<unsigned>(armTicksToNs(armGetSystemTick()) / 1000000ULL);
        const int n = std::snprintf(line, sizeof(line), "[%u] %s\n", ts_ms, msg);
        if (n > 0) {
            std::fwrite(line, 1, static_cast<size_t>(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1), f);
        }
        std::fclose(f);
    }
}

void try_init_engine(const char* iwad_path) {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    doom_trace("=== try_init_engine ===");

    int err = setjmp(g_doom_error_jmp);
    if (err != 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "longjmp received code=%d", err);
        doom_trace(buf);
        g_doom_failed = true;
        std::snprintf(g_doom_error_msg, sizeof(g_doom_error_msg),
                      "Engine error (code %d) — see /atmosphere/crash_reports/", err);
        return;
    }

    if (!iwad_path || iwad_path[0] == '\0') {
        g_doom_failed = true;
        std::snprintf(g_doom_error_msg, sizeof(g_doom_error_msg), "No IWAD selected");
        doom_trace("try_init_engine: empty iwad_path");
        return;
    }
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "selected IWAD: %s", iwad_path);
        doom_trace(buf);
    }

    // Extract IWAD stem (e.g. "/path/CHEX.WAD" → "chex") and hand it to the
    // music backend so per-WAD OGG packs (sdmc:/.../music/chex/d_e1m1.ogg)
    // are picked up automatically. music_ogg_set_iwad lowercases internally;
    // we just need to strip the dir + extension here.
    {
        const char* slash = std::strrchr(iwad_path, '/');
        const char* base  = slash ? slash + 1 : iwad_path;
        char stem[64];
        size_t i = 0;
        for (; base[i] && base[i] != '.' && i < sizeof(stem) - 1; ++i) {
            stem[i] = base[i];
        }
        stem[i] = '\0';
        music_ogg_set_iwad(stem);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "music_ogg: iwad scope = %s", stem);
        doom_trace(buf);
    }

    // STATIC: doomgeneric stores myargv as a pointer copy (no deep copy).
    // Stack-local storage would dangle after this function returns.
    //
    // We deliberately DO NOT pass -nomusic. The OPL music engine (vendored
    // chocolate-doom in source/opl/) is wired via i_sound_switch.c's
    // DG_music_module → music_opl_module bridge, and InitMusicModule binds
    // it during D_DoomMain. The 512 KB MIDI arena is Z_Malloc'd at music
    // init time. SFX is wired similarly via DG_sound_module.
    static char arg_doom[]    = "doom";
    static char arg_iwad[]    = "-iwad";
    static char arg_iwadpath[256];
    static char arg_mb[]      = "-mb";
    static char arg_mbsize[]  = "4";  // -mb 6 is too tight; 4 MiB confirmed working
    std::strncpy(arg_iwadpath, iwad_path, sizeof(arg_iwadpath) - 1);
    arg_iwadpath[sizeof(arg_iwadpath) - 1] = '\0';
    static char* engine_argv[] = {
        arg_doom, arg_iwad, arg_iwadpath,
        arg_mb, arg_mbsize,
        nullptr
    };
    int engine_argc = 5;

    doom_trace("calling doomgeneric_Create...");
    doomgeneric_Create(engine_argc, engine_argv);
    doom_trace("doomgeneric_Create returned OK");
    g_doom_initialized = true;

    S_SetSfxVolume(g_sfx_volume * 127 / 15);
    S_SetMusicVolume(g_music_volume * 127 / 15);
    audio_backend_set_boost(4);  // max boost, always on

    // Pre-set quickSaveSlot so the Save button (F6) skips the "set a slot
    // first" prompt and goes straight to the "Y/N over slot 0?" dialog
    // (which we auto-confirm with 'y' from onTouch). Slot 0 is the canonical
    // "savestate" slot for our purposes; user can still use F2/F3-style
    // multi-slot saves through the Doom in-game menu (Plus button).
    // Map savestate to last slot (Doom menu "Save Slot 8" = internal 7) so
    // the Save State button doesn't overwrite users' manual saves in slots
    // 1-7. Vanilla Doom has 8 slots; users typically fill 1-3, last slot is
    // safest as the auto-savestate target.
    quickSaveSlot = 7;

    // Auto-resume: check for an existing autosave (slot 7 = doomsav7.dsg) in
    // the per-WAD save directory set by patch 0006. D_SaveGameIWADName returns
    // the canonical name the engine uses for that directory (e.g. "doom.wad").
    // If the file exists, queue F9+ENTER so the very first tick auto-loads it —
    // giving seamless "close and reopen = resume" save-state semantics.
    {
        const char* iwad_save_dir = D_SaveGameIWADName(gamemission);
        if (iwad_save_dir) {
            char savefile[256];
            std::snprintf(savefile, sizeof(savefile),
                          "sdmc:/config/doom/saves/%s/doomsav7.dsg", iwad_save_dir);
            FILE* probe = std::fopen(savefile, "rb");
            if (probe) {
                std::fclose(probe);
                doom_trace("autosave found — queuing F9+Enter for auto-resume");
                doomgeneric_switch_push_key(1, kDoomKeyF9);
                doomgeneric_switch_push_key(0, kDoomKeyF9);
                doomgeneric_switch_push_key(1, kDoomKeyEnter);
                doomgeneric_switch_push_key(0, kDoomKeyEnter);
            } else {
                doom_trace("no autosave — starting fresh");
            }
        }
    }

    doom_blit::build_palette_lut_from_argb_struct(
        reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
    palette_changed = false;
}

// ── Music player ────────────────────────────────────────────────────────
struct TrackEntry {
    std::string path;    // full sdmc: path
    std::string display; // "subfolder/name" or "name" (d_ and .ogg stripped)
};
static std::vector<TrackEntry> g_track_list;
static int                     g_track_idx = -1;  // -1 = engine-controlled

static void scan_music_tracks() {
    g_track_list.clear();
    const char* iwad = music_ogg_get_iwad();
    char dir1[128] = {};
    char dir2[128];
    if (iwad && iwad[0])
        std::snprintf(dir1, sizeof(dir1), "sdmc:/config/doom/music/%s", iwad);
    std::snprintf(dir2, sizeof(dir2), "sdmc:/config/doom/music");

    auto make_display = [](const char* subfolder, const char* filename) -> std::string {
        const char* n = filename;
        if (n[0] == 'd' && n[1] == '_') n += 2;
        char base[48];
        size_t i = 0;
        for (; n[i] && n[i] != '.' && i < sizeof(base) - 1; ++i) base[i] = n[i];
        base[i] = '\0';
        if (subfolder && subfolder[0]) return std::string(subfolder) + "/" + base;
        return base;
    };

    auto add_track = [&](const char* dir, const char* subfolder, const char* filename) {
        std::string disp = make_display(subfolder, filename);
        for (const auto& t : g_track_list)
            if (t.display == disp) return;  // dedup by display name
        char full[512];
        if (subfolder && subfolder[0])
            std::snprintf(full, sizeof(full), "%s/%s/%s", dir, subfolder, filename);
        else
            std::snprintf(full, sizeof(full), "%s/%s", dir, filename);
        g_track_list.push_back({full, disp});
    };

    auto scan_one = [&](const char* d) {
        if (!d || !d[0]) return;
        DIR* dirp = opendir(d);
        if (!dirp) return;
        struct dirent* ent;
        while ((ent = readdir(dirp)) != nullptr) {
            const char* n = ent->d_name;
            if (n[0] == '.') continue;
            if (ent->d_type == DT_DIR) {
                // One level of subfolders (e.g. e1/, e2/ for episode packs)
                char subpath[512];
                std::snprintf(subpath, sizeof(subpath), "%s/%s", d, n);
                DIR* sd = opendir(subpath);
                if (!sd) continue;
                struct dirent* sent;
                while ((sent = readdir(sd)) != nullptr) {
                    const char* sn = sent->d_name;
                    if (sn[0] == '.') continue;
                    if (sent->d_type == DT_DIR) continue;
                    const size_t sl = std::strlen(sn);
                    if (sl < 5 || strcasecmp(sn + sl - 4, ".ogg") != 0) continue;
                    add_track(d, n, sn);
                }
                closedir(sd);
                continue;
            }
            const size_t nl = std::strlen(n);
            if (nl < 5 || strcasecmp(n + nl - 4, ".ogg") != 0) continue;
            add_track(d, nullptr, n);
        }
        closedir(dirp);
    };

    scan_one(dir1);
    scan_one(dir2);
    // Sort by (has_subfolder DESC, folder, name) so episode packs group together
    std::sort(g_track_list.begin(), g_track_list.end(),
              [](const TrackEntry& a, const TrackEntry& b) {
                  size_t sa = a.display.find('/'), sb = b.display.find('/');
                  bool ha = sa != std::string::npos, hb = sb != std::string::npos;
                  if (ha != hb) return ha > hb;  // subfolder tracks before root
                  std::string fa = ha ? a.display.substr(0, sa) : "";
                  std::string fb = hb ? b.display.substr(0, sb) : "";
                  int fc = strcasecmp(fa.c_str(), fb.c_str());
                  if (fc != 0) return fc < 0;
                  const char* na = ha ? a.display.c_str() + sa + 1 : a.display.c_str();
                  const char* nb = hb ? b.display.c_str() + sb + 1 : b.display.c_str();
                  return strcasecmp(na, nb) < 0;
              });
}

// Flat display list for music player: -2=section header, -1=auto, >=0=track index.
struct MusicItem { int track_idx; std::string label; };
static std::vector<MusicItem> g_music_items;

static void build_music_items() {
    g_music_items.clear();
    g_music_items.push_back({-1, "\xe2\x97\x80 Auto"});  // ◀ Auto
    std::string cur_folder;
    for (int i = 0; i < (int)g_track_list.size(); ++i) {
        const std::string& disp = g_track_list[i].display;
        size_t slash = disp.find('/');
        std::string folder = (slash != std::string::npos) ? disp.substr(0, slash) : "";
        std::string name   = (slash != std::string::npos) ? disp.substr(slash + 1) : disp;
        if (folder != cur_folder) {
            cur_folder = folder;
            if (!folder.empty()) {
                std::string hdr = folder;
                for (char& c : hdr) c = (char)toupper((unsigned char)c);
                g_music_items.push_back({-2, hdr});
            }
        }
        g_music_items.push_back({i, name});
    }
}

static const char* track_display_name() {
    if (g_track_idx < 0 || g_track_idx >= static_cast<int>(g_track_list.size()))
        return "Auto";
    static char buf[24];
    const std::string& d = g_track_list[g_track_idx].display;
    size_t i = 0;
    for (; i < d.size() && i < sizeof(buf) - 1; ++i) buf[i] = d[i];
    buf[i] = '\0';
    return buf;
}

// ── Screenshot — minimal PNG encoder (uncompressed DEFLATE, no libpng) ────────
static void take_screenshot() {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    mkdir("sdmc:/config/doom/screenshots", 0777);
    char path[128];
    for (int n = 0; n < 10000; ++n) {
        std::snprintf(path, sizeof(path),
                      "sdmc:/config/doom/screenshots/doom_%04d.png", n);
        FILE* probe = std::fopen(path, "rb");
        if (!probe) break;
        std::fclose(probe);
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(DG_ScreenBuffer);
    if (!src) { std::fclose(f); return; }
    const uint8_t* pal = reinterpret_cast<const uint8_t*>(&colors[0]);

    // CRC32 (IEEE 802.3 polynomial, used by PNG chunks)
    static uint32_t s_crc_tbl[256];
    static bool s_crc_init = false;
    if (!s_crc_init) {
        for (int i = 0; i < 256; ++i) {
            uint32_t c = (uint32_t)i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            s_crc_tbl[i] = c;
        }
        s_crc_init = true;
    }
    auto crc_feed = [](uint32_t& c, const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i)
            c = s_crc_tbl[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    };
    auto put = [&](const uint8_t* b, size_t n) { std::fwrite(b, 1, n, f); };
    auto put32be = [&](uint32_t v) {
        uint8_t b[4] = { uint8_t(v>>24), uint8_t(v>>16), uint8_t(v>>8), uint8_t(v) };
        std::fwrite(b, 1, 4, f);
    };
    auto write_chunk = [&](const uint8_t* type, const uint8_t* data, uint32_t len) {
        put32be(len);
        put(type, 4);
        if (len) put(data, len);
        uint32_t c = 0xFFFFFFFFu;
        crc_feed(c, type, 4);
        if (len) crc_feed(c, data, len);
        put32be(c ^ 0xFFFFFFFFu);
    };

    // PNG signature
    static const uint8_t kSig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    put(kSig, 8);

    // IHDR: 320×200, 8-bit RGB, no interlace
    {
        uint8_t ihdr[13] = {};
        // width=320 (0x00000140)
        ihdr[0]=0; ihdr[1]=0; ihdr[2]=1; ihdr[3]=0x40;
        // height=200 (0x000000C8)
        ihdr[4]=0; ihdr[5]=0; ihdr[6]=0; ihdr[7]=0xC8;
        ihdr[8]=8; ihdr[9]=2; // bit_depth=8, color_type=2 (RGB)
        static const uint8_t kT[4] = {'I','H','D','R'};
        write_chunk(kT, ihdr, 13);
    }

    // Build filtered rows: [filter=0][R G B ...] × 200 rows, heap-allocated
    const int kW = 320, kH = 200;
    const int kStride = 1 + kW * 3;           // 961 bytes/row
    const int kTotal  = kH * kStride;          // 192200 bytes
    uint8_t* raw = static_cast<uint8_t*>(malloc(kTotal));
    if (!raw) { std::fclose(f); return; }

    uint32_t adler_s1 = 1, adler_s2 = 0;
    for (int y = 0; y < kH; ++y) {
        uint8_t* row = raw + y * kStride;
        row[0] = 0;  // PNG filter = None
        for (int x = 0; x < kW; ++x) {
            const int idx  = src[y * kW + x];
            row[1 + x*3]   = pal[idx*4 + 2]; // R  (colors[] layout: BGRA)
            row[1 + x*3+1] = pal[idx*4 + 1]; // G
            row[1 + x*3+2] = pal[idx*4 + 0]; // B
        }
        for (int i = 0; i < kStride; ++i) {
            adler_s1 = (adler_s1 + row[i]) % 65521u;
            adler_s2 = (adler_s2 + adler_s1) % 65521u;
        }
    }

    // IDAT: zlib header + uncompressed DEFLATE blocks + Adler-32
    const int kMaxBlock = 65535;
    const int kNBlocks  = (kTotal + kMaxBlock - 1) / kMaxBlock;
    const uint32_t kIdatLen = 2u + (uint32_t)(kNBlocks * 5) + (uint32_t)kTotal + 4u;

    put32be(kIdatLen);
    static const uint8_t kIDAT[4] = {'I','D','A','T'};
    put(kIDAT, 4);
    uint32_t idat_crc = 0xFFFFFFFFu;
    crc_feed(idat_crc, kIDAT, 4);

    // zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no dict, checksum)
    const uint8_t zh[2] = { 0x78, 0x01 };
    put(zh, 2);
    crc_feed(idat_crc, zh, 2);

    for (int off = 0; off < kTotal; ) {
        const int blen   = std::min(kMaxBlock, kTotal - off);
        const bool final = (off + blen >= kTotal);
        const uint16_t ln = (uint16_t)blen, nl = ~ln;
        uint8_t bhdr[5] = { uint8_t(final ? 1 : 0),
                             uint8_t(ln), uint8_t(ln>>8),
                             uint8_t(nl), uint8_t(nl>>8) };
        put(bhdr, 5);          crc_feed(idat_crc, bhdr, 5);
        put(raw + off, blen);  crc_feed(idat_crc, raw + off, blen);
        off += blen;
    }
    free(raw);

    // Adler-32 trailer (big-endian)
    const uint32_t adler = (adler_s2 << 16) | adler_s1;
    const uint8_t ab[4] = { uint8_t(adler>>24), uint8_t(adler>>16),
                             uint8_t(adler>>8),  uint8_t(adler) };
    put(ab, 4);
    crc_feed(idat_crc, ab, 4);
    put32be(idat_crc ^ 0xFFFFFFFFu);

    // IEND
    static const uint8_t kIEND[4] = {'I','E','N','D'};
    write_chunk(kIEND, nullptr, 0);

    std::fclose(f);
    doom_trace(path);
}

}  // namespace

class CheatMenuGui final : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new DoomOverlayFrame("Back", "");
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Power-ups"));
        add_cheat(list, "God Mode",        "IDDQD",     "iddqd");
        add_cheat(list, "Invulnerability", "IDBEHOLDV",  "idbeholdv");
        add_cheat(list, "Berserk",         "IDBEHOLDS",  "idbeholds");
        add_cheat(list, "Invisibility",    "IDBEHOLDI",  "idbeholdi");
        add_cheat(list, "Radiation Suit",  "IDBEHOLDR",  "idbeholdr");
        add_cheat(list, "Light Amp",       "IDBEHOLDL",  "idbeholdl");

        list->addItem(new tsl::elm::CategoryHeader("Arsenal"));
        add_cheat(list, "All Weapons + Keys", "IDKFA",     "idkfa");
        add_cheat(list, "Ammo + Armor",       "IDFA",      "idfa");
        add_cheat(list, "Chainsaw",           "IDCHOPPERS","idchoppers");

        list->addItem(new tsl::elm::CategoryHeader("Navigation"));
        add_cheat(list, "No Clip",   "IDCLIP", "idclip");
        add_cheat(list, "Full Map",  "IDDT",   "iddt");
        add_cheat(list, "Automap",   "IDBEHOLDA", "idbeholda");

        frame->setContent(list);
        return frame;
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        const bool simulatedBack = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel);
        if (simulatedBack || (keysDown & HidNpadButton_Left) || (keysDown & HidNpadButton_B)) {
            tsl::goBack();
            return true;
        }
        return false;
    }

private:
    static void add_cheat(tsl::elm::List* list, const char* label,
                          const char* value, const char* seq) {
        auto* item = new tsl::elm::ListItem(label, value);
        std::string s = seq;
        item->setClickListener([s](u64 keys) -> bool {
            if ((keys & HidNpadButton_A) && g_doom_initialized && !g_doom_failed) {
                doomgeneric_switch_push_cheat(s.c_str());
                tsl::goBack();
                return true;
            }
            return false;
        });
        list->addItem(item);
    }
};

class ConfigGui final : public tsl::Gui {
    tsl::elm::TrackBar* m_sfx_bar    = nullptr;
    tsl::elm::TrackBar* m_mus_bar    = nullptr;
    tsl::elm::TrackBar* m_game_bar   = nullptr;
    tsl::elm::ListItem* m_effect_item = nullptr;
    tsl::elm::ListItem* m_filter_item = nullptr;
    bool m_effect_cycling = false;
    bool m_filter_cycling = false;
public:
    tsl::elm::Element* createUI() override {
        doom_trace("DIAG: ConfigGui::createUI enter");
        auto* frame = new DoomOverlayFrame("WAD Picker", "");
        doom_trace("DIAG: ConfigGui::createUI DoomOverlayFrame allocated");
        auto* list  = new tsl::elm::List();
        doom_trace("DIAG: ConfigGui::createUI List allocated");

        list->addItem(new tsl::elm::CategoryHeader("Audio"));
        doom_trace("DIAG: ConfigGui::createUI Audio header added");

        auto* sfx_bar = new tsl::elm::TrackBar("\xe2\x99\xaa", false, false, true, "SFX Volume", "", false);
        sfx_bar->setRange(0, 15);
        sfx_bar->setProgress(static_cast<u8>(g_sfx_volume));
        sfx_bar->setValueChangedListener([sfx_bar](u16) {
            g_sfx_volume = static_cast<int>(sfx_bar->getProgress());
            if (g_doom_initialized && !g_doom_failed) S_SetSfxVolume(g_sfx_volume * 127 / 15);
            save_config();
            });
        list->addItem(sfx_bar);
        m_sfx_bar = sfx_bar;
        doom_trace("DIAG: ConfigGui::createUI sfx_bar added");

        auto* mus_bar = new tsl::elm::TrackBar("\xe2\x99\xab", false, false, true, "Music Volume", "", false);
        mus_bar->setRange(0, 15);
        mus_bar->setProgress(static_cast<u8>(g_music_volume));
        mus_bar->setValueChangedListener([mus_bar](u16) {
            g_music_volume = static_cast<int>(mus_bar->getProgress());
            if (g_doom_initialized && !g_doom_failed) S_SetMusicVolume(g_music_volume * 127 / 15);
            save_config();
            });
        list->addItem(mus_bar);
        m_mus_bar = mus_bar;
        doom_trace("DIAG: ConfigGui::createUI mus_bar added");

        auto* game_bar = new tsl::elm::TrackBar("\xe2\x99\xac", false, false, true, "Game Volume", "", false);
        game_bar->setRange(0, 15);
        game_bar->setProgress(static_cast<u8>(g_game_volume));
        game_bar->setValueChangedListener([game_bar](u16) {
            g_game_volume = static_cast<int>(game_bar->getProgress());
            title_vol_apply(g_game_volume);
            save_config();
            });
        list->addItem(game_bar);
        m_game_bar = game_bar;
        doom_trace("DIAG: ConfigGui::createUI game_bar added");

        list->addItem(new tsl::elm::CategoryHeader("Display"));
        doom_trace("DIAG: ConfigGui::createUI Display header added");

        auto* gamma_bar = new tsl::elm::TrackBar("\xe2\x98\x80", false, false, true, "Gamma", "", false);
        gamma_bar->setRange(0, 15);
        gamma_bar->setProgress(static_cast<u8>(g_gamma));
        gamma_bar->setValueChangedListener([gamma_bar](u16) {
            g_gamma = static_cast<int>(gamma_bar->getProgress());
            save_config();
            });
        list->addItem(gamma_bar);

        auto* effect_item = new tsl::elm::ListItem("LCD Effect", kLcdEffectNames[g_lcd_effect]);
        effect_item->setClickListener([this, effect_item](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                if (!m_effect_cycling) {
                    m_effect_cycling = true;
                } else {
                    m_effect_cycling = false;
                    save_config();
                }
                return true;
            }
            return false;
        });
        list->addItem(effect_item);
        m_effect_item = effect_item;
        doom_trace("DIAG: ConfigGui::createUI effect_item added");

        auto* width_item = new tsl::elm::ListItem("Effect Width", kLcdWidthNames[g_lcd_width]);
        width_item->setClickListener([width_item](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                g_lcd_width ^= 1;
                width_item->setValue(kLcdWidthNames[g_lcd_width]);
                save_config();
                return true;
            }
            return false;
        });
        list->addItem(width_item);
        doom_trace("DIAG: ConfigGui::createUI width_item added");

        auto* filter_item = new tsl::elm::ListItem("Color Filter", kColorFilterNames[g_color_filter]);
        filter_item->setClickListener([this, filter_item](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                if (!m_filter_cycling) {
                    m_filter_cycling = true;
                } else {
                    m_filter_cycling = false;
                    save_config();
                }
                return true;
            }
            return false;
        });
        list->addItem(filter_item);
        m_filter_item = filter_item;
        doom_trace("DIAG: ConfigGui::createUI filter_item added");

        auto* vign_item = new tsl::elm::ToggleListItem("Vignette", g_vignette, ult::ON, ult::OFF);
        vign_item->setStateChangedListener([](bool state) {
            g_vignette = state;
            save_config();
        });
        list->addItem(vign_item);
        doom_trace("DIAG: ConfigGui::createUI vign_item added");

        auto* dither_item = new tsl::elm::ToggleListItem("Dither", g_dither, ult::ON, ult::OFF);
        dither_item->setStateChangedListener([](bool state) {
            g_dither = state;
            save_config();
        });
        list->addItem(dither_item);
        doom_trace("DIAG: ConfigGui::createUI dither_item added");
                                
                frame->setContent(list);
        doom_trace("DIAG: ConfigGui::createUI setContent done — returning frame");
        return frame;
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        const bool simulatedBack = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel);

        // Auto-exit cycle mode if focus moved away from the item
        if (m_effect_cycling && !(m_effect_item && m_effect_item->hasFocus()))
            m_effect_cycling = false;
        if (m_filter_cycling && !(m_filter_item && m_filter_item->hasFocus()))
            m_filter_cycling = false;

        // L/R scroll when LCD Effect is selected
        if (m_effect_item && m_effect_item->hasFocus() && m_effect_cycling) {
            if (keysDown & HidNpadButton_Right) {
                g_lcd_effect = (g_lcd_effect + 1) % kLcdEffectCount;
                m_effect_item->setValue(kLcdEffectNames[g_lcd_effect]);
                save_config();
                return true;
            }
            if (keysDown & HidNpadButton_Left) {
                g_lcd_effect = (g_lcd_effect - 1 + kLcdEffectCount) % kLcdEffectCount;
                m_effect_item->setValue(kLcdEffectNames[g_lcd_effect]);
                save_config();
                return true;
            }
        }

        // L/R scroll when Color Filter is selected
        if (m_filter_item && m_filter_item->hasFocus() && m_filter_cycling) {
            if (keysDown & HidNpadButton_Right) {
                g_color_filter = (g_color_filter + 1) % kColorFilterCount;
                m_filter_item->setValue(kColorFilterNames[g_color_filter]);
                save_config();
                return true;
            }
            if (keysDown & HidNpadButton_Left) {
                g_color_filter = (g_color_filter - 1 + kColorFilterCount) % kColorFilterCount;
                m_filter_item->setValue(kColorFilterNames[g_color_filter]);
                save_config();
                return true;
            }
        }

        const bool cycleActive = (m_effect_item && m_effect_item->hasFocus() && m_effect_cycling)
                               || (m_filter_item && m_filter_item->hasFocus() && m_filter_cycling);
        // Only navigate back on Left when no slider or cycle item is active.
        const bool sliderActive = ((m_sfx_bar  && m_sfx_bar->hasFocus())
                                || (m_mus_bar  && m_mus_bar->hasFocus())
                                || (m_game_bar && m_game_bar->hasFocus()))
                                && ult::allowSlide.load(std::memory_order_acquire);
        const bool wantBack = !(sliderActive || cycleActive) && (simulatedBack
                            || (keysDown & HidNpadButton_B)
                            || (keysDown & HidNpadButton_Left));
        if (wantBack) {
            m_effect_cycling = false;
            m_filter_cycling = false;
            ult::allowSlide.store(false, std::memory_order_release);
            tsl::goBack();
            return true;
        }
        return false;
    }
};


// Game viewport rendering — extracted as a free function so we can drop it
// into a tsl::elm::CustomDrawer (which gives us a non-interactive rendering
// region inside a libtesla List, sized by the list).
//
// Draws either: failure message, "Loading…" placeholder, OR the scaled Doom
// framebuffer (320×200 → 448×336 nearest-neighbor) followed by a one-line
// memory counter (newlib + Doom Z_Malloc zone).
inline void draw_doom_viewport(tsl::gfx::Renderer* renderer,
                               s32 boundsX, s32 boundsY, s32 boundsW, s32 boundsH) {
    (void)boundsX; (void)boundsW; (void)boundsH;

    if (g_doom_failed) {
        renderer->drawString("Engine init failed:", false, 20, boundsY + 60, 22, tsl::Color(0xFFFF));
        renderer->drawString(g_doom_error_msg,      false, 20, boundsY + 100, 18, tsl::Color(0xFA0F));
        return;
    }
    if (!g_doom_initialized) {
        renderer->drawString("Loading...", false, 20, boundsY + 60, 22, tsl::Color(0xFFFF));
        return;
    }

    if (palette_changed) {
        doom_blit::build_palette_lut_from_argb_struct(
            reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
        palette_changed = false;
    }

    // Scale 320x200 → 448x336 (fill-width, correct 4:3 Doom aspect).
    // Background already filled by DoomOverlayFrame::draw().
    const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(DG_ScreenBuffer);
    if (src) {
        const int lcd = g_lcd_effect;
        const bool coarse = (g_lcd_width == 1);
        const int filter = g_color_filter;
        const bool vign = g_vignette;

        // Vignette LUT — 2D radial, 8× downsampled (42×56 = 2352 bytes).
        // dist2 is in physical-pixel space (normalised by half-diagonal = 280px)
        // so the falloff shape is a true circle on the 448×336 viewport.
        // Transition starts at 65% of diagonal; quadratic drop to near-black at corners.
        static constexpr int kVigRows = (kScaledH + 7) / 8;  // 42
        static constexpr int kVigCols = (kScaledW + 7) / 8;  // 56
        static constexpr float kVigHalfDiag2 = (224.f * 224.f + 168.f * 168.f);  // 280² = 78400
        static uint8_t s_vig_lut[kVigRows][kVigCols] = {};
        static bool s_vig_init = false;
        if (!s_vig_init) {
            s_vig_init = true;
            for (int iy = 0; iy < kVigRows; ++iy) {
                for (int ix = 0; ix < kVigCols; ++ix) {
                    // Physical pixel coords relative to viewport centre
                    float px = (ix + 0.5f) * 8.f - kScaledW * 0.5f;
                    float py = (iy + 0.5f) * 8.f - kScaledH * 0.5f;
                    float dist2 = (px * px + py * py) / kVigHalfDiag2;
                    float excess = dist2 > 0.65f ? dist2 - 0.65f : 0.f;
                    float w = 1.0f - 4.5f * excess * excess;
                    if (w < 0.04f) w = 0.04f;
                    s_vig_lut[iy][ix] = static_cast<uint8_t>(w * 16.f + 0.5f);
                }
            }
        }

        // Bayer 4×4 ordered dither — proper 0/+1 threshold, staggered per channel
        // to avoid correlated R/G/B bias creating coloured noise patches.
        static constexpr u8 kBayer4[4][4] = {
            {  0,  8,  2, 10 },
            { 12,  4, 14,  6 },
            {  3, 11,  1,  9 },
            { 15,  7, 13,  5 }
        };
        const bool dither = g_dither;
        const int gamma_mul = g_gamma + 8;  // 8..23; 16=1× (identity)

        static uint32_t s_frame_cnt = 0;
        ++s_frame_cnt;

        for (int dy = 0; dy < kScaledH; ++dy) {
            const int sy_lin = dy * kDoomH / kScaledH;
            const std::uint8_t* srow = src + sy_lin * kDoomW;
            const int fy = kDoomOffsetY + dy;
            // Scanlines: Fine = 1 dark per 3 rows (thin gap), Coarse = 1 dark per 2 rows (thick gap)
            const bool dimRow = (lcd == 1 && (coarse ? (dy & 1) : (dy % 3 == 0)));
            // Interlaced: 2-field alternation each display frame.
            // Fine: 2-row bands. Coarse: 4-row bands. Both 50/50 split (2-field).
            // >>1 on frame counter: each field holds 2 render frames → ~30 Hz flip,
            // visible to direct gaze (was 60 Hz = above fusion threshold).
            const bool interlaceHide = (lcd == 7) && (coarse
                ? (((dy >> 2) & 1) != ((s_frame_cnt >> 1) & 1))
                : (((dy >> 1) & 1) != ((s_frame_cnt >> 1) & 1)));
            for (int dx = 0; dx < kScaledW; ++dx) {
                const int sx = dx * kDoomW / kScaledW;
                tsl::Color col(g_palette_lut[srow[sx]]);

                // Chromatic aberration: R/B channels sampled from offset source pixels
                if (lcd == 6) {
                    const int off = coarse ? 2 : 1;
                    const int sx_r = std::max(0, std::min(kDoomW - 1, (dx - off) * kDoomW / kScaledW));
                    const int sx_b = std::max(0, std::min(kDoomW - 1, (dx + off) * kDoomW / kScaledW));
                    col.r = tsl::Color(g_palette_lut[srow[sx_r]]).r;
                    col.b = tsl::Color(g_palette_lut[srow[sx_b]]).b;
                }

                // Phosphor / tint filters: desaturate to luma then tint
                if (filter > 0) {
                    const u8 luma = static_cast<u8>((col.r * 2 + col.g * 5 + col.b) >> 3);
                    // Phosphor contrast snap: real tubes bloom at high brightness
                    const u8 snap = luma > 10 ? static_cast<u8>(std::min(15, int(luma) + 1)) : luma;
                    switch (filter) {
                        case 1: col.r = snap >> 3; col.g = snap; col.b = 0; break;  // Green P31
                        case 2: col.r = snap; col.g = static_cast<u8>((snap * 11) >> 4); col.b = 0; break;  // Amber P3
                        case 3: col.r = col.g = col.b = luma; break;  // White
                        case 4: col.r = static_cast<u8>(std::min(15, luma * 18 >> 4));  // Sepia
                                col.g = static_cast<u8>(std::min(15, luma * 14 >> 4));
                                col.b = static_cast<u8>(luma * 11 >> 4); break;
                        case 5: col.r = luma; col.g = luma >> 2; col.b = luma >> 3; break;  // Red phosphor
                        case 6: col.r = snap >> 3; col.g = snap >> 1; col.b = snap; break;  // Blue phosphor
                        case 7: col.r = 0; col.g = snap; col.b = snap; break;  // Cyan phosphor
                        case 8: col.r = snap; col.g = snap >> 3; col.b = snap; break;  // Purple phosphor
                        case 9: { u8 nv = snap < 4 ? u8(0) : u8(std::min(15, (int(snap) - 3) * 9 >> 3));  // Night vision
                                  col.r = 0; col.g = nv; col.b = 0; break; }
                        case 10: col.r = u8(15 - col.r); col.g = u8(15 - col.g); col.b = u8(15 - col.b); break;  // Invert
                        case 11: { const u8 vb = luma > 2 ? u8(std::min(15, int(luma) + 2)) : 0;  // Virtual Boy
                                   col.r = vb; col.g = 0; col.b = 0; break; }
                        case 12: { static constexpr u8 kTR[16] = {  0, 1, 3, 2, 0, 0, 0, 0, 0, 4,10,13,15,15,15,15 };  // Thermal
                                   static constexpr u8 kTG[16] = {  0, 0, 0, 0, 0, 3, 8,12,15,15,15,11, 6, 2, 0,15 };
                                   static constexpr u8 kTB[16] = {  0, 2, 5, 8,13,12,10, 6, 4, 0, 0, 0, 0, 0, 0,15 };
                                   col.r = kTR[luma]; col.g = kTG[luma]; col.b = kTB[luma]; break; }
                    }
                }

                // Scanlines (lcd==1): dark gap rows + brightness boost on lit rows
                if (lcd == 1) {
                    if (dimRow) {
                        // Gap: Fine=25%, Coarse=~12%
                        const int shift = coarse ? 3 : 2;
                        col = tsl::Color({u8(col.r >> shift), u8(col.g >> shift), u8(col.b >> shift), col.a});
                    } else {
                        // Lit row boost ×1.25 to compensate for dark gaps
                        col.r = u8(std::min(15, int(col.r) * 5 >> 2));
                        col.g = u8(std::min(15, int(col.g) * 5 >> 2));
                        col.b = u8(std::min(15, int(col.b) * 5 >> 2));
                    }
                }

                // LCD grid — pitch-based dark border + sub-pixel RGB columns (coarse)
                // Fine (pitch=3): 2×2 lit interior, 1px near-black border
                // Coarse (pitch=6): 5×5 lit interior, 1px near-black border, RGB sub-pixels
                if (lcd == 2) {
                    const int pitch      = coarse ? 6 : 3;
                    const bool on_border = ((dx % pitch) == pitch - 1) || ((dy % pitch) == pitch - 1);
                    if (on_border) {
                        col = tsl::Color({u8(col.r >> 3), u8(col.g >> 3), u8(col.b >> 3), col.a});
                    } else if (coarse) {
                        switch (dx % 3) {
                            case 0: col.g = u8(col.g * 12 >> 4); col.b = u8(col.b * 12 >> 4); break;
                            case 1: col.r = u8(col.r * 12 >> 4); col.b = u8(col.b * 12 >> 4); break;
                            case 2: col.r = u8(col.r * 12 >> 4); col.g = u8(col.g * 12 >> 4); break;
                        }
                    }
                }

                // Dot matrix — radial circular dots, near-black gaps
                // Fine (pitch=3): cross dot, dot_r2=1 → 5/9 coverage
                // Coarse (pitch=6): round dot, dot_r2=7 → 21/36 coverage
                if (lcd == 3) {
                    const int pitch  = coarse ? 6 : 3;
                    const int cx     = (dx % pitch) - pitch / 2;
                    const int cy     = (dy % pitch) - pitch / 2;
                    const int r2     = cx * cx + cy * cy;
                    const int dot_r2 = coarse ? 7 : 1;
                    if (r2 > dot_r2)
                        col = tsl::Color({u8(col.r >> 3), u8(col.g >> 3), u8(col.b >> 3), col.a});
                }

                // CRT — Fine: Trinitron aperture grille + every-other-row gap
                //        Coarse: shadow mask — staggered hex RGB dots, 2-of-4 row gaps
                if (lcd == 4) {
                    if (coarse) {
                        // Shadow mask: 4-row period — rows 0,1 lit; rows 2,3 dark
                        const int rowPhase = dy % 4;
                        if (rowPhase >= 2) {
                            col.r >>= 4; col.g >>= 4; col.b >>= 4;  // ~6% — shadow gap
                        } else {
                            // Odd lit rows offset sub-pixel columns by 1 for hex stagger
                            const int sub = (dx + (rowPhase & 1)) % 3;
                            switch (sub) {
                                case 0: col.g = u8(col.g * 11 >> 4); col.b = u8(col.b * 11 >> 4); break;
                                case 1: col.r = u8(col.r * 11 >> 4); col.b = u8(col.b * 11 >> 4); break;
                                case 2: col.r = u8(col.r * 11 >> 4); col.g = u8(col.g * 11 >> 4); break;
                            }
                        }
                    } else {
                        if (dy & 1) {
                            col.r >>= 2; col.g >>= 2; col.b >>= 2;  // 25% scanline gap
                        } else {
                            switch (dx % 3) {
                                case 0: col.g = u8(col.g * 12 >> 4); col.b = u8(col.b * 12 >> 4); break;
                                case 1: col.r = u8(col.r * 12 >> 4); col.b = u8(col.b * 12 >> 4); break;
                                case 2: col.r = u8(col.r * 12 >> 4); col.g = u8(col.g * 12 >> 4); break;
                            }
                        }
                    }
                }

                // Game Boy — 4-shade quantize + DMG (Fine) or GBP (Coarse) palette + pixel grid
                if (lcd == 5) {
                    const u8 luma = static_cast<u8>((col.r * 2 + col.g * 5 + col.b) >> 3);
                    const int shade = luma >> 2;  // 0-15 → 0-3
                    static constexpr u8 kDmgR[4] = { 1,  3,  7, 11 };  // DMG: olive-green
                    static constexpr u8 kDmgG[4] = { 2,  5, 10, 13 };
                    static constexpr u8 kDmgB[4] = { 0,  1,  2,  4 };
                    static constexpr u8 kGbpR[4] = { 1,  4,  9, 14 };  // GBP: cool gray
                    static constexpr u8 kGbpG[4] = { 1,  5, 10, 14 };
                    static constexpr u8 kGbpB[4] = { 1,  4,  9, 13 };
                    col.r = coarse ? kGbpR[shade] : kDmgR[shade];
                    col.g = coarse ? kGbpG[shade] : kDmgG[shade];
                    col.b = coarse ? kGbpB[shade] : kDmgB[shade];
                    // 4px pitch pixel grid — 3px lit, 1px near-black border
                    if ((dx & 3) == 3 || (dy & 3) == 3)
                        col = tsl::Color({u8(col.r >> 2), u8(col.g >> 2), u8(col.b >> 2), col.a});
                }

                // Interlaced — dark field at 50% (>>1) not 25% (>>2): 2:1 contrast
                // ratio vs 4:1. Lower contrast = less visible flicker = more stable.
                if (interlaceHide) {
                    col.r >>= 1; col.g >>= 1; col.b >>= 1;
                }

                // Trinitron aperture grille — vertical phosphor stripes + thin wire seams.
                // Fine (pitch=3): R-G-seam; tight pitch, no horizontal wires.
                // Coarse (pitch=4): R-G-B-seam; one clean RGB triad per group.
                //   Old pitch=6 gave R-G-B-R-G-seam = 2:2:1 R:G:B ratio (blue underrepresented).
                //   pitch=4 gives a perfect 1:1:1 triad. Boost ×21/16≈1.31 for 3/4 fill.
                if (lcd == 9) {
                    if (coarse) {
                        const int phase = dx % 4;
                        if (phase == 3) {
                            col.r >>= 3; col.g >>= 3; col.b >>= 3;  // vertical wire
                        } else {
                            switch (phase) {
                                case 0: col.g = u8(col.g * 13 >> 4); col.b = u8(col.b * 13 >> 4); break;
                                case 1: col.r = u8(col.r * 13 >> 4); col.b = u8(col.b * 13 >> 4); break;
                                case 2: col.r = u8(col.r * 13 >> 4); col.g = u8(col.g * 13 >> 4); break;
                            }
                            col.r = u8(std::min(15, int(col.r) * 21 >> 4));
                            col.g = u8(std::min(15, int(col.g) * 21 >> 4));
                            col.b = u8(std::min(15, int(col.b) * 21 >> 4));
                        }
                        // Horizontal shadow wires at 1/3 and 2/3 of viewport height
                        if (dy == 112 || dy == 224) { col.r >>= 2; col.g >>= 2; col.b >>= 2; }
                    } else {
                        const int pitch = 3;
                        if ((dx % pitch) == pitch - 1) {
                            col.r >>= 3; col.g >>= 3; col.b >>= 3;
                        } else {
                            switch (dx % 3) {
                                case 0: col.g = u8(col.g * 13 >> 4); col.b = u8(col.b * 13 >> 4); break;
                                case 1: col.r = u8(col.r * 13 >> 4); col.b = u8(col.b * 13 >> 4); break;
                                case 2: col.r = u8(col.r * 13 >> 4); col.g = u8(col.g * 13 >> 4); break;
                            }
                            col.r = u8(std::min(15, int(col.r) * 9 >> 3));
                            col.g = u8(std::min(15, int(col.g) * 9 >> 3));
                            col.b = u8(std::min(15, int(col.b) * 9 >> 3));
                        }
                    }
                }

                // Halftone — luma-scaled circular dots; Fine=dark BG pop-art, Coarse=warm paper
                if (lcd == 10) {
                    const int pitch  = coarse ? 6 : 4;
                    const int cx     = dx % pitch - pitch / 2;
                    const int cy     = dy % pitch - pitch / 2;
                    const int r2     = cx * cx + cy * cy;
                    const u8  luma   = u8((col.r * 2 + col.g * 5 + col.b) >> 3);
                    const int max_r2 = (pitch / 2) * (pitch / 2);
                    const int dot_r2 = int(luma) * max_r2 / 15;
                    if (r2 > dot_r2) {
                        if (coarse) { col.r = 14; col.g = 13; col.b = 12; }  // warm paper
                        else        { col.r =  1; col.g =  1; col.b =  1; }  // near-black
                    }
                }

                // Composite NTSC — horizontal chroma blur on R+B, luma (G) stays sharp
                if (lcd == 8) {
                    static int s_prv_r = 0, s_prv_b = 0;
                    if (dx == 0) { s_prv_r = col.r; s_prv_b = col.b; }
                    const int blr = coarse ? (col.r + s_prv_r + s_prv_r) / 3
                                           : (col.r + s_prv_r) >> 1;
                    const int blb = coarse ? (col.b + s_prv_b + s_prv_b) / 3
                                           : (col.b + s_prv_b) >> 1;
                    s_prv_r = col.r; s_prv_b = col.b;
                    col.r = static_cast<u8>(blr);
                    col.b = static_cast<u8>(blb);
                }

                // Vignette: 2D radial falloff (fixed-point 0-16, 8× downsampled LUT)
                if (vign) {
                    const int vs = s_vig_lut[dy >> 3][dx >> 3];
                    col.r = static_cast<u8>((col.r * vs) >> 4);
                    col.g = static_cast<u8>((col.g * vs) >> 4);
                    col.b = static_cast<u8>((col.b * vs) >> 4);
                }

                // Gamma: linear brightness scale. gamma_mul=8 → 1× (identity).
                // gamma_mul=16 → 2× (bright); gamma_mul=4 → 0.5× (dark). Clamp to 15.
                if (gamma_mul != 16) {
                    col.r = static_cast<u8>(std::min(15, (col.r * gamma_mul) >> 4));
                    col.g = static_cast<u8>(std::min(15, (col.g * gamma_mul) >> 4));
                    col.b = static_cast<u8>(std::min(15, (col.b * gamma_mul) >> 4));
                }

                // Bayer 4×4 ordered dither — 0/+1 threshold, channels staggered
                if (dither) {
                    const int b = kBayer4[dy & 3][dx & 3];
                    if (b >= 8  && col.r < 15) col.r++;
                    if (((b + 5) & 15) >= 8 && col.g < 15) col.g++;
                    if (((b + 11) & 15) >= 8 && col.b < 15) col.b++;
                }

                renderer->setPixel(dx, fy, col);
            }
        }
    }

    // Memory counter — newlib (mostly static post-init) + Doom Z_Malloc zone
    // (visibly fluctuates as lump/sound cache loads + evicts).
    {
        static u64 mem_last_sample_ns = 0;
        static char mem_label[112] = "mem: …";
        const u64 now_ns = armTicksToNs(armGetSystemTick());
        if (now_ns - mem_last_sample_ns > 1'000'000'000ULL) {
            mem_last_sample_ns = now_ns;
            const struct mallinfo mi = mallinfo();
            const size_t newlib_used_kb = static_cast<size_t>(mi.uordblks) / 1024;
            const size_t newlib_free_kb = static_cast<size_t>(mi.fordblks) / 1024;
            const size_t zone_free_kb   =
                g_doom_initialized
                ? static_cast<size_t>(Z_FreeMemory()) / 1024
                : 0;
            std::snprintf(mem_label, sizeof(mem_label),
                          "newlib: %zuk used / %zuk free   doom zone: %zuk free",
                          newlib_used_kb, newlib_free_kb, zone_free_kb);
        }
        // Draw above where the footer separator would be (y=647), two lines stacked.
        renderer->drawString(mem_label, false, 20, 618, 14,
                             tsl::Color(0xCFFF));

        // Build identity — git branch@hash[+] from Makefile -DBUILD_ID, plus
        // the live audio backend (OGG decoder vs OPL fallback). Lets us tell
        // at a glance which build is on the Switch and which engine is
        // currently producing music — these answers used to require trace.log.
#ifdef BUILD_ID
        char build_line[160];
        std::snprintf(build_line, sizeof(build_line),
                      "build: " BUILD_ID "   audio: %s",
                      music_ogg_current_backend());
        renderer->drawString(build_line, false, 20, 634, 13,
                             tsl::Color(0x9FFF));
#endif

        // FPS counter — EMA smoothed, top-right corner of game viewport.
        if (g_show_fps) {
            static u64  fps_last_ns  = 0;
            static float fps_smooth  = 35.0f;
            static char  fps_label[12] = "-- fps";
            const u64 now_ns = armTicksToNs(armGetSystemTick());
            if (fps_last_ns != 0) {
                const u64 delta = now_ns - fps_last_ns;
                if (delta > 0) {
                    fps_smooth = fps_smooth * 0.85f + (1e9f / static_cast<float>(delta)) * 0.15f;
                    std::snprintf(fps_label, sizeof(fps_label), "%.0f fps", fps_smooth);
                }
            }
            fps_last_ns = now_ns;
            renderer->drawString(fps_label, false,
                                 kDoomOffsetX + kScaledW - 52,
                                 kDoomOffsetY + 20,
                                 14, tsl::Color(0xCFFF));
        }
    }
}

// Single content element for the Doom GUI: game viewport + quick settings panel.
class DoomElement final : public tsl::elm::Element {
public:
    void draw(tsl::gfx::Renderer* renderer) override {
        // Game viewport + memory line.
        draw_doom_viewport(renderer, getX(), getY(), getWidth(), getHeight());

        if (!g_doom_initialized || g_doom_failed) return;

        // Quick Settings panel — drawn directly, no tsl::changeTo (crashes on Switch).
        if (g_config_open) {
            constexpr s32 kPanelX = 20;
            constexpr s32 kPanelY = kDoomOffsetY + 8;
            constexpr s32 kPanelW = kScaledW - 40;
            constexpr s32 kItemH  = 28;
            constexpr s32 kItemsY = kPanelY + 46;
            const int     nItems  = (g_config_mode == 0) ? kConfigItemCount :
                                    (g_config_mode == 1) ? kCheatCount : kWarpPageSize;

            renderer->drawRect(kPanelX - 4, kPanelY - 4,
                               kPanelW + 8, kItemH * nItems + 70,
                               tsl::Color(0x000F));

            static const std::vector<std::string> kSettingsGlyphs = {"\uE0E1", "\uE0E2", "\uE0E3", "\uE0ED", "\uE0EE"};
            static const std::vector<std::string> kABGlyphs        = {"\uE0E1", "\uE0E0"};

            if (g_config_mode == 0) {
                renderer->drawString("QUICK SETTINGS", false, kPanelX + 4, kPanelY + 20,
                                     20, tsl::Color(0xFFFF));
                static constexpr const char* kItemLabels[] = {
                    "SFX Volume", "Music Volume", "Game Vol",
                    "Gamma", "LCD Effect", "Effect Width", "Color Filter",
                    "Vignette", "Dither", "FPS Counter", "Screensaver",
                    "Idle Timer", "Music Player", "Cheats"
                };
                for (int i = 0; i < kConfigItemCount; ++i) {
                    const s32 iy = kItemsY + i * kItemH;
                    if (i == g_config_selected)
                        renderer->drawRect(kPanelX - 2, iy - 18, kPanelW + 4, kItemH,
                                           tsl::Color(0x444F));
                    const tsl::Color col = (i == g_config_selected)
                                           ? tsl::Color(0xFFFF) : tsl::Color(0xAAAF);
                    renderer->drawString(kItemLabels[i], false, kPanelX + 4, iy, 18, col);
                    char val[24];
                    switch (i) {
                        case 0:  if (g_sfx_muted)   std::snprintf(val, sizeof(val), "Mute");
                                 else                std::snprintf(val, sizeof(val), "%d", g_sfx_volume);   break;
                        case 1:  if (g_music_muted) std::snprintf(val, sizeof(val), "Mute");
                                 else                std::snprintf(val, sizeof(val), "%d", g_music_volume); break;
                        case 2:  if (g_game_muted) std::snprintf(val, sizeof(val), "Mute");
                                 else std::snprintf(val, sizeof(val), "%d", g_game_volume);         break;
                        case 3:  std::snprintf(val, sizeof(val), "%d", g_gamma);                           break;
                        case 4:  std::snprintf(val, sizeof(val), "%s", kLcdEffectNames[g_lcd_effect]);     break;
                        case 5:  std::snprintf(val, sizeof(val), "%s", kLcdWidthNames[g_lcd_width]);       break;
                        case 6:  std::snprintf(val, sizeof(val), "%s", kColorFilterNames[g_color_filter]); break;
                        case 7:  std::snprintf(val, sizeof(val), "%s", g_vignette ? "On" : "Off");         break;
                        case 8:  std::snprintf(val, sizeof(val), "%s", g_dither   ? "On" : "Off");         break;
                        case 9:  std::snprintf(val, sizeof(val), "%s", g_show_fps ? "On" : "Off");         break;
                        case 10: std::snprintf(val, sizeof(val), "%s", g_screensaver_enabled ? "On" : "Off"); break;
                        case 11: std::snprintf(val, sizeof(val), "%d min", g_screensaver_minutes);          break;
                        case 12: std::snprintf(val, sizeof(val), "%s", "\xe2\x96\xb6");                     break;
                        case 13: std::snprintf(val, sizeof(val), "%s", "\xe2\x96\xb6");                     break;
                        default: val[0] = '\0'; break;
                    }
                    renderer->drawString(val, false, kPanelX + kPanelW - 90, iy, 18, col);
                }
                renderer->drawStringWithColoredSections(
                    (g_config_selected <= 2)
                        ? "\uE0E1 Close   \uE0ED/\uE0EE Change   \uE0E2 Screenshot   \uE0E3 Mute"
                        : "\uE0E1 Close   \uE0ED/\uE0EE Change   \uE0E2 Screenshot",
                    false, kSettingsGlyphs,
                    kPanelX + 4, kItemsY + kConfigItemCount * kItemH + 8,
                    15, tsl::bottomTextColor, tsl::buttonColor);
            } else if (g_config_mode == 1) {
                renderer->drawString("CHEATS", false, kPanelX + 4, kPanelY + 20,
                                     20, tsl::Color(0xFFFF));
                for (int i = 0; i < kCheatCount; ++i) {
                    const s32 iy = kItemsY + i * kItemH;
                    if (i == g_config_selected)
                        renderer->drawRect(kPanelX - 2, iy - 18, kPanelW + 4, kItemH,
                                           tsl::Color(0x444F));
                    const tsl::Color col = (i == g_config_selected)
                                           ? tsl::Color(0xFFFF) : tsl::Color(0xAAAF);
                    renderer->drawString(kCheats[i].label, false, kPanelX + 4, iy, 18, col);
                }
                renderer->drawStringWithColoredSections(
                    "\uE0E1 Back   \uE0E0 Activate",
                    false, kABGlyphs,
                    kPanelX + 4, kItemsY + kCheatCount * kItemH + 8,
                    15, tsl::bottomTextColor, tsl::buttonColor);
            } else if (g_config_mode == 4) {
                // Music player — grouped by episode subfolder
                renderer->drawString("MUSIC PLAYER", false, kPanelX + 4, kPanelY + 20,
                                     20, tsl::Color(0xFFFF));
                const int total4 = (int)g_music_items.size();
                const int disp4  = std::min(total4 - g_warp_scroll, kMusicPageSize);
                for (int i = 0; i < disp4; ++i) {
                    const int idx = g_warp_scroll + i;
                    if (idx >= total4) break;
                    const auto& mi = g_music_items[idx];
                    const s32 iy   = kItemsY + i * kItemH;
                    if (mi.track_idx == -2) {
                        // Section header: smaller, dimmer, no highlight
                        renderer->drawString(mi.label.c_str(), false,
                                             kPanelX + 4, iy, 14, tsl::Color(0x888F));
                        continue;
                    }
                    const bool sel    = (idx == g_config_selected);
                    const bool active = (mi.track_idx == -1 && g_track_idx < 0) ||
                                        (mi.track_idx >= 0 && mi.track_idx == g_track_idx);
                    if (sel)
                        renderer->drawRect(kPanelX - 2, iy - 18, kPanelW + 4, kItemH,
                                           tsl::Color(0x444F));
                    const tsl::Color col = active ? tsl::Color(0xFF8F) :
                                           sel    ? tsl::Color(0xFFFF) : tsl::Color(0xAAAF);
                    renderer->drawString(mi.label.c_str(), false, kPanelX + 4, iy, 18, col);
                }
                renderer->drawStringWithColoredSections(
                    " Back    Play",
                    false, kABGlyphs,
                    kPanelX + 4, kItemsY + disp4 * kItemH + 8,
                    15, tsl::bottomTextColor, tsl::buttonColor);
            } else {
                // Warp picker — mode 2: episode (doom1) or map; mode 3: map after episode
                const bool ep_mode  = (g_config_mode == 2) && (gamemission == 0);
                const int  warp_total = ep_mode ? 4 : (g_config_mode == 3 ? 9 : 32);
                renderer->drawString(ep_mode ? "WARP: EPISODE" : "WARP: MAP",
                                     false, kPanelX + 4, kPanelY + 20, 20, tsl::Color(0xFFFF));
                const int disp = std::min(warp_total, kWarpPageSize);
                for (int i = 0; i < disp; ++i) {
                    const int idx = g_warp_scroll + i;
                    if (idx >= warp_total) break;
                    char label[20];
                    if (ep_mode)
                        std::snprintf(label, sizeof(label), "Episode %d", idx + 1);
                    else if (g_config_mode == 3)
                        std::snprintf(label, sizeof(label), "E%dM%d", g_warp_episode, idx + 1);
                    else
                        std::snprintf(label, sizeof(label), "MAP %02d", idx + 1);
                    const s32 iy = kItemsY + i * kItemH;
                    const bool sel = (idx == g_config_selected);
                    if (sel)
                        renderer->drawRect(kPanelX - 2, iy - 18, kPanelW + 4, kItemH,
                                           tsl::Color(0x444F));
                    renderer->drawString(label, false, kPanelX + 4, iy, 18,
                                         sel ? tsl::Color(0xFFFF) : tsl::Color(0xAAAF));
                }
                renderer->drawStringWithColoredSections(
                    "\uE0E1 Back   \uE0E0 Warp",
                    false, kABGlyphs,
                    kPanelX + 4, kItemsY + disp * kItemH + 8,
                    15, tsl::bottomTextColor, tsl::buttonColor);
            }
            return;
        }

        // Touch strip: SAVE / LOAD / QUIT — shown below viewport when in-game
        if (g_doom_initialized && !g_doom_failed && !g_config_open && gamestate == 0) {
            constexpr s32 kBtnY1 = 648, kBtnH = 71;
            struct { s32 x; const char* label; } kBtns[] = {
                {20, "SAVE"}, {160, "LOAD"}, {300, "QUIT"}
            };
            for (int i = 0; i < 3; ++i) {
                const tsl::Color bg = (g_touch_strip_btn == i)
                    ? tsl::Color(0xF666)
                    : tsl::Color(0xF333);
                renderer->drawRect(kBtns[i].x, kBtnY1, 127, kBtnH, bg);
                renderer->drawString(kBtns[i].label, false,
                    kBtns[i].x + 35, 693, 20, tsl::Color(0xFFFF));
            }
        }
    }

    void layout(u16, u16, u16, u16) override {}

    bool handleInput(u64, u64, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        return false;
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32, s32, s32, s32) override {
        if (!g_doom_initialized || g_doom_failed || g_config_open || gamestate != 0)
            return false;
        constexpr s32 kBtnY1 = 648, kBtnY2 = 719;
        if (currY < kBtnY1 || currY > kBtnY2) return false;
        const int btn = (currX >= 20  && currX < 148) ? 0
                      : (currX >= 160 && currX < 288) ? 1
                      : (currX >= 300 && currX < 428) ? 2 : -1;
        if (btn < 0) return false;
        if (event == tsl::elm::TouchEvent::Touch || event == tsl::elm::TouchEvent::Hold) {
            g_touch_strip_btn = btn;
        } else if (event == tsl::elm::TouchEvent::Release) {
            g_touch_strip_btn = -1;
            switch (btn) {
                case 0:
                    G_SaveGame(7, (char*)"");
                    break;
                case 1: {
                    const char* dir = D_SaveGameIWADName(gamemission);
                    if (dir) {
                        char savefile[256];
                        std::snprintf(savefile, sizeof(savefile),
                            "sdmc:/config/doom/saves/%s/doomsav7.dsg", dir);
                        G_LoadGame(savefile);
                        doomgeneric_switch_reanchor_clock();
                    }
                    break;
                }
                case 2:
                    G_SaveGame(7, (char*)"");
                    g_deferred_close_ticks = 4;
                    break;
            }
        } else {
            g_touch_strip_btn = -1;
        }
        return true;
    }
};


class DoomGui final : public tsl::Gui {
public:
    explicit DoomGui(std::string wad_path) : m_wadPath(std::move(wad_path)) {}

    ~DoomGui() { tsl::disableHiding = false; }

    tsl::elm::Element* createUI() override {
        tsl::disableHiding = true;   // combo must close (not hide) while game runs
        m_frame = new DoomOverlayFrame("", "Configure");

        m_doomElement = new DoomElement();
        m_frame->setContent(m_doomElement);
        return m_frame;
    }

    void update() override {
        // Lazy-init: doomgeneric_Create is heavy. Do it on first update() so
        // the framebuffer is already set up before we attempt to draw anything.
        if (!g_doom_initialized && !g_doom_failed) {
            g_screensaver_active     = false;
            g_screensaver_save_ticks = 0;
            g_last_input_ns          = 0;
            try_init_engine(m_wadPath.c_str());
            if (g_doom_initialized && m_frame)
                m_frame->setFooterHidden(true);
            return;
        }
        if (g_doom_failed) return;

        // Refresh longjmp recovery target every frame. try_init_engine()'s setjmp
        // stack frame is dead after that function returns. Any engine longjmp during
        // doomgeneric_Tick() (I_Error, I_Quit, or the patches/0002 exit() sites) would
        // otherwise jump into a dead frame → ARM unwind into garbage → Switch crash.
        // Refreshing here makes update()'s frame the valid target for this tick batch.
        // On catch: close the overlay immediately; cannot safely call back into the
        // engine after a longjmp.
        {
            const int jmp_code = setjmp(g_doom_error_jmp);
            if (jmp_code != 0) {
                doom_trace("engine longjmp caught in update — closing overlay");
                launchComboHasTriggered.store(true, std::memory_order_release);
                tsl::Overlay::get()->close();
                return;
            }
        }

        // Deferred close: G_SaveGame sets gameaction=ga_savegame; G_DoSaveGame runs on the
        // next G_Ticker. Wait a few ticks so the save completes before closing.
        if (g_deferred_close_ticks > 0 && --g_deferred_close_ticks == 0) {
            doom_trace("deferred close: save complete — closing overlay");
            launchComboHasTriggered.store(true, std::memory_order_release);
            tsl::Overlay::get()->close();
            return;
        }

        // Pause engine while Quick Settings is open; re-anchor clock on resume.
        if (g_config_open) return;

        // Screensaver idle check — only when in-game (gamestate==0) and not already active.
        if (g_screensaver_enabled && !g_screensaver_active && gamestate == 0) {
            const u64 now_check = armTicksToNs(armGetSystemTick());
            if (g_last_input_ns == 0) g_last_input_ns = now_check;
            const u64 idle_threshold_ns = static_cast<u64>(g_screensaver_minutes) * 60ULL * 1'000'000'000ULL;
            if (now_check - g_last_input_ns >= idle_threshold_ns) {
                doom_trace("screensaver: idle threshold reached — autosaving then starting title");
                G_SaveGame(7, (char*)"");
                g_screensaver_save_ticks = 4;
                g_screensaver_active = true;
                g_last_input_ns = now_check;  // reset so we don't re-trigger immediately on wake
            }
        }

        // Screensaver deferred D_StartTitle after save completes.
        if (g_screensaver_save_ticks > 0 && --g_screensaver_save_ticks == 0) {
            doom_trace("screensaver: save complete — calling D_StartTitle");
            D_StartTitle();
        }

        // 35 Hz tick accumulator. Cap at 4 catch-up ticks to avoid spiral of death.
        static u64 last_ns  = 0;
        static u64 accum_ns = 0;
        constexpr u64 kTickPeriodNs    = 1'000'000'000ULL / 35ULL;
        constexpr int kMaxCatchupTicks = 4;
        constexpr u64 kCatchupCapNs    = kTickPeriodNs * kMaxCatchupTicks;

        u64 now_ns = armTicksToNs(armGetSystemTick());
        if (last_ns == 0) last_ns = now_ns;
        const u64 delta_ns = now_ns - last_ns;
        last_ns = now_ns;

        // Resume-from-dismiss: discard giant gaps so we don't try to catch up
        // across the entire dismissal interval (engine clock re-anchored in onShow).
        if (delta_ns > kCatchupCapNs) {
            accum_ns = 0;
        } else {
            accum_ns += delta_ns;
        }

        int ticks_run = 0;
        while (accum_ns >= kTickPeriodNs && ticks_run < kMaxCatchupTicks) {
            doomgeneric_Tick();
            accum_ns -= kTickPeriodNs;
            ticks_run++;
        }
        if (accum_ns >= kTickPeriodNs * kMaxCatchupTicks) {
            accum_ns = 0;
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        u64 keysUp = m_prevKeysHeld & ~keysHeld;
        m_prevKeysHeld = keysHeld;
        const bool simulatedNext = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel);

        // Reset idle screensaver timer on any button activity.
        if (keysDown && g_doom_initialized && !g_doom_failed)
            g_last_input_ns = armTicksToNs(armGetSystemTick());

        // Close overlay with the user's configured Ultrahand launch combo.
        // Same combo that opened the overlay — intuitive and user-configurable.
        const bool launchCombo =
            (keysDown & tsl::cfg::launchCombo) &&
            (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo);
        if (launchCombo) {
            g_config_open = false;
            title_vol_restore();
            if (g_doom_initialized && !g_doom_failed && gamestate == 0) {
                doom_trace("launch combo — autosave then close");
                G_SaveGame(7, (char*)"");
                g_deferred_close_ticks = 4;
            } else {
                doom_trace("launch combo — closing overlay (not in game, no autosave)");
                tsl::Overlay::get()->close();
            }
            return true;
        }
        if (g_config_open) {
            if ((keysDown & HidNpadButton_A) || simulatedNext) {
                if (g_config_mode == 0) {
                    if (g_config_selected == kConfigItemCount - 1) {
                        g_config_mode = 1; g_config_selected = 0;
                    } else if (g_config_selected == kConfigItemCount - 2) {
                        // Enter music player — build display list, pre-scroll to active track
                        build_music_items();
                        g_config_mode = 4;
                        g_config_selected = 0;
                        for (int mi = 0; mi < (int)g_music_items.size(); ++mi) {
                            if ((g_track_idx < 0 && g_music_items[mi].track_idx == -1) ||
                                (g_track_idx >= 0 && g_music_items[mi].track_idx == g_track_idx)) {
                                g_config_selected = mi; break;
                            }
                        }
                        const int total4 = (int)g_music_items.size();
                        g_warp_scroll = std::max(0, std::min(g_config_selected - kMusicPageSize / 2,
                                                             total4 - kMusicPageSize));
                    }
                } else if (g_config_mode == 4) {
                    // Select track and play immediately
                    const auto& mi = g_music_items[g_config_selected];
                    if (mi.track_idx == -1) {
                        g_track_idx = -1;
                    } else if (mi.track_idx >= 0) {
                        g_track_idx = mi.track_idx;
                        music_ogg_force_track(g_track_list[mi.track_idx].path.c_str());
                    }
                    if (mi.track_idx != -2) {  // don't close on header press
                        save_config();
                        g_config_mode = 0;
                        g_config_selected = kConfigItemCount - 2;
                    }
                } else if (g_config_mode == 1) {
                    // "Level Warp" entry — enter warp picker
                    if (kCheats[g_config_selected].seq[0] == '\0') {
                        g_config_mode = 2;
                        g_config_selected = 0;
                        g_warp_scroll = 0;
                        g_warp_episode = 0;
                    } else {
                        if (g_doom_initialized && !g_doom_failed)
                            doomgeneric_switch_push_cheat(kCheats[g_config_selected].seq);
                        g_config_open = false;
                        doomgeneric_switch_reanchor_clock();
                    }
                } else if (g_config_mode == 2) {
                    if (gamemission == 0) {
                        // Doom 1: episode chosen → go to map picker
                        g_warp_episode = g_config_selected + 1;
                        g_config_mode = 3;
                        g_config_selected = 0;
                        g_warp_scroll = 0;
                    } else {
                        // Doom 2: map chosen → warp
                        const int m = g_config_selected + 1;
                        char seq[12];
                        std::snprintf(seq, sizeof(seq), "idclev%d%d", m / 10, m % 10);
                        if (g_doom_initialized && !g_doom_failed)
                            doomgeneric_switch_push_cheat(seq);
                        g_config_open = false;
                        doomgeneric_switch_reanchor_clock();
                    }
                } else if (g_config_mode == 3) {
                    // Doom 1: map chosen → warp
                    char seq[12];
                    std::snprintf(seq, sizeof(seq), "idclev%d%d", g_warp_episode, g_config_selected + 1);
                    if (g_doom_initialized && !g_doom_failed)
                        doomgeneric_switch_push_cheat(seq);
                    g_config_open = false;
                    doomgeneric_switch_reanchor_clock();
                }
            } else if ((keysDown & HidNpadButton_Y) && g_config_mode == 0 && g_config_selected <= 2) {
                switch (g_config_selected) {
                    case 0:
                        if (g_sfx_muted) {
                            g_sfx_volume = g_sfx_pre_mute;
                            g_sfx_muted  = false;
                        } else {
                            g_sfx_pre_mute = g_sfx_volume;
                            g_sfx_volume   = 0;
                            g_sfx_muted    = true;
                        }
                        if (g_doom_initialized && !g_doom_failed) S_SetSfxVolume(g_sfx_volume * 127 / 15);
                        save_config();
                        break;
                    case 1:
                        if (g_music_muted) {
                            g_music_volume = g_music_pre_mute;
                            g_music_muted  = false;
                        } else {
                            g_music_pre_mute = g_music_volume;
                            g_music_volume   = 0;
                            g_music_muted    = true;
                        }
                        if (g_doom_initialized && !g_doom_failed) S_SetMusicVolume(g_music_volume * 127 / 15);
                        save_config();
                        break;
                    case 2:
                        if (g_game_muted) {
                            g_game_volume = g_game_pre_mute;
                            g_game_muted  = false;
                        } else {
                            g_game_pre_mute = g_game_volume;
                            g_game_volume   = 0;
                            g_game_muted    = true;
                        }
                        title_vol_apply(g_game_volume);
                        save_config();
                        break;
                }
            } else if ((keysDown & HidNpadButton_B) || simulatedNext) {
                if (g_config_mode == 1)      { g_config_mode = 0; g_config_selected = 13; }
                else if (g_config_mode == 4) { g_config_mode = 0; g_config_selected = 12; g_warp_scroll = 0; }
                else if (g_config_mode == 2) { g_config_mode = 1; g_config_selected = kCheatCount - 1; g_warp_scroll = 0; }
                else if (g_config_mode == 3) { g_config_mode = 2; g_config_selected = g_warp_episode - 1; g_warp_scroll = 0; }
                else                         { g_config_open = false; doomgeneric_switch_reanchor_clock(); }
            } else if (keysDown & HidNpadButton_Minus) {
                g_config_open = false;
                doomgeneric_switch_reanchor_clock();
            } else if ((keysDown & HidNpadButton_X) && g_config_mode == 0) {
                take_screenshot();
            } else if (keysDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                const int maxSel = (g_config_mode == 0) ? kConfigItemCount - 1 :
                                   (g_config_mode == 1) ? kCheatCount - 1 :
                                   (g_config_mode == 2 && gamemission == 0) ? 3 :
                                   (g_config_mode == 3) ? 8 : 31;
                if (g_config_mode == 4) {
                    // Music player: wrap + skip section headers
                    const int tot = (int)g_music_items.size();
                    int next = (g_config_selected + 1) % tot;
                    for (int guard = tot; guard-- > 0 && g_music_items[next].track_idx == -2;)
                        next = (next + 1) % tot;
                    g_config_selected = next;
                    if (next == 0) g_warp_scroll = 0;
                    else if (next >= g_warp_scroll + kMusicPageSize) ++g_warp_scroll;
                } else if (g_config_mode == 0 || g_config_mode == 1) {
                    // Wrap-around navigation
                    g_config_selected = (g_config_selected >= maxSel) ? 0 : g_config_selected + 1;
                } else {
                    if (g_config_selected < maxSel) {
                        ++g_config_selected;
                        if (g_config_selected >= g_warp_scroll + kWarpPageSize) ++g_warp_scroll;
                    }
                }
            } else if (keysDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                const int total4 = (int)g_music_items.size();
                const int maxSel = (g_config_mode == 0) ? kConfigItemCount - 1 :
                                   (g_config_mode == 1) ? kCheatCount - 1 :
                                   (g_config_mode == 4) ? total4 - 1 :
                                   (g_config_mode == 2 && gamemission == 0) ? 3 :
                                   (g_config_mode == 3) ? 8 : 31;
                if (g_config_mode == 4) {
                    // Music player: wrap + skip section headers
                    const int tot = (int)g_music_items.size();
                    int prev = (g_config_selected + tot - 1) % tot;
                    for (int guard = tot; guard-- > 0 && g_music_items[prev].track_idx == -2;)
                        prev = (prev + tot - 1) % tot;
                    g_config_selected = prev;
                    if (prev >= tot - kMusicPageSize)
                        g_warp_scroll = std::max(0, tot - kMusicPageSize);
                    else if (prev < g_warp_scroll) --g_warp_scroll;
                } else if (g_config_mode == 0 || g_config_mode == 1) {
                    // Wrap-around navigation
                    g_config_selected = (g_config_selected <= 0) ? maxSel : g_config_selected - 1;
                } else {
                    if (g_config_selected > 0) {
                        --g_config_selected;
                        if (g_config_selected < g_warp_scroll) --g_warp_scroll;
                    }
                }
            } else if ((keysDown & (HidNpadButton_L | HidNpadButton_Left)) && g_config_mode == 0) {
                switch (g_config_selected) {
                    case 0:  g_sfx_volume   = std::max(0, g_sfx_volume   - 1); if (g_doom_initialized && !g_doom_failed) S_SetSfxVolume(g_sfx_volume * 127 / 15);   save_config(); break;
                    case 1:  g_music_volume = std::max(0, g_music_volume - 1); if (g_doom_initialized && !g_doom_failed) S_SetMusicVolume(g_music_volume * 127 / 15); save_config(); break;
                    case 2:  g_game_volume  = std::max(0, g_game_volume  - 1); title_vol_apply(g_game_volume);                                                         save_config(); break;
                    case 3:  g_gamma    = std::max(0, g_gamma - 1); save_config(); break;
                    case 4:  g_lcd_effect = (g_lcd_effect + kLcdEffectCount - 1) % kLcdEffectCount; save_config(); break;
                    case 5:  g_lcd_width  = 1 - g_lcd_width; save_config(); break;
                    case 6:  g_color_filter = (g_color_filter + kColorFilterCount - 1) % kColorFilterCount; save_config(); break;
                    case 7:  g_vignette = !g_vignette; save_config(); break;
                    case 8:  g_dither   = !g_dither;   save_config(); break;
                    case 9:  g_show_fps = !g_show_fps; save_config(); break;
                    case 10: g_screensaver_enabled = !g_screensaver_enabled; save_config(); break;
                    case 11: g_screensaver_minutes = std::max(1, g_screensaver_minutes - 1); save_config(); break;
                }
            } else if ((keysDown & (HidNpadButton_R | HidNpadButton_Right)) && g_config_mode == 0) {
                switch (g_config_selected) {
                    case 0:  g_sfx_volume   = std::min(15, g_sfx_volume   + 1); if (g_doom_initialized && !g_doom_failed) S_SetSfxVolume(g_sfx_volume * 127 / 15);   save_config(); break;
                    case 1:  g_music_volume = std::min(15, g_music_volume + 1); if (g_doom_initialized && !g_doom_failed) S_SetMusicVolume(g_music_volume * 127 / 15); save_config(); break;
                    case 2:  g_game_volume  = std::min(15, g_game_volume  + 1); title_vol_apply(g_game_volume);                                                         save_config(); break;
                    case 3:  g_gamma    = std::min(15, g_gamma + 1); save_config(); break;
                    case 4:  g_lcd_effect = (g_lcd_effect + 1) % kLcdEffectCount; save_config(); break;
                    case 5:  g_lcd_width  = 1 - g_lcd_width; save_config(); break;
                    case 6:  g_color_filter = (g_color_filter + 1) % kColorFilterCount; save_config(); break;
                    case 7:  g_vignette = !g_vignette; save_config(); break;
                    case 8:  g_dither   = !g_dither;   save_config(); break;
                    case 9:  g_show_fps = !g_show_fps; save_config(); break;
                    case 10: g_screensaver_enabled = !g_screensaver_enabled; save_config(); break;
                    case 11: g_screensaver_minutes = std::min(5, g_screensaver_minutes + 1); save_config(); break;
                }
            }
            return true;
        }

        if (simulatedNext || ((keysDown & HidNpadButton_Minus) && !(keysHeld & HidNpadButton_Plus))) {
            scan_music_tracks();
            g_config_open     = true;
            g_config_mode     = 0;
            g_config_selected = 0;
            return true;
        }

        if (g_doom_initialized && !g_doom_failed) {
            // Screensaver wake: any button press while demo plays → load autosave.
            if (g_screensaver_active && keysDown) {
                g_screensaver_active = false;
                doom_trace("screensaver: input detected — loading autosave");
                const char* iwad_save_dir = D_SaveGameIWADName(gamemission);
                if (iwad_save_dir) {
                    char savefile[256];
                    std::snprintf(savefile, sizeof(savefile),
                                  "sdmc:/config/doom/saves/%s/doomsav7.dsg", iwad_save_dir);
                    G_LoadGame(savefile);
                }
                return true;
            }

            doom_input::dispatch(keysDown, keysUp);
        }

        return true;
    }

private:
    std::string       m_wadPath;
    DoomOverlayFrame* m_frame        = nullptr;
    DoomElement*      m_doomElement  = nullptr;
    u64               m_prevKeysHeld = 0;
};

class WadPickerGui final : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new DoomOverlayFrame("", "Settings");
        auto* list  = new tsl::elm::List();

        auto wads = scan_wads();
        if (wads.empty()) {
            list->addItem(new tsl::elm::ListItem("(no WADs found)"));
            list->addItem(new tsl::elm::ListItem("Put *.wad in:"));
            list->addItem(new tsl::elm::ListItem(kWadDir));
        } else {
            for (const auto& wad : wads) {
                auto* item = new tsl::elm::ListItem(wad.display_name, wad.filename);
                std::string fullpath = wad.fullpath;
                item->setClickListener([fullpath](u64 keys) -> bool {
                    if (keys & HidNpadButton_A) {
                        tsl::changeTo<DoomGui>(fullpath);
                        return true;
                    }
                    return false;
                });
                list->addItem(item);
            }
        }
        frame->setContent(list);
        return frame;
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        const bool simulatedNext = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel);
        if (simulatedNext || (keysDown & HidNpadButton_Right)) {
            tsl::changeTo<ConfigGui>();
            return true;
        }
        return false;
    }
};

// draw_doom_title — full-logo dynamic wave animation with Doom fire colors.
//   All 4 letters of "DOOM" get a staggered sine wave.
//   Dynamic: wave between doomRGB2 (blood red) and doomRGB1 (fire orange-red).
//   Static:  all letters in doomRGB1 (bright fire red).
static s32 __attribute__((optimize("O3"))) draw_doom_title(tsl::gfx::Renderer* renderer,
                                const s32 x, const s32 y, const u32 fontSize) {
    static const tsl::Color doomRGB1 = tsl::RGB888("D7BB43");  // gold
    static const tsl::Color doomRGB2 = tsl::RGB888("0000FF");  // blue

    static constexpr const char kTitle[] = "DOOM";

    char buf[2] = {0, 0};
    s32 cx = x;

    if (ult::useDynamicLogo) {
        static constexpr double kTwoPi = 2.0 * ult::_M_PI;
        const double t = ult::nowNs() / 1e9;
        float countOffset = 0.f;
        for (const char ch : kTitle) {
            if (ch == '\0') break;
            const float counter = static_cast<float>(kTwoPi) *
                                  static_cast<float>(std::fmod(t / 4.0 + countOffset, 2.0)) / 2.0f;
            const float tp = ult::cos(3.0f * (counter - static_cast<float>(kTwoPi) / 3.0f));
            const float bl = (tp + 1.0f) / 2.0f;
            const tsl::Color col = {
                static_cast<u8>((doomRGB1.r - doomRGB2.r) * bl + doomRGB2.r),
                static_cast<u8>((doomRGB1.g - doomRGB2.g) * bl + doomRGB2.g),
                static_cast<u8>((doomRGB1.b - doomRGB2.b) * bl + doomRGB2.b),
                0xF
            };
            buf[0] = ch;
            cx += renderer->drawString(buf, false, cx, y, fontSize, col).first;
            countOffset -= 0.2f;
        }
    } else {
        cx += renderer->drawString(kTitle, false, cx, y, fontSize, doomRGB1).first;
    }
    return cx;
}

class DoomOverlay final : public tsl::Overlay {
public:
    void initServices() override {
        mkdir("sdmc:/config", 0777);
        mkdir(kConfigDir, 0777);   // ensure dir exists before load or save
        load_config();

        // backgroundEventPoller (libtesla) crashes when its 8 KB stack overflows
        // inside the notification-scanning path (opendir + readdir + std::string
        // JSON parse every 300 ms, triggered only when useNotifications=true).
        // Doom never sends or displays Ultrahand notifications, so disable both
        // the scan and the minus-key toggle that would re-enable it at runtime.
        // Must be set here, after parseOverlaySettings() restores them to true,
        // and before the backgroundEventThread is spawned.
        ult::useNotifications       = false;
        ult::useNotificationsHotkey = false;

        // Audio backend MUST init here (initServices), not later. Audout's
        // state shifts by the time the user has navigated GUIs — late-bind
        // hits a half-torn-down session. Same constraint as sx-doom-overlay.
        const audio_backend_status_t st =
            audio_backend_init(NULL, &g_audio_backend);
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "audio_backend_init -> %d (be=%p)",
                      static_cast<int>(st),
                      static_cast<void*>(g_audio_backend));
        doom_trace(buf);
        switch_audio_set_backend(g_audio_backend);
    }

    void exitServices() override {
        if (g_audio_backend) {
            // Stop publishing first so any late engine tic finds NULL and
            // skips submit instead of pushing into a freed backend.
            switch_audio_set_backend(nullptr);
            audio_backend_shutdown(g_audio_backend);
            g_audio_backend = nullptr;
        }
    }

    void onShow() override {
        doomgeneric_switch_reanchor_clock();
    }

    void onHide() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<WadPickerGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<DoomOverlay>(argc, argv);
}
