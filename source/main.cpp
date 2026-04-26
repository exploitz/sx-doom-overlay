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
#include <setjmp.h>
#include <sys/stat.h>

#include "blit.hpp"

extern "C" {
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
}

namespace {

constexpr const char* kAppTitle      = "Doom Overlay";
constexpr const char* kAppVersion    = "0.0.1-task7";

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
constexpr int         kRenderScale   = 1;       // forced to 1× for default FB
constexpr int         kDoomOffsetX   = (kFbWidth  - kDoomW * kRenderScale) / 2;  // 64
constexpr int         kDoomOffsetY   = (kFbHeight - kDoomH * kRenderScale) / 2;  // 260
constexpr const char* kIWadPath      = "sdmc:/switch/.overlays/doom/freedoom1.wad";
constexpr const char* kZoneSizeMb    = "4";

// Engine state — protected by the libtesla single-thread invariant.
bool        g_doom_initialized = false;
bool        g_doom_failed      = false;
char        g_doom_error_msg[128] = {0};
doom_blit::PaletteLut g_palette_lut;

// Trace logger — writes to sdmc:/config/sx-doom-overlay/trace.log so
// if the engine crashes we can read the file off the SD and see exactly
// how far it got. Lightweight: opens append, writes one line, closes.
void doom_trace(const char* msg) {
    FILE* f = std::fopen("sdmc:/config/sx-doom-overlay/trace.log", "a");
    if (f) {
        std::fprintf(f, "[%u] %s\n", static_cast<unsigned>(armTicksToNs(armGetSystemTick()) / 1000000ULL), msg);
        std::fclose(f);
    }
}

void try_init_engine() {
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

    // argv for doomgeneric_Create. Defensive flags for hardware test:
    //   -iwad <path>   — IWAD to load
    //   -mb 4          — zone allocator size in MiB (matches our heap budget)
    //   -nosound       — skip SFX init (we have no DG_sound_module yet, Task 9)
    //   -nomusic       — skip music init (no DG_music_module either)
    //   -nodraw        — DO NOT use; doomgeneric needs DG_DrawFrame called
    static char arg_doom[]    = "doom";
    static char arg_iwad[]    = "-iwad";
    static char arg_iwadpath[256];
    static char arg_mb[]      = "-mb";
    static char arg_mbsize[]  = "4";
    static char arg_nosound[] = "-nosound";
    static char arg_nomusic[] = "-nomusic";
    std::strncpy(arg_iwadpath, kIWadPath, sizeof(arg_iwadpath) - 1);
    char* engine_argv[] = { arg_doom, arg_iwad, arg_iwadpath, arg_mb, arg_mbsize,
                            arg_nosound, arg_nomusic, nullptr };
    int   engine_argc   = 7;

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

class DoomElement final : public tsl::elm::Element {
   public:
    void draw(tsl::gfx::Renderer* renderer) override {
        // If engine init failed, render the error message instead of trying
        // to blit garbage from an uninitialized DG_ScreenBuffer.
        if (g_doom_failed) {
            renderer->fillScreen(tsl::Color(0x000F));  // black
            renderer->drawString("Doom engine init failed:", false, 16, 32, 22, tsl::Color(0xFFFF));
            renderer->drawString(g_doom_error_msg,           false, 16, 64, 18, tsl::Color(0xFA0F));
            return;
        }
        if (!g_doom_initialized) {
            renderer->fillScreen(tsl::Color(0x000F));
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
        // Per-pixel call overhead is fine for Tegra X1: at scale=2 we do
        // 320×200×4 = 256k pixels per frame at 35 Hz = 9 Mpx/s,
        // well within budget. Optimization to NEON + precomputed swizzle
        // LUTs (UltraGB pattern) is a follow-up task.
        // Background: clear the full framebuffer to black so old text
        // from the bootstrap doesn't show around the Doom viewport.
        renderer->fillScreen(tsl::Color(0xF000));

        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(DG_ScreenBuffer);
        if (src) {
            // Doom 320×200 at 1× centered in libtesla's default 448×720
            // framebuffer. Scale > 1 requires libtesla's windowed mode
            // (or a swizzle replacement) since the block-linear math is
            // hardcoded for 448-wide; a follow-up task.
            for (int sy = 0; sy < kDoomH; ++sy) {
                const std::uint8_t* row = src + sy * kDoomW;
                const int dy = kDoomOffsetY + sy;
                for (int sx = 0; sx < kDoomW; ++sx) {
                    renderer->setPixel(kDoomOffsetX + sx, dy, tsl::Color(g_palette_lut[row[sx]]));
                }
            }
        }
    }

    void layout(u16 /*parentX*/, u16 /*parentY*/, u16 /*parentWidth*/, u16 /*parentHeight*/) override {}
    bool handleInput(u64 /*keysDown*/, u64 /*keysHeld*/, const HidTouchState& /*touchPos*/,
                     HidAnalogStickState /*joyStickPosLeft*/, HidAnalogStickState /*joyStickPosRight*/) override {
        return false;
    }
};

class DoomGui final : public tsl::Gui {
   public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(kAppTitle, kAppVersion);
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
            try_init_engine();
            return;  // give the UI one composite to settle before ticking
        }
        if (g_doom_failed) return;

        // 35 Hz tick accumulator. Cap at 4 catch-up ticks per update() to
        // avoid a "spiral of death" if libtesla composite stalls — Doom
        // gracefully handles dropped frames.
        static u64 last_ns = 0;
        static u64 accum_ns = 0;
        constexpr u64 kTickPeriodNs = 1'000'000'000ULL / 35ULL;  // ~28.6 ms
        constexpr int kMaxCatchupTicks = 4;

        u64 now_ns = armTicksToNs(armGetSystemTick());
        if (last_ns == 0) last_ns = now_ns;
        accum_ns += (now_ns - last_ns);
        last_ns = now_ns;

        int ticks_run = 0;
        while (accum_ns >= kTickPeriodNs && ticks_run < kMaxCatchupTicks) {
            static unsigned tick_count = 0;
            // Log every 35 ticks (~1 sec) so we can see how far the engine got
            // before any crash. After ~5 sec we stop logging to avoid disk spam.
            if ((tick_count % 35) == 0 && tick_count < 35 * 10) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "tick %u (sec %u)", tick_count, tick_count / 35);
                doom_trace(buf);
            }
            tick_count++;
            doomgeneric_Tick();
            accum_ns -= kTickPeriodNs;
            ticks_run++;
        }
        // If we missed by more than the catch-up cap, drop the surplus.
        if (accum_ns >= kTickPeriodNs * kMaxCatchupTicks) {
            accum_ns = 0;
        }
    }

   private:
    DoomElement* m_doomElement = nullptr;
};

class DoomOverlay final : public tsl::Overlay {
   public:
    void initServices() override {
        // Stay on libtesla's default 448×720 framebuffer — the block-linear
        // swizzle (tesla.hpp:2700) is hardcoded for this geometry. Doom is
        // drawn into a centered 320×200 region inside the default buffer.
        // Larger scale support is a follow-up task that will need either
        // libtesla's windowed mode or a custom swizzle replacement.
    }

    void exitServices() override {
        // Engine has no clean shutdown path — we just abandon it.
        // libtesla will tear down the layer + framebuffer on process exit.
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
        return std::make_unique<DoomGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<DoomOverlay>(argc, argv);
}
