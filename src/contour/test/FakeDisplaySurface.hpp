// SPDX-License-Identifier: Apache-2.0
#pragma once

// A recording session::DisplaySurface, so what a session ASKS OF ITS VIEW is assertable.
//
// Before DisplaySurface existed, the only view was TerminalDisplay -- a QQuickItem needing a window, a
// scene graph and an RHI device -- so every session test ran display-less and could assert only that
// the view-dependent paths did not crash. "Does DECSCUSR reach the cursor shape?" and "does CSI 8 t ask
// the window to resize?" had no home. This is that home.

#include <contour/session/DisplaySurface.hpp>

#include <QtQuick/QQuickWindow>

#include <functional>
#include <utility>
#include <vector>

namespace contour::test
{

/// A @c session::DisplaySurface that records what it is told and answers metrics a test sets.
///
/// Deliberately NOT a mock framework: the recorded vectors ARE the assertions, and a test reads them
/// directly. The metric fields below are plain public data, so a test states the geometry it wants to
/// reason about in one line instead of standing up a renderer to produce it.
class FakeDisplaySurface final: public session::DisplaySurface
{
  public:
    // {{{ What the surface answers — set these before attaching
    vtbackend::ImageSize cellSizeValue { vtbackend::Width(8), vtbackend::Height(16) };
    vtbackend::ImageSize pixelSizeValue { vtbackend::Width(640), vtbackend::Height(384) };
    vtbackend::ImageSize reportedPixelSizeValue { vtbackend::Width(640), vtbackend::Height(384) };
    vtbackend::RefreshRate refreshRateValue { 60.0 };
    double reportedPixelScaleValue = 1.0;
    double contentScaleValue = 1.0;
    vtrasterizer::GridMetrics gridMetricsValue {};
    vtbackend::FontDef fontDefValue {};
    /// What @ref setFontSize reports back — a test flips it to model a font that failed to load.
    bool fontSizeApplies = true;
    /// True once a test decides the surface is composited; @c configureDisplay() gates on it.
    bool renderTargetPresent = false;
    // }}}

    // {{{ What the session asked for
    std::vector<input::MouseCursorShape> cursorShapes;
    std::vector<vtrasterizer::FontDescriptions> fontsSet;
    std::vector<text::font_size> fontSizesRequested;
    std::vector<std::pair<vtbackend::LineCount, vtbackend::ColumnCount>> pageResizeRequests;
    std::vector<std::pair<vtbackend::Width, vtbackend::Height>> pixelResizeRequests;
    std::vector<std::pair<vtrasterizer::Decorator, vtrasterizer::Decorator>> hyperlinkDecorations;
    std::vector<vtbackend::ScreenType> bufferChanges;
    std::vector<ScreenshotOutput> screenshotOutputs;
    std::vector<config::TabBarVisibility> tabBarVisibilities;
    std::vector<config::TabBarPosition> tabBarPositions;
    std::vector<bool> blurBehindRequests;

    int postCount = 0;
    int redrawCount = 0;
    int renderBufferUpdateCount = 0;
    int closeCount = 0;
    int releaseSessionCount = 0;
    int inspectCount = 0;
    int discardedImages = 0;
    int cursorMovedReports = 0;
    int terminalResizeRequests = 0;
    int fullScreenToggles = 0;
    int titleBarToggles = 0;
    int imeToggles = 0;
    int contentScaleChangesApplied = 0;
    // }}}

    /// Whether posted work runs immediately (the default, so a test asserts the EFFECT of a post) or is
    /// only counted. Set to false to inspect the un-run queue — the state a session sees mid-post.
    bool runPostsImmediately = true;
    /// Posted work that has not run, newest last. Only populated while @ref runPostsImmediately is false.
    std::vector<std::function<void()>> pendingPosts;

    /// Drains @ref pendingPosts in order (a hand-pumped GUI thread).
    void drainPosts()
    {
        auto posts = std::exchange(pendingPosts, {});
        for (auto& fn: posts)
            fn();
    }

    /// The session this surface shows. Assigned by the test right after attachDisplay(); needed only by
    /// the window-layer half of the interface, which a session test does not exercise.
    session::TerminalSession* attachedSession = nullptr;

    // {{{ session::DisplaySurface
    void post(std::function<void()> fn) override
    {
        ++postCount;
        if (runPostsImmediately)
            fn();
        else
            pendingPosts.push_back(std::move(fn));
    }

    void closeDisplay() override { ++closeCount; }
    void releaseSession() override { ++releaseSessionCount; }
    [[nodiscard]] QQuickWindow* window() const override { return nullptr; }

    [[nodiscard]] vtbackend::RefreshRate refreshRate() const override { return refreshRateValue; }
    [[nodiscard]] vtbackend::ImageSize cellSize() const override { return cellSizeValue; }
    [[nodiscard]] vtbackend::ImageSize pixelSize() const override { return pixelSizeValue; }
    [[nodiscard]] vtbackend::ImageSize reportedPixelSize(vtbackend::PageSize /*totalPageSize*/) const override
    {
        return reportedPixelSizeValue;
    }
    [[nodiscard]] double reportedPixelScale() const override { return reportedPixelScaleValue; }
    [[nodiscard]] double contentScale() const override { return contentScaleValue; }
    [[nodiscard]] vtrasterizer::GridMetrics gridMetrics() const override { return gridMetricsValue; }
    [[nodiscard]] bool hasRenderTarget() const override { return renderTargetPresent; }

    void resizeTerminalToDisplaySize() override { ++terminalResizeRequests; }
    void resizeWindow(vtbackend::LineCount lineCount, vtbackend::ColumnCount columnCount) override
    {
        pageResizeRequests.emplace_back(lineCount, columnCount);
    }
    void resizeWindow(vtbackend::Width width, vtbackend::Height height) override
    {
        pixelResizeRequests.emplace_back(width, height);
    }

    [[nodiscard]] vtbackend::FontDef getFontDef() override { return fontDefValue; }
    void setFonts(vtrasterizer::FontDescriptions fontDescriptions) override
    {
        fontsSet.push_back(std::move(fontDescriptions));
    }
    bool setFontSize(text::font_size newFontSize) override
    {
        fontSizesRequested.push_back(newFontSize);
        return fontSizeApplies;
    }
    void setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover) override
    {
        hyperlinkDecorations.emplace_back(normal, hover);
    }
    void setMouseCursorShape(input::MouseCursorShape newCursorShape) override
    {
        cursorShapes.push_back(newCursorShape);
    }
    void setBlurBehind(bool enable) override { blurBehindRequests.push_back(enable); }

    void scheduleRedraw() override { ++redrawCount; }
    void renderBufferUpdated() override { ++renderBufferUpdateCount; }
    void bufferChanged(vtbackend::ScreenType screenType) override { bufferChanges.push_back(screenType); }
    void discardImage(vtbackend::Image const& /*image*/) override { ++discardedImages; }
    void setScreenshotOutput(ScreenshotOutput where) override
    {
        screenshotOutputs.push_back(std::move(where));
    }
    void inspect() override { ++inspectCount; }
    void reportCursorMoved() override { ++cursorMovedReports; }

    void toggleFullScreen() override { ++fullScreenToggles; }
    void toggleTitleBar() override { ++titleBarToggles; }
    void setTabBarVisibility(config::TabBarVisibility mode) override { tabBarVisibilities.push_back(mode); }
    void setTabBarPosition(config::TabBarPosition position) override { tabBarPositions.push_back(position); }
    void toggleInputMethodEditorHandling() override { ++imeToggles; }

    [[nodiscard]] bool hasSession() const override { return attachedSession != nullptr; }
    [[nodiscard]] session::TerminalSession& session() override { return *attachedSession; }
    void applyContentScaleChange() override { ++contentScaleChangesApplied; }
    // }}}
};

} // namespace contour::test
