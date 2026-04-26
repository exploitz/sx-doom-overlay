// sx-doom-overlay — Doom inside an Ultrahand overlay
//
// Task 7 (engine integration) — minimal viable: doomgeneric runs inside
// libtesla's update()/draw() callbacks, attract demo plays, no input or
// audio yet. Tasks 8 (input mapping) and 9 (audio) build on this.
//
// Threading: single-threaded (engine + render in libtesla callbacks).
// Audio thread will be added in Task 9.
//
// Licensed under GPLv2.

#define NDEBUG
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <vector>

// Reverted: __nx_main_thread_stack_size override was actually SHRINKING the
// libnx default 1 MiB to 256 KiB, not growing it — could induce stack
// overflow in Doom's BSP recursion. Trust libnx's default.

#include "blit.hpp"
#include "input_map.hpp"

extern "C" {
#include "audio_backend.h"
#include "audio_glue.h"
#include "doomgeneric.h"
// NB: NOT including doomkeys.h here — its KEY_MINUS = 0x2d (keyboard scancode)
// collides with libultrahand's KEY_MINUS = HidNpadButton_Minus (Switch button).
// doomgeneric_switch.c is the only TU that needs the doom keycodes.
#include "i_video.h"  // extern colors[256], extern palette_changed

extern jmp_buf g_doom_error_jmp;             // defined in doomgeneric_switch.c
extern void doomgeneric_switch_reanchor_clock(void);

// Engine entry points.
void doomgeneric_Create(int argc, char** argv);
void doomgeneric_Tick(void);

// Diagnostic externs for title/demo crash investigation (Task 7a).
// gamestate enum: 0=GS_LEVEL, 1=GS_INTERMISSION, 2=GS_FINALE, 3=GS_DEMOSCREEN
extern int gamestate;        // gamestate_t backed by int
extern int gametic;
extern int demosequence;     // only meaningful when gamestate==GS_DEMOSCREEN
}

namespace {

constexpr const char* kAppTitle      = "ULTRAWADS";  // overlay brand — UltraGB-style naming + WADS
constexpr const char* kAppVersion    = "0.1";

// libtesla's default framebuffer is 448×720 with a hardcoded block-linear
// swizzle (tesla.hpp:2700: "Pure functions of the fixed 448×720 /
// offsetWidthVar=112 geometry"). Setting cfg::FramebufferWidth/Height to
// non-default values does NOT recompute the swizzle constants — pixels
// past x=447 land at wrong memory addresses and corrupt libtesla state.
// We therefore stay at the default and draw Doom CENTERED in that region.
constexpr int         kFbWidth       = 448;
constexpr int         kFbHeight      = 720;
constexpr int         kDoomW         = 320;
constexpr int         kDoomH         = 200;

// Display viewport: 1.4× nearest-neighbor stretch from 320×200 → 448×280.
// Full framebuffer width (the swizzle is hardcoded for 448 wide so we can use
// every column safely). 1.4× chosen so the picture is meaningfully bigger than
// 1× without exceeding what the per-pixel blit can do at 60 Hz on Tegra X1
// (448×280 = 125k pixels/frame ≈ 7.5 M setPixel ops/sec — within budget).
constexpr int         kDisplayW      = 448;
constexpr int         kDisplayH      = 280;
constexpr int         kDisplayX      = 0;
constexpr int         kDisplayY      = (kFbHeight - kDisplayH) / 2;  // 220
// WAD location moved from /switch/.overlays/doom/ (wrong — .overlays is for
// .ovl binaries only, per Switch homebrew convention) to /switch/sx-doom-overlay/
// (per-app data dir). User drops *.wad files here; picker enumerates them.
constexpr const char* kWadDir        = "sdmc:/switch/sx-doom-overlay";
constexpr const char* kZoneSizeMb    = "4";

// Engine state — protected by the libtesla single-thread invariant.
bool             g_doom_initialized = false;
bool             g_doom_failed      = false;
char             g_doom_error_msg[128] = {0};
audio_backend_t* g_audio_backend    = nullptr;
doom_blit::PaletteLut g_palette_lut;

// Friendly name of the loaded WAD — set by DoomGui ctor, drawn under the
// viewport by DoomElement. Global because Element doesn't get constructor args
// in our simple architecture.
std::string g_wad_display_name;

// System status — refreshed every ~60 frames (≈1 sec) by DoomElement::draw.
// psmInitialize() in DoomOverlay::initServices; psmExit() in exitServices.
struct SysStatus {
    char     time_str[16]    = "--:--";   // HH:MM:SS
    int      battery_pct     = -1;        // -1 = psm not available
    unsigned heap_used_kb    = 0;
    unsigned heap_total_kb   = 0;
    bool     psm_ready       = false;
} g_sys_status;

void refresh_sys_status() {
    // Time
    time_t t = std::time(nullptr);
    struct tm tm_buf;
    if (localtime_r(&t, &tm_buf) != nullptr) {
        std::strftime(g_sys_status.time_str, sizeof(g_sys_status.time_str), "%H:%M:%S", &tm_buf);
    }
    // Battery (psm)
    if (g_sys_status.psm_ready) {
        u32 pct = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) {
            g_sys_status.battery_pct = static_cast<int>(pct);
        }
    }
    // Heap (mallinfo) — uordblks = currently allocated, fordblks = free in arena
    struct mallinfo mi = mallinfo();
    g_sys_status.heap_used_kb  = static_cast<unsigned>(mi.uordblks) / 1024;
    g_sys_status.heap_total_kb = static_cast<unsigned>(mi.uordblks + mi.fordblks) / 1024;
}

// WAD discovery + friendly-name table.
struct WadEntry {
    std::string filename;      // "doom.wad"
    std::string fullpath;      // "sdmc:/switch/sx-doom-overlay/doom.wad"
    std::string display_name;  // "Doom (Ultimate)"
};

std::string friendly_wad_name(const std::string& fn) {
    // Lowercase compare so e.g. DOOM.WAD and doom.wad both match.
    auto eq = [&](const char* p) { return strcasecmp(fn.c_str(), p) == 0; };
    if (eq("doom.wad"))       return "Doom (Ultimate)";
    if (eq("doom2.wad"))      return "Doom II";
    if (eq("doom1.wad"))      return "Doom (shareware)";
    if (eq("tnt.wad"))        return "TNT: Evilution";
    if (eq("plutonia.wad"))   return "The Plutonia Experiment";
    if (eq("freedoom1.wad"))  return "Freedoom Phase 1";
    if (eq("freedoom2.wad"))  return "Freedoom Phase 2";
    if (eq("chex.wad"))       return "Chex Quest";
    if (eq("chex3.wad"))      return "Chex Quest 3";
    if (eq("hacx.wad"))       return "HacX";
    return fn;  // unknown — show the filename
}

std::vector<WadEntry> scan_wads() {
    std::vector<WadEntry> result;
    mkdir(kWadDir, 0777);  // create on first run so user has somewhere to drop WADs
    DIR* d = opendir(kWadDir);
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = std::strlen(name);
        if (len > 4 && strcasecmp(name + len - 4, ".wad") == 0) {
            WadEntry e;
            e.filename     = name;
            e.fullpath     = std::string(kWadDir) + "/" + name;
            e.display_name = friendly_wad_name(e.filename);
            result.push_back(std::move(e));
        }
    }
    closedir(d);
    return result;
}

// Trace logger — writes to sdmc:/config/sx-doom-overlay/trace.log so
// if the engine crashes we can read the file off the SD and see exactly
// how far it got. Lightweight: opens append, writes one line, closes.
//
// IMPORTANT: uses snprintf+fwrite, NOT fprintf. Our stdio_stubs.c overrides
// fprintf to a no-op (necessary because the engine has 220+ unguarded prints
// that crash on NULL stdout). Going through fprintf here would silently drop
// the line. snprintf-into-buffer + fwrite reaches the file.
extern "C" void doom_trace(const char* msg);

void doom_trace(const char* msg) {
    FILE* f = std::fopen("sdmc:/config/sx-doom-overlay/trace.log", "a");
    if (f) {
        char line[256];
        const unsigned ts_ms = static_cast<unsigned>(armTicksToNs(armGetSystemTick()) / 1000000ULL);
        const int n = std::snprintf(line, sizeof(line), "[%u] %s\n", ts_ms, msg);
        if (n > 0) {
            std::fwrite(line, 1, static_cast<size_t>(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1), f);
        }
        std::fclose(f);
    }
}

void try_init_engine(const char* iwad_path, int warp_episode, int warp_map) {
    // Ensure log directory exists.
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/sx-doom-overlay", 0777);
    doom_trace("=== try_init_engine ===");

    // setjmp checkpoint — if the engine longjmps from any I_Error /
    // I_Quit path (patches/0002), we land here with err != 0.
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

    // argv for doomgeneric_Create. WAD chosen by WadPickerGui.
    //   -iwad <path>   — IWAD to load (passed in)
    //   -mb 4          — zone allocator size in MiB. -mb 6 confirmed too tight
    //                    (Z_Init malloc fails, longjmp 6 in 6ms after framebuffer
    //                    fixes). Libtesla overlay heap can't spare 6 contiguous MiB.
    //   SFX enabled via DG_sound_module in i_sound_switch.c (SWITCH_SOUND).
    //   Music enabled via DG_music_module → music_opl_module (Nuked-OPL3
    //   FM synth, vendored from chocolate-doom in source/opl/).
    if (!iwad_path || iwad_path[0] == '\0') {
        g_doom_failed = true;
        std::snprintf(g_doom_error_msg, sizeof(g_doom_error_msg), "No IWAD selected");
        doom_trace("try_init_engine called with empty iwad_path");
        return;
    }
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "selected IWAD: %s", iwad_path);
        doom_trace(buf);
    }

    static char arg_doom[]    = "doom";
    static char arg_iwad[]    = "-iwad";
    static char arg_iwadpath[256];
    static char arg_mb[]      = "-mb";
    static char arg_mbsize[]  = "4";
    static char arg_warp[]    = "-warp";
    static char arg_warp_e[8]; // "1".."4"
    static char arg_warp_m[8]; // "1".."32"
    static char arg_skill[]   = "-skill";
    static char arg_skill_n[] = "2";  // "I'm too young to die"
    std::strncpy(arg_iwadpath, iwad_path, sizeof(arg_iwadpath) - 1);
    arg_iwadpath[sizeof(arg_iwadpath) - 1] = '\0';
    // STATIC: doomgeneric stores `myargv = argv` (pointer copy, no deep copy
    // — see lib/doomgeneric/doomgeneric/doomgeneric.c:17). If this array
    // were stack-local, the pointer would dangle the moment try_init_engine
    // returns; later ticks scanning args (G_DoPlayDemo's M_CheckParm calls,
    // etc.) would deref freed stack memory and crash. Static storage gives
    // the array program lifetime to match the engine's reference.
    static char* engine_argv[16];
    int idx = 0;
    engine_argv[idx++] = arg_doom;
    engine_argv[idx++] = arg_iwad;
    engine_argv[idx++] = arg_iwadpath;
    engine_argv[idx++] = arg_mb;
    engine_argv[idx++] = arg_mbsize;
    if (warp_episode > 0 && warp_map > 0) {
        std::snprintf(arg_warp_e, sizeof(arg_warp_e), "%d", warp_episode);
        std::snprintf(arg_warp_m, sizeof(arg_warp_m), "%d", warp_map);
        engine_argv[idx++] = arg_warp;
        engine_argv[idx++] = arg_warp_e;
        engine_argv[idx++] = arg_warp_m;
        engine_argv[idx++] = arg_skill;
        engine_argv[idx++] = arg_skill_n;
        char dbg[64];
        std::snprintf(dbg, sizeof(dbg), "warp to E%dM%d", warp_episode, warp_map);
        doom_trace(dbg);
    }
    engine_argv[idx]   = nullptr;
    int engine_argc    = idx;

    doom_trace("calling doomgeneric_Create...");
    doomgeneric_Create(engine_argc, engine_argv);
    doom_trace("doomgeneric_Create returned OK");
    g_doom_initialized = true;

    // Pre-build palette LUT from the engine's current colors[] table.
    // Doom calls I_SetPalette during D_DoomMain init which populates
    // colors[]; we read it here and refresh every frame if changed.
    doom_blit::build_palette_lut_from_argb_struct(
        reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
    palette_changed = false;
}

}  // namespace

// Touch buttons rendered in the empty margin below the Doom viewport.
// Each button is a tap target with a static label and an action callback.
//
// Layout: Doom viewport occupies y=260..460 (320×200 centered in 448×720).
// We place three vertically-stacked 40px buttons in y=475..615, centered
// horizontally at x=124 (200px wide → spans x=124..324 inside the 448 FB).
struct OverlayButton {
    int x, y, w, h;
    const char* label;
    void (*action)();
};

void action_change_wad();
void action_show_controls();
void action_quit_overlay();
void draw_wads_header(tsl::gfx::Renderer* renderer, const char* subtitle);

// Two horizontal touch buttons at the very bottom.
constexpr OverlayButton kOverlayButtons[] = {
    {  16, 678, 200, 36, "Quit",      &action_quit_overlay   },
    { 232, 678, 200, 36, "Controls",  &action_show_controls  },
};
constexpr int kOverlayButtonCount = sizeof(kOverlayButtons) / sizeof(kOverlayButtons[0]);

class DoomElement final : public tsl::elm::Element {
   public:
    void draw(tsl::gfx::Renderer* renderer) override {
        // If engine init failed, render the error message instead of trying
        // to blit garbage from an uninitialized DG_ScreenBuffer.
        if (g_doom_failed) {
            renderer->fillScreen(tsl::Color(0xF000));  // black
            renderer->drawString("Doom engine init failed:", false, 16, 32, 22, tsl::Color(0xFFFF));
            renderer->drawString(g_doom_error_msg,           false, 16, 64, 18, tsl::Color(0xFA0F));
            return;
        }
        if (!g_doom_initialized) {
            renderer->fillScreen(tsl::Color(0xF000));
            renderer->drawString("Loading Doom...", false, 16, 32, 22, tsl::Color(0xFFFF));
            return;
        }

        // Refresh palette LUT if the engine switched palette (damage flash etc.)
        if (palette_changed) {
            doom_blit::build_palette_lut_from_argb_struct(
                reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
            palette_changed = false;
        }

        // Blit the engine's 320×200 indexed buffer at integer scale into
        // libtesla's framebuffer.
        //
        // CRITICAL: libtesla's framebuffer is block-linear (Tegra GPU
        // swizzled), NOT flat row-major. See processRectChunk in
        // tesla.hpp:1390-1393 which uses blockLinearYPart() for row
        // addressing. Writing dst[y*w + x] directly is wrong and
        // scribbles memory all over libtesla's state, eventually
        // crashing Atmosphère. Route through Renderer::setPixel() which
        // calls getPixelOffset() (the correct swizzle) under the hood.
        //
        // Refresh status info once per second (every ~60 composite frames).
        // psm/time queries are cheap but no need to hammer them.
        static u32 s_frame_counter = 0;
        if ((++s_frame_counter % 60) == 0) {
            refresh_sys_status();
        }

        // Color palette — libtesla uses ABGR4444 (alpha high, red LOW nibble).
        const tsl::Color kBg          (0xF000);  // black
        const tsl::Color kFrameMid    (0xF333);  // dark gray (frame body)
        const tsl::Color kFrameBevelHi(0xF666);  // mid gray (subtle bevel hint)
        const tsl::Color kLabelDim    (0xF888);  // medium gray
        const tsl::Color kLabelDoom   (0xF03F);  // Doom red-orange
        const tsl::Color kBtnBg       (0xF222);  // subtle button bg
        const tsl::Color kBtnText     (0xFFFF);  // white

        // Full-screen black background (we own the entire framebuffer now —
        // OverlayFrame is passed empty title strings so it doesn't draw text).
        renderer->fillScreen(kBg);

        // ====== WAD-derived 4-letter gradient logo (top-left) ======
        // Title = first 4 alphanumeric chars of the loaded WAD name, uppercase.
        // So "Chex Quest 3" → CHEX, "Freedoom Phase 1" → FREE, "Doom (Ultimate)"
        // → DOOM. Strong red→yellow gradient (full color range — was barely
        // noticeable before with red→orange only). Drop-shadow for depth.
        {
            char logo[5] = {0};
            int  jj = 0;
            for (size_t ii = 0; ii < g_wad_display_name.size() && jj < 4; ++ii) {
                const char c = g_wad_display_name[ii];
                if (c >= 'A' && c <= 'Z') logo[jj++] = c;
                else if (c >= 'a' && c <= 'z') logo[jj++] = static_cast<char>(c - 32);
                else if (c >= '0' && c <= '9') logo[jj++] = c;
                // skip spaces / punctuation
            }
            if (jj == 0) { logo[0]='D'; logo[1]='G'; logo[2]='E'; logo[3]='N'; jj=4; }

            // Strong gradient: red → orange → yellow → bright yellow.
            // ABGR4444 nibble-by-nibble (A=F, B=0..F, G=0..F, R=0..F).
            const tsl::Color colors[4] = {
                tsl::Color(0xF00F),  // pure red                  R=F G=0 B=0
                tsl::Color(0xF05F),  // red-orange                R=F G=5 B=0
                tsl::Color(0xF0AF),  // orange                    R=F G=A B=0
                tsl::Color(0xF0FF),  // bright yellow             R=F G=F B=0
            };
            const tsl::Color kLogoShadow(0xF002);  // dark red shadow

            int lx = 16;
            for (int i = 0; i < jj; ++i) {
                char one[2] = { logo[i], '\0' };
                renderer->drawString(one, false, lx + 2, 40, 32, kLogoShadow);
                renderer->drawString(one, false, lx,     38, 32, colors[i]);
                lx += 30;
            }

            // Full WAD name as subtitle directly under the logo.
            if (!g_wad_display_name.empty()) {
                renderer->drawString(g_wad_display_name.c_str(), false, 16, 64, 14, kLabelDim);
            }
        }

        // ====== Status panel — vertical stack in the top-right ======
        // Three lines, ~22px apart so labels don't collide:
        //   Time       (always gray)
        //   BAT xx%    (color by level)
        //   MEM xx/yyK (color by % used)
        {
            char buf[48];
            constexpr int kStatusX = kFbWidth - 130;

            // Battery color thresholds
            const int bat = g_sys_status.battery_pct;
            tsl::Color bat_col = (bat >= 50) ? tsl::Color(0xF0F0)
                               : (bat >= 20) ? tsl::Color(0xF0FF)
                                             : tsl::Color(0xF00F);

            // Memory color thresholds
            unsigned mem_pct = g_sys_status.heap_total_kb
                                 ? (g_sys_status.heap_used_kb * 100u) / g_sys_status.heap_total_kb
                                 : 0;
            tsl::Color mem_col = (mem_pct < 70) ? tsl::Color(0xF0F0)
                               : (mem_pct < 90) ? tsl::Color(0xF0FF)
                                                : tsl::Color(0xF00F);

            renderer->drawString(g_sys_status.time_str, false, kStatusX, 30, 14, kLabelDim);

            if (bat >= 0) {
                std::snprintf(buf, sizeof(buf), "BAT %d%%", bat);
                renderer->drawString(buf, false, kStatusX, 52, 14, bat_col);
            }

            std::snprintf(buf, sizeof(buf), "MEM %u/%uK",
                          g_sys_status.heap_used_kb, g_sys_status.heap_total_kb);
            renderer->drawString(buf, false, kStatusX, 74, 14, mem_col);
        }

        // ====== Doom viewport (1.4× nearest-neighbor stretch, full FB width) ======
        // 4-px gray bar above + below as soft framing (no glow, no animation).
        renderer->drawRect(0, kDisplayY - 4, kFbWidth, 4, kFrameMid);
        renderer->drawRect(0, kDisplayY + kDisplayH, kFbWidth, 4, kFrameMid);

        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(DG_ScreenBuffer);
        if (src) {
            for (int dy = 0; dy < kDisplayH; ++dy) {
                const int sy = (dy * kDoomH) / kDisplayH;          // nearest-neighbor
                const std::uint8_t* row = src + sy * kDoomW;
                const int target_y = kDisplayY + dy;
                for (int dx = 0; dx < kDisplayW; ++dx) {
                    const int sx = (dx * kDoomW) / kDisplayW;
                    renderer->setPixel(kDisplayX + dx, target_y, tsl::Color(g_palette_lut[row[sx]]));
                }
            }
        }

        // ====== Touch buttons (Ultrahand list-row style — text + separator) ======
        // No boxy background — text on the panel bg with a thin separator line
        // below each row, like libtesla's ListItem default rendering.
        const tsl::Color kListSep(0xF333);
        for (int i = 0; i < kOverlayButtonCount; ++i) {
            const auto& b = kOverlayButtons[i];
            renderer->drawString(b.label, false, b.x + 16, b.y + 26, 18, kBtnText);
            renderer->drawRect(b.x, b.y + b.h - 1, b.w, 1, kListSep);
        }
    }

    void layout(u16 /*parentX*/, u16 /*parentY*/, u16 /*parentWidth*/, u16 /*parentHeight*/) override {}
    bool handleInput(u64 /*keysDown*/, u64 /*keysHeld*/, const HidTouchState& /*touchPos*/,
                     HidAnalogStickState /*joyStickPosLeft*/, HidAnalogStickState /*joyStickPosRight*/) override {
        return false;
    }
};

// Forward declarations for cross-referencing GUI transitions.
// tsl::changeTo<T> needs T complete at the call site, so the bodies that
// reference other GUIs are defined out-of-line below.
class WadPickerGui;
class ControlsHelpGui;
class SettingsGui;
class LevelPickerGui;

class DoomGui final : public tsl::Gui {
   public:
    DoomGui(std::string wad_path, std::string wad_display_name,
            int warp_episode = 0, int warp_map = 0)
        : m_wadPath(std::move(wad_path)),
          m_wadDisplayName(std::move(wad_display_name)),
          m_warpEpisode(warp_episode),
          m_warpMap(warp_map) {
        g_wad_display_name = m_wadDisplayName;
    }

    tsl::elm::Element* createUI() override {
        // Pass empty strings so OverlayFrame doesn't draw its own title text —
        // we render a custom gradient "DOOM" logo + status panel in the header
        // area inside DoomElement::draw, matching Ultrahand's logo style.
        auto* frame = new tsl::elm::OverlayFrame("", "");
        m_doomElement = new DoomElement();
        frame->setContent(m_doomElement);
        return frame;
    }

    void update() override {
        // Lazy-init: doomgeneric_Create is heavy (allocates zone, loads WAD).
        // Do it on first update() call instead of in initServices() so the
        // libtesla framebuffer is already set up — engine init may print
        // banner messages we want visible if anything fails.
        if (!g_doom_initialized && !g_doom_failed) {
            try_init_engine(m_wadPath.c_str(), m_warpEpisode, m_warpMap);
            return;  // give the UI one composite to settle before ticking
        }
        if (g_doom_failed) return;

        // 35 Hz tick accumulator. Cap at 4 catch-up ticks per update() to
        // avoid a "spiral of death" if libtesla composite stalls — Doom
        // gracefully handles dropped frames.
        static u64 last_ns = 0;
        static u64 accum_ns = 0;
        constexpr u64 kTickPeriodNs    = 1'000'000'000ULL / 35ULL;  // ~28.6 ms
        constexpr int kMaxCatchupTicks = 4;
        constexpr u64 kCatchupCapNs    = kTickPeriodNs * kMaxCatchupTicks;

        u64 now_ns = armTicksToNs(armGetSystemTick());
        if (last_ns == 0) last_ns = now_ns;
        const u64 delta_ns = now_ns - last_ns;
        last_ns = now_ns;

        // Resume-from-dismiss handling: if the gap since our last tick is
        // larger than the catch-up cap, the overlay was hidden (touched-out)
        // and resumed. Don't try to walk the engine forward through the
        // dismissal interval — that would burn 4 ticks per update() until
        // the debt drained, freezing gameplay for many seconds. Instead
        // discard the gap and resume at real-time. The engine's own clock
        // was already re-anchored in DoomOverlay::onShow().
        if (delta_ns > kCatchupCapNs) {
            doom_trace("resume: discarded huge tick delta");
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
        // If we missed by more than the catch-up cap, drop the surplus.
        if (accum_ns >= kTickPeriodNs * kMaxCatchupTicks) {
            accum_ns = 0;
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                     HidAnalogStickState leftStick, HidAnalogStickState rightStick) override;

   private:
    bool         m_prevTouched = false;  // edge-detect taps
    std::string  m_wadPath;
    std::string  m_wadDisplayName;
    int          m_warpEpisode = 0;      // 0 = no warp; else 1..4
    int          m_warpMap     = 0;      // 0 = no warp; else 1..32
    DoomElement* m_doomElement = nullptr;
    u64          m_prevKeysHeld = 0;
};

// ControlsHelpGui — static cheat sheet listing all button bindings. Pushed onto
// the stack from the "Show Controls" touch button; B returns to DoomGui.
class ControlsHelpGui final : public tsl::Gui {
   public:
    tsl::elm::Element* createUI() override;
};

// SettingsGui — overlay-side settings page. Currently a status read-out (no
// runtime-tweakable knobs yet). Pushed from the WAD picker's "Settings" item.
// Future home for: audio toggle, turn-sensitivity slider, render-scale picker
// (when bigger viewport ships), volume sliders (when audio works).
class SettingsGui final : public tsl::Gui {
   public:
    tsl::elm::Element* createUI() override;
};

// LevelPickerGui — intermediate picker between WAD selection and DoomGui.
// Lets the user warp directly to a specific level instead of starting at the
// title screen. Useful for skipping past crashing levels (Doom E1M3 reportedly
// crashes for some users), and for picking up testing where a previous run
// died. "Default" preserves the title-screen attract-demo experience.
class LevelPickerGui final : public tsl::Gui {
   public:
    LevelPickerGui(std::string wad_path, std::string wad_display_name)
        : m_wadPath(std::move(wad_path)),
          m_wadDisplayName(std::move(wad_display_name)) {}
    tsl::elm::Element* createUI() override;
   private:
    std::string m_wadPath;
    std::string m_wadDisplayName;
};

// WadPickerGui — first screen the user sees. Lists all *.wad files in
// /switch/sx-doom-overlay/, user presses A to pick one, transitions to DoomGui
// with the chosen path. If no WADs found, shows where to drop them.
//
// Uses HeaderOverlayFrame so we can render a custom-drawn colored "WADS"
// gradient logo as the header (matching the in-game per-WAD logo style).
class WadPickerGui final : public tsl::Gui {
   public:
    tsl::elm::Element* createUI() override {
        auto* frame  = new tsl::elm::HeaderOverlayFrame();
        auto* header = new tsl::elm::CustomDrawer(
            [](tsl::gfx::Renderer* r, s32, s32, s32, s32) {
                draw_wads_header(r, "Select a WAD");
            });
        frame->setHeader(header);
        auto* list  = new tsl::elm::List();

        auto wads = scan_wads();
        if (wads.empty()) {
            list->addItem(new tsl::elm::ListItem("(no WADs found)"));
            list->addItem(new tsl::elm::ListItem("Put *.wad in:"));
            list->addItem(new tsl::elm::ListItem("/switch/sx-doom-overlay/"));
        } else {
            for (const auto& wad : wads) {
                auto* item = new tsl::elm::ListItem(wad.display_name, wad.filename);
                std::string fullpath = wad.fullpath;
                std::string display  = wad.display_name;
                // Route through LevelPickerGui so the user can choose to
                // boot at the title screen OR warp directly to a level.
                item->setClickListener([fullpath, display](u64 keys) -> bool {
                    if (keys & HidNpadButton_A) {
                        tsl::changeTo<LevelPickerGui>(fullpath, display);
                        return true;
                    }
                    return false;
                });
                list->addItem(item);
            }
        }

        // Settings + Quit at the bottom of the picker. Settings opens the
        // SettingsGui (read-only status for now). Quit closes the overlay.
        auto* settings_item = new tsl::elm::ListItem("Settings");
        settings_item->setClickListener([](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<SettingsGui>();
                return true;
            }
            return false;
        });
        list->addItem(settings_item);

        auto* quit_item = new tsl::elm::ListItem("Quit Overlay");
        quit_item->setClickListener([](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::Overlay::get()->close();
                return true;
            }
            return false;
        });
        list->addItem(quit_item);

        frame->setContent(list);
        return frame;
    }
};

// Helper: render the ULTRAWADS gradient logo + a subtitle. Used by the WAD
// picker and Controls help screens to match the in-game header style.
// Per-letter color interpolated red → yellow across 9 letters.
//
// Geometry matches libtesla's standard OverlayFrame title (tesla.hpp:4956):
// fontSize=32, x=20, y=50 — so users with libtesla muscle memory know where
// to look. Subtitle below at y=72 follows libtesla's subtitleY = y + 25 form.
void draw_wads_header(tsl::gfx::Renderer* renderer, const char* subtitle) {
    const char* const word = "ULTRAWADS";       // 9 letters
    constexpr int     kCount    = 9;
    constexpr int     kFontSize = 32;            // matches libtesla native title
    constexpr int     kStride   = 26;            // a touch tight for that font, gives the title weight
    const tsl::Color  shadow(0xF002);

    int lx = 20;
    for (int i = 0; i < kCount; ++i) {
        // Gradient: G channel rises from 0 (red) to F (yellow) across the word.
        // ABGR4444: A=F, B=0, G=0..F, R=F.
        const u8 g4 = static_cast<u8>((i * 15) / (kCount - 1));
        const tsl::Color c(static_cast<u16>(0xF000 | (g4 << 4) | 0x000F));
        char one[2] = { word[i], '\0' };
        renderer->drawString(one, false, lx + 1, 51, kFontSize, shadow);
        renderer->drawString(one, false, lx,     50, kFontSize, c);
        lx += kStride;
    }
    if (subtitle && *subtitle) {
        renderer->drawString(subtitle, false, 20, 75, 15, tsl::Color(0xF888));
    }
}

// ===== Out-of-line method bodies (cross-class refs need complete types) =====

void action_change_wad() {
    // Engine can't switch WADs in-process (doomgeneric_Create allocates the
    // zone once, loads lumps, registers atexit handlers — calling it again
    // would leak/fail). To pick a new WAD the user closes + reopens, getting
    // a fresh process that re-runs the WAD picker. So this action is
    // functionally equivalent to action_quit_overlay; kept around so picker
    // ListItems can call it semantically.
    doom_trace("change WAD requested — closing overlay; reopen to pick new WAD");
    tsl::Overlay::get()->close();
}

void action_show_controls() {
    doom_trace("touch button: Show Controls");
    tsl::changeTo<ControlsHelpGui>();
}

void action_quit_overlay() {
    doom_trace("touch button: Quit Overlay");
    tsl::Overlay::get()->close();
}

bool DoomGui::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                          HidAnalogStickState /*leftStick*/, HidAnalogStickState /*rightStick*/) {
    // libtesla's Gui::handleInput doesn't receive keysUp directly — derive
    // from the prev-keysHeld delta (same pattern libtesla uses internally
    // at tesla.hpp:8482-8483).
    const u64 keysUp = m_prevKeysHeld & ~keysHeld;
    m_prevKeysHeld = keysHeld;

    // Quit combo: hold Plus, tap Minus. Two-button intentional combo, hard
    // to fire by accident. Engine state is abandoned (no save) — reopening
    // shows the WAD picker fresh.
    if ((keysHeld & HidNpadButton_Plus) && (keysDown & HidNpadButton_Minus)) {
        doom_trace("quit combo (Plus+Minus) — closing overlay");
        tsl::Overlay::get()->close();
        return true;
    }

    // Touch buttons — edge-detect on tap (transition no-touch → touch).
    // libtesla zeroes touchPos when no finger is down, so any nonzero
    // coordinate counts as "touched."
    const bool now_touched = (touchPos.x != 0) || (touchPos.y != 0);
    if (now_touched && !m_prevTouched) {
        const int tx = static_cast<int>(touchPos.x);
        const int ty = static_cast<int>(touchPos.y);
        for (int i = 0; i < kOverlayButtonCount; ++i) {
            const auto& b = kOverlayButtons[i];
            if (tx >= b.x && tx < b.x + b.w &&
                ty >= b.y && ty < b.y + b.h) {
                if (b.action) b.action();
                m_prevTouched = now_touched;
                return true;
            }
        }
    }
    m_prevTouched = now_touched;

    // Only forward inputs to the engine if Doom actually loaded; the
    // loading and error screens shouldn't accept gameplay input.
    if (g_doom_initialized && !g_doom_failed) {
        doom_input::dispatch(keysDown, keysUp);
    }

    // Return true so libtesla considers the input "handled" and doesn't
    // route it back to its menu logic (which would close the overlay
    // unexpectedly on B/+/etc.).
    return true;
}

tsl::elm::Element* LevelPickerGui::createUI() {
    auto* frame  = new tsl::elm::HeaderOverlayFrame();
    auto* header = new tsl::elm::CustomDrawer(
        [](tsl::gfx::Renderer* r, s32, s32, s32, s32) {
            draw_wads_header(r, "Pick Level");
        });
    frame->setHeader(header);
    auto* list   = new tsl::elm::List();

    const std::string wad_path = m_wadPath;
    const std::string wad_disp = m_wadDisplayName;

    // Default: no warp, title screen + attract demo
    {
        auto* item = new tsl::elm::ListItem("Title Screen", "boot normal");
        item->setClickListener([wad_path, wad_disp](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<DoomGui>(wad_path, wad_disp, 0, 0);
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

    // Doom 1 / Chex / Freedoom 1 / Heretic style: 4 episodes × 9 maps.
    // We list ALL 36 — engines silently fall back to E1M1 if a level isn't
    // present in the loaded WAD, so it's safe to over-list.
    for (int ep = 1; ep <= 4; ++ep) {
        for (int mp = 1; mp <= 9; ++mp) {
            char label[16];
            std::snprintf(label, sizeof(label), "E%dM%d", ep, mp);
            auto* item = new tsl::elm::ListItem(label);
            const int e = ep, m = mp;
            item->setClickListener([wad_path, wad_disp, e, m](u64 keys) -> bool {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<DoomGui>(wad_path, wad_disp, e, m);
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }
    }

    // Doom 2 style: MAP01..MAP32 (we map MAP01..MAP09 onto -warp 1, episode
    // ignored; chocolate-doom interprets -warp <map> as MAP## for Doom 2).
    // For consistency we still pass episode=1 and use map number directly.
    for (int mp = 1; mp <= 32; ++mp) {
        char label[16];
        std::snprintf(label, sizeof(label), "MAP%02d", mp);
        auto* item = new tsl::elm::ListItem(label, "Doom 2 style");
        const int m = mp;
        item->setClickListener([wad_path, wad_disp, m](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                // For Doom 2, -warp uses one number (the map). The engine
                // accepts both forms; episode arg ignored when WAD is Doom 2.
                tsl::changeTo<DoomGui>(wad_path, wad_disp, 1, m);
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

    frame->setContent(list);
    return frame;
}

tsl::elm::Element* SettingsGui::createUI() {
    auto* frame  = new tsl::elm::HeaderOverlayFrame();
    auto* header = new tsl::elm::CustomDrawer(
        [](tsl::gfx::Renderer* r, s32, s32, s32, s32) {
            draw_wads_header(r, "Settings");
        });
    frame->setHeader(header);
    auto* list  = new tsl::elm::List();
    // Status read-out for now (real toggles come once the underlying systems
    // they control are working — audio is blocked, render scale needs swizzle
    // replacement). Left intentionally simple.
    list->addItem(new tsl::elm::ListItem("Engine",        "doomgeneric (chocolate-doom fork)"));
    list->addItem(new tsl::elm::ListItem("Doom zone",     "4 MiB (-mb 4)"));
    list->addItem(new tsl::elm::ListItem("Render scale",  "1.4x nearest-neighbor"));
    list->addItem(new tsl::elm::ListItem("Turn rate",     "1024 BAM/tic (patches/0003)"));
    list->addItem(new tsl::elm::ListItem("Audio",         "SFX 22050 Hz / 8 ch (no music)"));
    list->addItem(new tsl::elm::ListItem("Save format",   "per-WAD slots (patches/0006)"));
    list->addItem(new tsl::elm::ListItem("Quit handling", "I_Quit longjmps cleanly (patches/0002+0005)"));
    frame->setContent(list);
    return frame;
}

tsl::elm::Element* ControlsHelpGui::createUI() {
    auto* frame  = new tsl::elm::HeaderOverlayFrame();
    auto* header = new tsl::elm::CustomDrawer(
        [](tsl::gfx::Renderer* r, s32, s32, s32, s32) {
            draw_wads_header(r, "Controls");
        });
    frame->setHeader(header);
    auto* list  = new tsl::elm::List();
    // Single-arg ListItems (text only, no right-aligned value field). The
    // two-arg form was triggering a font cache crash under audio-backend
    // heap pressure (stbtt__csctx_rccurve_to in libtesla); single-arg uses
    // a simpler render path that survives the tighter memory budget.
    list->addItem(new tsl::elm::ListItem("L-stick: move + strafe"));
    list->addItem(new tsl::elm::ListItem("R-stick: turn left/right"));
    list->addItem(new tsl::elm::ListItem("D-pad: arrows / menu nav"));
    list->addItem(new tsl::elm::ListItem("ZR: FIRE (primary)"));
    list->addItem(new tsl::elm::ListItem("ZL: RUN (hold)"));
    list->addItem(new tsl::elm::ListItem("A: USE / menu select"));
    list->addItem(new tsl::elm::ListItem("B: alt FIRE / menu back"));
    list->addItem(new tsl::elm::ListItem("X: toggle automap"));
    list->addItem(new tsl::elm::ListItem("L bumper: prev weapon"));
    list->addItem(new tsl::elm::ListItem("R bumper: next weapon"));
    list->addItem(new tsl::elm::ListItem("Plus: in-game menu"));
    list->addItem(new tsl::elm::ListItem("Plus + Minus: quit overlay"));
    frame->setContent(list);
    return frame;
}

class DoomOverlay final : public tsl::Overlay {
   public:
    void initServices() override {
        if (R_SUCCEEDED(psmInitialize())) {
            g_sys_status.psm_ready = true;
        }

        // Heap-tier gate. Ethan's targets: stock 4 MB → silent; 6 MB+ → audio.
        // envGetHeapOverrideSize is the overlay's pool ceiling (Ultrahand's
        // "Overlay Memory" slider). Below ~5 MB we skip audio init entirely:
        //   - audio_backend buffers + ring  ≈ 28 KB
        //   - OPL2 chip + queue + scratch   ≈ 20 KB
        //   - SFX cache cap                 ≤ 192 KB
        //   - active MIDI buffer            ≤  96 KB
        // Plus Doom's 4 MiB zone + libtesla overhead — pool needs ~5.5 MB
        // working room before audio is safe to enable.
        const u64 pool_bytes = envGetHeapOverrideSize();
        const u64 AUDIO_MIN_POOL_BYTES = 5ULL * 1024 * 1024;  // 5 MB
        if (pool_bytes != 0 && pool_bytes < AUDIO_MIN_POOL_BYTES) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "audio: skipped — pool %llu KB < %llu KB threshold",
                          (unsigned long long)(pool_bytes / 1024),
                          (unsigned long long)(AUDIO_MIN_POOL_BYTES / 1024));
            doom_trace(buf);
            switch_audio_set_backend(nullptr);
            return;
        }

        // Audio backend MUST init here (initServices), not later. We
        // experimentally moved this to after doomgeneric_Create; the drain
        // thread immediately crashed in audoutWaitPlayFinish with a NULL
        // service handle (Result 2168-0002). libtesla's ult::Audio attach
        // is only stable during the initServices window — by the time the
        // user has navigated through GUIs and picked a WAD, audout's state
        // has shifted and our late-bind hits a half-torn-down session.
        const audio_backend_status_t st =
            audio_backend_init(NULL, &g_audio_backend);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "audio_backend_init -> %d (be=%p) pool=%lluKB",
                      static_cast<int>(st),
                      static_cast<void*>(g_audio_backend),
                      (unsigned long long)(pool_bytes / 1024));
        doom_trace(buf);
        switch_audio_set_backend(g_audio_backend);
    }

    void exitServices() override {
        if (g_audio_backend) {
            // Stop publishing first so any late engine tic finds NULL and
            // skips submit, instead of pushing into a freed backend.
            switch_audio_set_backend(nullptr);
            audio_backend_shutdown(g_audio_backend);
            g_audio_backend = nullptr;
        }
        if (g_sys_status.psm_ready) {
            psmExit();
            g_sys_status.psm_ready = false;
        }
    }

    void onShow() override {
        // Re-anchor the engine clock so the resumed simulation doesn't
        // see a giant elapsed-time jump from the time we were dismissed.
        doomgeneric_switch_reanchor_clock();
    }

    void onHide() override {
        // Pause-on-dismiss: just stop calling update(). Engine state
        // stays in memory; resumes on onShow + next update() call.
        // Audio pause will be added in Task 9.
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<WadPickerGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<DoomOverlay>(argc, argv);
}
