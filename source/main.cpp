// sx-doom-overlay — Doom inside an Ultrahand overlay
//
// Task 1 (bootstrap): the overlay loads, displays a one-line status, and exits
// cleanly on B. No engine, no audio, no settings — those land in Tasks 6–12.
// The point of this placeholder is to validate the toolchain end to end.
//
// Licensed under GPLv2.

#define NDEBUG
#define STBTT_STATIC

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
