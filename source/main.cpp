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
#include <string>
#include <vector>

#include "blit.hpp"
#include "input_map.hpp"

extern "C" {
#include "audio_backend.h"
#include "audio_glue.h"
#include "doomgeneric.h"
#include "i_video.h"  // extern colors[256], extern palette_changed

extern jmp_buf g_doom_error_jmp;             // defined in doomgeneric_nx.c
extern void doomgeneric_switch_reanchor_clock(void);

void doomgeneric_Create(int argc, char** argv);
void doomgeneric_Tick(void);

extern int gamestate;
extern int gametic;
extern int demosequence;
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

// Bottom-of-overlay UI: heap counter + Save/Load/Quit buttons.
// Game viewport ends at y = 108 + 336 = 444. Footer is hidden during gameplay
// (DoomGui sets m_footerHidden=true). Free space below: y = 444..720 (276 px).
//
//   y = 470: heap counter text (single line, small font)
//   y = 540: button row, 60 px tall, three buttons evenly spaced
//
// Buttons are touchable rectangles drawn via renderer->drawRoundedRect with
// libtesla-style click highlight. Save → push KEY_F2 (opens Doom save menu),
// Load → push KEY_F3 (load menu), Quit → tsl::Overlay::close().
constexpr int kBtnY      = 540;
constexpr int kBtnH      = 60;
constexpr int kBtnW      = 120;
constexpr int kBtnSaveX  = 30;
constexpr int kBtnLoadX  = 164;
constexpr int kBtnQuitX  = 298;

// Doom F-keys (chocolate-doom doomkeys.h).
constexpr unsigned char kDoomKeyF2 = 0x80 + 0x3c;  // save game
constexpr unsigned char kDoomKeyF3 = 0x80 + 0x3d;  // load game

// WAD directories. We accept BOTH locations during the integration window so
// neither legacy installs nor RetroArch-style new installs break.
//   - kWadDir          (primary): /roms/doom/   — RetroArch / EmuDeck convention
//   - kWadDirLegacy:               /switch/sx-doom-overlay/ — original sx-doom-
//                                  overlay layout, still used by users who
//                                  installed before this branch landed.
// scan_wads() walks both, dedupes by basename. After everyone's migrated to
// /roms/doom we can retire the legacy path.
constexpr const char* kWadDir        = "sdmc:/roms/doom";
constexpr const char* kWadDirLegacy  = "sdmc:/switch/sx-doom-overlay";
constexpr const char* kConfigDir     = "sdmc:/config/doom";
constexpr const char* kTraceLog      = "sdmc:/config/doom/trace.log";
constexpr const char* kConfigFile    = "sdmc:/config/doom/config.ini";
constexpr const char* kConfigSection = "doom";

bool g_lcd_grid = false;

// stdio_stubs.c overrides fprintf/fputs to no-ops (suppresses 220+ engine
// prints that crash on NULL stdout). libultrahand's setIniFile uses both, so
// any save via ult::setIniFileValue writes an empty file silently.
// Use fwrite/fread directly — neither is overridden by stdio_stubs.c.
static void save_lcd_grid() {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);
    FILE* f = std::fopen(kConfigFile, "w");
    if (!f) return;
    const char* line = g_lcd_grid ? "[doom]\nlcd_grid=1\n" : "[doom]\nlcd_grid=0\n";
    std::fwrite(line, 1, std::strlen(line), f);
    std::fclose(f);
}
static void load_config() {
    FILE* f = std::fopen(kConfigFile, "r");
    if (!f) return;
    char buf[64];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    if      (std::strstr(buf, "lcd_grid=1")) g_lcd_grid = true;
    else if (std::strstr(buf, "lcd_grid=0")) g_lcd_grid = false;
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
    scan_wad_dir(kWadDir,       result);  // primary: /roms/doom
    scan_wad_dir(kWadDirLegacy, result);  // fallback: /switch/sx-doom-overlay
    return result;
}

extern "C" void doom_trace(const char* msg) {
    FILE* f = std::fopen(kTraceLog, "a");
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

    doom_blit::build_palette_lut_from_argb_struct(
        reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
    palette_changed = false;
}

}  // namespace

class ConfigGui final : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new DoomOverlayFrame("Back", "");
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Display"));

        auto* grid_item = new tsl::elm::ToggleListItem("LCD Grid", g_lcd_grid, ult::ON, ult::OFF);
        grid_item->setStateChangedListener([](bool state) {
            g_lcd_grid = state;
            save_lcd_grid();
        });
        list->addItem(grid_item);

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
};

class DoomElement final : public tsl::elm::Element {
public:
    void draw(tsl::gfx::Renderer* renderer) override {
        if (g_doom_failed) {
            renderer->drawString("Engine init failed:", false, 20, 200, 22, tsl::Color(0xFFFF));
            renderer->drawString(g_doom_error_msg,      false, 20, 240, 18, tsl::Color(0xFA0F));
            return;
        }
        if (!g_doom_initialized) {
            renderer->drawString("Loading...", false, 20, 200, 22, tsl::Color(0xFFFF));
            return;
        }

        if (palette_changed) {
            doom_blit::build_palette_lut_from_argb_struct(
                reinterpret_cast<const uint8_t*>(&colors[0]), g_palette_lut);
            palette_changed = false;
        }

        // Scale 320x200 → 448x336 (fill-width, correct 4:3 Doom aspect).
        // Nearest-neighbor: src_x = dx*320/448, src_y = dy*200/336.
        // Integer division gives correct 0..319 / 0..199 range at all endpoints.
        // Background already filled by DoomOverlayFrame::draw().
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(DG_ScreenBuffer);
        if (src) {
            const bool doGrid = g_lcd_grid;
            for (int dy = 0; dy < kScaledH; ++dy) {
                const int sy = dy * kDoomH / kScaledH;
                const std::uint8_t* srow = src + sy * kDoomW;
                const int fy = kDoomOffsetY + dy;
                const bool dimRow = doGrid && (dy % 2 == 1);
                for (int dx = 0; dx < kScaledW; ++dx) {
                    const int sx = dx * kDoomW / kScaledW;
                    tsl::Color col(g_palette_lut[srow[sx]]);
                    if (dimRow || (doGrid && (dx % 2 == 1))) {
                        col = tsl::Color({u8(col.r >> 1), u8(col.g >> 1), u8(col.b >> 1), col.a});
                    }
                    renderer->setPixel(dx, fy, col);
                }
            }
        }

        // ── Heap counter (sampled once per second) ───────────────────────
        {
            static u64 heap_last_sample_ns = 0;
            static char heap_label[80] = "heap: …";
            const u64 now_ns = armTicksToNs(armGetSystemTick());
            if (now_ns - heap_last_sample_ns > 1'000'000'000ULL) {
                heap_last_sample_ns = now_ns;
                struct mallinfo mi = mallinfo();
                const size_t free_kb = static_cast<size_t>(mi.fordblks) / 1024;
                const size_t used_kb = static_cast<size_t>(mi.uordblks) / 1024;
                std::snprintf(heap_label, sizeof(heap_label),
                              "heap: %zu KB used  /  %zu KB free",
                              used_kb, free_kb);
            }
            renderer->drawString(heap_label, false, 30, 470, 14,
                                 tsl::Color(0xCFFF));
        }

        // ── Action buttons: Save (F2) / Load (F3) / Quit (close overlay) ─
        auto draw_btn = [&](int x, const char* label, bool pressed) {
            const tsl::Color bg   = pressed
                ? a(tsl::clickColor)
                : tsl::Color(0x4448);
            renderer->drawRoundedRect(static_cast<float>(x),
                                      static_cast<float>(kBtnY),
                                      static_cast<float>(kBtnW),
                                      static_cast<float>(kBtnH),
                                      8.f, bg);
            const auto td = renderer->getTextDimensions(label, false, 20);
            const int tx = x + (kBtnW - static_cast<int>(td.first)) / 2;
            const int ty = kBtnY + (kBtnH + 14) / 2;
            renderer->drawString(label, false, tx, ty, 20,
                                 tsl::Color(0xFFFF));
        };
        draw_btn(kBtnSaveX, "Save", m_btnPressed == 1);
        draw_btn(kBtnLoadX, "Load", m_btnPressed == 2);
        draw_btn(kBtnQuitX, "Quit", m_btnPressed == 3);
    }

    void layout(u16, u16, u16, u16) override {}

    bool handleInput(u64, u64, const HidTouchState&,
                     HidAnalogStickState, HidAnalogStickState) override {
        return false;
    }

    bool onTouch(tsl::elm::TouchEvent event,
                 s32 currX, s32 currY,
                 s32 prevX, s32 prevY,
                 s32 initialX, s32 initialY) override {
        (void)prevX; (void)prevY; (void)initialX; (void)initialY;
        auto inBtn = [&](int x) {
            return currX >= x && currX < x + kBtnW &&
                   currY >= kBtnY && currY < kBtnY + kBtnH;
        };
        // Press: latch which button is being held for visual highlight.
        if (event == tsl::elm::TouchEvent::Touch) {
            if      (inBtn(kBtnSaveX)) { m_btnPressed = 1; return true; }
            else if (inBtn(kBtnLoadX)) { m_btnPressed = 2; return true; }
            else if (inBtn(kBtnQuitX)) { m_btnPressed = 3; return true; }
        }
        // Release: only fire the action if release happened in the same
        // button that was originally pressed (drag-out-then-release cancels).
        if (event == tsl::elm::TouchEvent::Release) {
            const int hit = m_btnPressed;
            m_btnPressed = 0;
            if (hit == 1 && inBtn(kBtnSaveX)) {
                doomgeneric_switch_push_key(1, kDoomKeyF2);
                doomgeneric_switch_push_key(0, kDoomKeyF2);
                return true;
            }
            if (hit == 2 && inBtn(kBtnLoadX)) {
                doomgeneric_switch_push_key(1, kDoomKeyF3);
                doomgeneric_switch_push_key(0, kDoomKeyF3);
                return true;
            }
            if (hit == 3 && inBtn(kBtnQuitX)) {
                // CRITICAL: set launchComboHasTriggered before close. Without
                // this flag, libtesla's close path runs the "exit feedback
                // sound" branch (tesla.hpp:13867) which conflicts with our
                // audio backend teardown and crashes Atmosphère. Combo close
                // sets this; the original Quit button on sx-doom-overlay
                // didn't, which is why combo worked but Quit was crashy.
                doom_trace("Quit button — closing overlay");
                launchComboHasTriggered.store(true, std::memory_order_release);
                tsl::Overlay::get()->close();
                return true;
            }
        }
        return false;
    }

private:
    int m_btnPressed = 0;  // 0=none, 1=Save, 2=Load, 3=Quit
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
            try_init_engine(m_wadPath.c_str());
            if (g_doom_initialized && m_frame)
                m_frame->setFooterHidden(true);
            return;
        }
        if (g_doom_failed) return;

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
        const u64 keysUp = m_prevKeysHeld & ~keysHeld;
        m_prevKeysHeld = keysHeld;

        // Close overlay with the user's configured Ultrahand launch combo.
        // Same combo that opened the overlay — intuitive and user-configurable.
        const bool launchCombo =
            (keysDown & tsl::cfg::launchCombo) &&
            (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo);
        if (launchCombo) {
            doom_trace("launch combo — closing overlay");
            launchComboHasTriggered.store(true, std::memory_order_release);
            tsl::Overlay::get()->close();
            return true;
        }

        // Minus alone → settings.
        const bool simulatedNext = ult::simulatedNextPage.exchange(false, std::memory_order_acq_rel);
        if (simulatedNext || ((keysDown & HidNpadButton_Minus) && !(keysHeld & HidNpadButton_Plus))) {
            tsl::changeTo<ConfigGui>();
            return true;
        }

        if (g_doom_initialized && !g_doom_failed) {
            doom_input::dispatch(keysDown, keysUp);
        }

        return true;
    }

private:
    std::string       m_wadPath;
    DoomElement*      m_doomElement = nullptr;
    DoomOverlayFrame* m_frame       = nullptr;
    u64               m_prevKeysHeld = 0;
};

class WadPickerGui final : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new DoomOverlayFrame("", "Configure");
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
