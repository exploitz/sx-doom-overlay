/********************************************************************************
 * File: elm_ultradoomframe.hpp
 * Description:
 *   DoomOverlayFrame — overlay frame matching UltraGB's UltraGBOverlayFrame.
 *   Draws the animated "DOOM" wave title, version label, bottom separator,
 *   and footer buttons (B=Back, A=OK) exactly as UltraGB does.
 *   No wallpaper: Doom fills the content area so wallpaper is not needed.
 *
 *  Licensed under GPLv2
 ********************************************************************************/

#pragma once

#pragma GCC push_options
#pragma GCC optimize("Os")

#include <tesla.hpp>
#include <ultra.hpp>
#include <string>

// Version label shown on the overlay header. Fallback when the build system
// doesn't pass -DAPP_VERSION (Ethan's Makefile sets it; ours doesn't yet —
// Task 11 territory). Hardcoded "0.1.0" until then.
#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

// draw_doom_title is defined in main.cpp with __attribute__((optimize("O3"))).
// Forward-declare with matching attribute pragma to suppress -Wattributes.
#pragma GCC push_options
#pragma GCC optimize("O3")
static s32 draw_doom_title(tsl::gfx::Renderer*, s32, s32, u32);
#pragma GCC pop_options

class DoomOverlayFrame final : public tsl::elm::Element {
public:
    explicit DoomOverlayFrame(std::string pageLeftName  = "",
                                   std::string pageRightName = "")
        : tsl::elm::Element()
        , m_pageLeftName(std::move(pageLeftName))
        , m_pageRightName(std::move(pageRightName))
    {
        ult::activeHeaderHeight = 97;
        m_isItem = false;
        disableSound.store(false, std::memory_order_release);
    }

    ~DoomOverlayFrame() override { delete m_contentElement; }

    // -------------------------------------------------------------------------
    void __attribute__((optimize("O3"))) draw(tsl::gfx::Renderer* renderer) override {
        renderer->fillScreen(a(tsl::defaultBackgroundColor));

#if USING_WIDGET_DIRECTIVE
        renderer->drawWidget();
#endif

        // --- Animated "DOOM" title — Tetris-style baseline y=68 ---
        draw_doom_title(renderer, 20, 68, 42);

        // --- Version: APP_VERSION (semver, e.g. "0.1.0") at top header.
        // Build identity (git branch@hash) is shown separately near the heap
        // counter via BUILD_ID — the two are decoupled so users see a clean
        // marketing version up here.
        static const std::string versionLabel = ult::cleanVersionLabel(APP_VERSION);
        renderer->drawString(versionLabel.c_str(), false, 183, 60, 15, tsl::bannerVersionTextColor);

        if (m_footerHidden) {
            if (m_contentElement != nullptr) m_contentElement->frame(renderer);
            if (!ult::useRightAlignment)
                renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
            else
                renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));
            return;
        }

        // --- Bottom separator ---
        renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73,
                           tsl::cfg::FramebufferWidth - 30, 1,
                           a(tsl::bottomSeparatorColor));

        // -----------------------------------------------------------------------
        // Static footer dimension cache (measured once, never changes at runtime)
        // -----------------------------------------------------------------------
        static bool  s_dimsReady   = false;
        static float s_gapWidth    = 0.f;
        static float s_halfGap     = 0.f;
        static float s_backWidth   = 0.f;
        static float s_selectWidth = 0.f;

        if (!s_dimsReady) {
            s_gapWidth    = renderer->getTextDimensions(ult::GAP_1,                                  false, 23).first;
            s_backWidth   = renderer->getTextDimensions("" + ult::GAP_2 + ult::BACK, false, 23).first + s_gapWidth;
            s_selectWidth = renderer->getTextDimensions("" + ult::GAP_2 + ult::OK,   false, 23).first + s_gapWidth;
            s_halfGap     = s_gapWidth * 0.5f;
            s_dimsReady   = true;
        }

        const auto updateAtomic = [](std::atomic<float>& atom, float val) {
            if (val != atom.load(std::memory_order_acquire))
                atom.store(val, std::memory_order_release);
        };
        updateAtomic(ult::halfGap,     s_halfGap);
        updateAtomic(ult::backWidth,   s_backWidth);
        updateAtomic(ult::selectWidth, s_selectWidth);

        static const float s_buttonY    = static_cast<float>(tsl::cfg::FramebufferHeight - 73 + 1);
        static constexpr float buttonStartX = 30.f;

        // -----------------------------------------------------------------------
        // Instance footer cache — rebuilt only when setPageNames() is called
        // -----------------------------------------------------------------------
        if (m_footerDirty) {
            m_hasNextPage = !m_pageLeftName.empty() || !m_pageRightName.empty();
            if (m_hasNextPage) {
                m_cachedPageLabel = !m_pageLeftName.empty()
                    ? ("" + ult::GAP_2 + m_pageLeftName)
                    : ("" + ult::GAP_2 + m_pageRightName);
                m_cachedPageLabelWidth =
                    renderer->getTextDimensions(m_cachedPageLabel, false, 23).first + s_gapWidth;
                m_cachedFooterString =
                    "" + ult::GAP_2 + ult::BACK + ult::GAP_1 +
                    "" + ult::GAP_2 + ult::OK   + ult::GAP_1 +
                    m_cachedPageLabel + ult::GAP_1;
            } else {
                m_cachedPageLabel      = {};
                m_cachedPageLabelWidth = 0.f;
                m_cachedFooterString   =
                    "" + ult::GAP_2 + ult::BACK + ult::GAP_1 +
                    "" + ult::GAP_2 + ult::OK   + ult::GAP_1;
            }
            ult::hasNextPageButton.store(m_hasNextPage, std::memory_order_release);
            ult::nextPageWidth.store(m_cachedPageLabelWidth, std::memory_order_release);
            m_footerDirty = false;
        }

        // --- Touch highlights ---
        if (m_hasNextPage) {
            if (ult::touchingNextPage.load(std::memory_order_acquire)) {
                const float nextX = buttonStartX + 2.f - s_halfGap + s_backWidth + 1.f + s_selectWidth;
                renderer->drawRoundedRect(nextX, s_buttonY, m_cachedPageLabelWidth - 2.f, 73.0f, 12.0f,
                                          a(tsl::clickColor));
            }
        }
        if (ult::touchingBack)
            renderer->drawRoundedRect(buttonStartX + 2.f - s_halfGap, s_buttonY,
                                      s_backWidth - 1.f, 73.0f, 12.0f, a(tsl::clickColor));
        if (ult::touchingSelect.load(std::memory_order_acquire))
            renderer->drawRoundedRect(buttonStartX + 2.f - s_halfGap + s_backWidth + 1.f,
                                      s_buttonY, s_selectWidth - 2.f, 73.0f, 12.0f,
                                      a(tsl::clickColor));

        // --- Footer text ---
        renderer->drawStringWithColoredSections(m_cachedFooterString, false,
            tsl::s_footerSpecialChars, buttonStartX, 693, 23,
            tsl::bottomTextColor, tsl::buttonColor);

        if (!selectIsUsingFocusedColor) {
            static const std::string okOverdraw = "" + ult::GAP_2 + ult::OK + ult::GAP_1;
            renderer->drawStringWithColoredSections(okOverdraw, false, tsl::s_footerSpecialChars,
                buttonStartX + s_backWidth, 693, 23,
                tsl::unfocusedColor, tsl::unfocusedColor);
        }

        // --- Content ---
        if (m_contentElement != nullptr)
            m_contentElement->frame(renderer);

        // --- Edge separator ---
        if (!ult::useRightAlignment)
            renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
        else
            renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));
    }

    // -------------------------------------------------------------------------
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        setBoundaries(parentX, parentY, parentWidth, parentHeight);
        if (m_contentElement != nullptr) {
            const u16 bottomMargin = m_footerHidden ? 8 : 73 + 105 - 97;
            m_contentElement->setBoundaries(parentX + 35, parentY + 97,
                                            parentWidth - 85, parentHeight - 97 - bottomMargin);
            m_contentElement->invalidate();
        }
    }

    tsl::elm::Element* requestFocus(tsl::elm::Element* oldFocus,
                                    tsl::FocusDirection direction) override {
        return m_contentElement
            ? m_contentElement->requestFocus(oldFocus, direction)
            : nullptr;
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (!m_contentElement || !m_contentElement->inBounds(currX, currY))
            return false;
        return m_contentElement->onTouch(event, currX, currY,
                                         prevX, prevY, initialX, initialY);
    }

    void setContent(tsl::elm::Element* content) {
        delete m_contentElement;
        m_contentElement = content;
        if (content != nullptr) {
            m_contentElement->setParent(this);
            invalidate();
        }
    }

    void setPageNames(std::string left, std::string right) {
        m_pageLeftName  = std::move(left);
        m_pageRightName = std::move(right);
        m_footerDirty   = true;
    }

    void setFooterHidden(bool hidden) {
        m_footerHidden = hidden;
        if (hidden) {
            // Kill all footer touch zones so libtesla doesn't map footer touches
            // to B/A button events. backWidth/selectWidth define the x-ranges
            // libtesla uses; leaving them non-zero causes bottom-screen taps to
            // inject HidNpadButton_B every frame → KEY_FIRE → constant weapon fire.
            ult::hasNextPageButton.store(false, std::memory_order_release);
            ult::backWidth.store(0.f,   std::memory_order_release);
            ult::selectWidth.store(0.f, std::memory_order_release);
            ult::halfGap.store(0.f,     std::memory_order_release);
        }
        invalidate();
    }

    tsl::elm::Element* getContent() const { return m_contentElement; }

private:
    tsl::elm::Element* m_contentElement = nullptr;
    std::string        m_pageLeftName;
    std::string        m_pageRightName;

    bool        m_footerHidden         = false;
    bool        m_footerDirty          = true;
    bool        m_hasNextPage          = false;
    std::string m_cachedPageLabel;
    std::string m_cachedFooterString;
    float       m_cachedPageLabelWidth = 0.f;
};
#pragma GCC pop_options
