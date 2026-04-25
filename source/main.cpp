// sx-doom-overlay — Doom inside an Ultrahand overlay
//
// Task 1 (bootstrap): the overlay loads, displays a one-line status, and exits
// cleanly on B. No engine, no audio, no settings — those land in Tasks 6–12.
// The point of this placeholder is to validate the toolchain end to end.
//
// Licensed under GPLv2.

#define NDEBUG
// libtesla single-header library convention — exactly ONE translation unit
// in the project must define TESLA_INIT_IMPL before including tesla.hpp.
// That one TU emits the implementation symbols (tsl::cfg::FramebufferWidth,
// launchCombo, the libnx __appInit/__appExit overrides, the stb_truetype
// implementation, etc.). main.cpp is the canonical place for it.
//
// We do NOT define STBTT_STATIC here even though Tetris-Overlay does —
// libtesla's tesla.cpp source unit calls stbtt_FindGlyphIndex directly,
// so the stb_truetype symbols MUST be externally visible (extern linkage).
// STBTT_STATIC makes them static-linkage = TU-local, which breaks the link.
#define TESLA_INIT_IMPL

#include <tesla.hpp>

namespace {

constexpr const char* kAppTitle = "sx-doom-overlay";
constexpr const char* kBootstrapVersion = "0.0.1-bootstrap";
constexpr const char* kStatusLine = "Bootstrap build — engine integration pending (Task 7)";

}  // namespace

class BootstrapGui final : public tsl::Gui {
   public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(kAppTitle, kBootstrapVersion);
        auto* label = new tsl::elm::CustomDrawer(
            [](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
                renderer->drawString(kStatusLine, false, x + 16, y + 32, 18,
                                     tsl::Color(0xFFFF));
            });
        frame->setContent(label);
        return frame;
    }
};

class DoomOverlay final : public tsl::Overlay {
   public:
    void initServices() override {}
    void exitServices() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<BootstrapGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<DoomOverlay>(argc, argv);
}
