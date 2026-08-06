// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/TabBarMode.hpp>
#include <contour/input/MouseMapping.hpp>

#include <vtbackend/Image.hpp>
#include <vtbackend/primitives.hpp>

#include <vtrasterizer/Decorator.hpp>
#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GridMetrics.hpp>

#include <text_shaper/font.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <variant>

QT_BEGIN_NAMESPACE
class QQuickWindow;
QT_END_NAMESPACE

namespace contour::session
{

class TerminalSession;

/// The view a terminal session drives.
///
/// A @c TerminalSession owns a terminal, not a widget: it decides *that* the screen must be redrawn,
/// *that* the font changed, *that* the application asked for a 80x24 window — and something else turns
/// those into pixels. This interface is that something else, described by what the session needs of it
/// and nothing more.
///
/// Declared HERE, in the layer that CALLS it, and implemented by @c display::TerminalDisplay a layer
/// above (Dependency Inversion). Two things follow, and they are the reason it exists:
///
///  - **The session layer names no display type.** Together with @c display::WindowHost (the same move
///    one layer up) this turns the old session <-> display <-> window cycle into a chain of interfaces
///    that all point downwards.
///  - **A session is testable without a GUI.** Constructing a @c TerminalDisplay needs a QQuickItem, a
///    scene graph and an RHI device; constructing a stub that records calls needs none of those, so
///    "does DECSCUSR reach the cursor shape?" becomes an assertion instead of a manual check. See
///    @c contour::test::FakeDisplaySurface.
///
/// **Threading.** Most of this is GUI-thread-only, but @ref scheduleRedraw, @ref renderBufferUpdated and
/// @ref post are reached from the terminal/parser thread — @ref post being how everything else gets
/// there. An implementation must keep those three safe to call from any thread.
///
/// A session may have no surface at all (a background tab that was never shown, a headless test); every
/// call site guards for it, and this interface adds no obligation to be attached.
class DisplaySurface
{
  public:
    /// Where the next rendered frame is written: a file path, or @c std::monostate for "keep it in
    /// memory". @c std::nullopt disarms the capture.
    using ScreenshotOutput = std::optional<std::variant<std::filesystem::path, std::monostate>>;

    virtual ~DisplaySurface() = default;

    // {{{ Thread hop + lifetime
    /// Runs @p fn on the GUI thread. THE way the terminal thread reaches everything else here.
    /// Cancelled automatically if the surface dies before @p fn runs.
    /// @param fn The work to run.
    virtual void post(std::function<void()> fn) = 0;

    /// Closes the pane this surface renders (the session ended, or asked to be closed).
    virtual void closeDisplay() = 0;

    /// Detaches the session this surface was showing, leaving the surface alive and empty. Called by the
    /// session as it tears down, so the surface never dereferences a dead session.
    virtual void releaseSession() = 0;

    /// The OS window this surface is shown in, or nullptr while it is not in one (a background tab, or
    /// before the first show). Used to reach the screen behind the window; the session does not touch
    /// window geometry, which is @c display::WindowHost's job.
    [[nodiscard]] virtual QQuickWindow* window() const = 0;
    // }}}

    // {{{ Metrics the terminal is sized and reported in
    /// The screen refresh rate to pace the terminal's rendering against.
    [[nodiscard]] virtual vtbackend::RefreshRate refreshRate() const = 0;

    /// One cell, in device pixels.
    [[nodiscard]] virtual vtbackend::ImageSize cellSize() const = 0;

    /// This surface's whole extent, in device pixels (margins included — it describes the area a window
    /// would have to provide). Not what an application is told; that is @ref reportedPixelSize.
    [[nodiscard]] virtual vtbackend::ImageSize pixelSize() const = 0;

    /// The pixel size to tell APPLICATIONS about for a page of @p totalPageSize (CSI 14 t and friends).
    /// Deliberately not the window's pixel size: that includes margins, which come back as cell-size
    /// error once an application divides by the page.
    /// @param totalPageSize The page the report is for; callers mid-resize must pass the NEW page.
    [[nodiscard]] virtual vtbackend::ImageSize reportedPixelSize(vtbackend::PageSize totalPageSize) const = 0;

    /// Device pixels per reported pixel: the divisor taking a device-pixel extent into the unit
    /// @c pixel_reporting selects. 1.0 when reporting device pixels, the content scale otherwise.
    [[nodiscard]] virtual double reportedPixelScale() const = 0;

    /// Device pixels per logical pixel (the DPI scale in force, including any forced-DPI override).
    [[nodiscard]] virtual double contentScale() const = 0;

    /// The renderer's grid metrics — margins and cell size — for mapping a cell to a rectangle.
    [[nodiscard]] virtual vtrasterizer::GridMetrics gridMetrics() const = 0;

    /// Whether this surface has a live render target. False while a pane exists but has never been
    /// composited, which is when configuring it against its geometry would read zeroes.
    [[nodiscard]] virtual bool hasRenderTarget() const = 0;
    // }}}

    // {{{ Sizing
    /// Re-derives the terminal's page size from this surface's current extent (the window->grid
    /// direction), issuing the resulting @c resizeScreen()/SIGWINCH to the child.
    virtual void resizeTerminalToDisplaySize() = 0;

    /// Requests a window resize so the page becomes @p lineCount x @p columnCount (DECSLPP / CSI 8 t).
    /// Relayed to the window layer, which owns window geometry; the WM is free to refuse.
    virtual void resizeWindow(vtbackend::LineCount lineCount, vtbackend::ColumnCount columnCount) = 0;

    /// Pixel-flavored window resize request (CSI 4 t). @see resizeWindow(LineCount, ColumnCount).
    virtual void resizeWindow(vtbackend::Width width, vtbackend::Height height) = 0;
    // }}}

    // {{{ Fonts and decoration
    /// The font family/size the renderer currently has loaded, as an application asks for it (DECRQSS).
    [[nodiscard]] virtual vtbackend::FontDef getFontDef() = 0;

    /// Loads @p fontDescriptions and re-derives the geometry that depends on the resulting cell size.
    /// @param fontDescriptions The font set to load.
    virtual void setFonts(vtrasterizer::FontDescriptions fontDescriptions) = 0;

    /// Changes the font size (the zoom actions and OSC 50).
    /// @param newFontSize The requested size.
    /// @return True if the size was applied; false if it was rejected or the fonts failed to load.
    virtual bool setFontSize(text::FontSize newFontSize) = 0;

    /// Sets how hyperlinks are underlined, normally and while hovered.
    /// @param normal The decoration for a hyperlink at rest.
    /// @param hover  The decoration for the hovered hyperlink.
    virtual void setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover) = 0;

    /// Sets the mouse cursor shape over this surface (the VT-driven shape, or hidden while typing).
    /// @param newCursorShape The shape to show.
    virtual void setMouseCursorShape(input::MouseCursorShape newCursorShape) = 0;

    /// Enables or disables the compositor's blur-behind effect for a translucent background.
    /// @param enable Whether the profile asks for blur.
    virtual void setBlurBehind(bool enable) = 0;
    // }}}

    // {{{ Frames and terminal events
    /// Asks for a frame. Reached from the terminal thread while a VT sequence is processed.
    virtual void scheduleRedraw() = 0;

    /// Signals that the render buffer the surface reads has been refreshed. Terminal thread.
    virtual void renderBufferUpdated() = 0;

    /// Notifies that the active screen buffer changed (primary <-> alternate), so surface state keyed to
    /// the buffer can follow.
    /// @param screenType The buffer now active.
    virtual void bufferChanged(vtbackend::ScreenType screenType) = 0;

    /// Drops any cached GPU resources for @p image, which the terminal is about to forget.
    /// @param image The image being released.
    virtual void discardImage(vtbackend::Image const& image) = 0;

    /// Arms (or disarms) a one-shot capture of the next rendered frame.
    /// @param where Where the frame goes. @see ScreenshotOutput.
    virtual void setScreenshotOutput(ScreenshotOutput where) = 0;

    /// Opens the terminal state inspector, dumping the current state to it.
    virtual void inspect() = 0;

    /// Announces a caret move to the platform: the IME cursor rectangle, and the accessible caret when
    /// assistive technology is listening. Must be reached through @ref post — the terminal-side
    /// notification arrives on the terminal thread, possibly holding the terminal's state mutex.
    virtual void reportCursorMoved() = 0;
    // }}}

    // {{{ Window-scoped actions the session relays
    // These are WINDOW decisions the session is asked to make (a key binding, a VT sequence) while it
    // knows only its surface. The surface forwards them to display::WindowHost, which is where they are
    // actually decided -- and it must, because the request identifies a PANE, and which pane is asking
    // is exactly what the surface knows and the session does not.

    /// Toggles this window between fullscreen and its previous state.
    virtual void toggleFullScreen() = 0;

    /// Toggles this window's title-bar visibility.
    virtual void toggleTitleBar() = 0;

    /// Sets this window's tab-strip visibility mode for as long as the window lives.
    /// @param mode The requested mode.
    virtual void setTabBarVisibility(config::TabBarVisibility mode) = 0;

    /// Sets this window's tab-strip placement for as long as the window lives.
    /// @param position The requested placement.
    virtual void setTabBarPosition(config::TabBarPosition position) = 0;

    /// Toggles whether this surface accepts input-method (IME) composition.
    virtual void toggleInputMethodEditorHandling() = 0;
    // }}}

    // {{{ Driven by the WINDOW layer, not by the session
    // The window layer sizes its window from the pane it is showing, so it reads a surface too --
    // through the same interface, rather than the concrete display type. That is what lets
    // display::WindowHost declare its whole API in terms of a surface and name no display class at all,
    // which is the other half of breaking the display <-> window cycle.

    /// Whether this surface is currently showing a session. False for a surface between sessions (a
    /// split hand-off, a collapsing pane), where everything derived from the session would be stale.
    [[nodiscard]] virtual bool hasSession() const = 0;

    /// The session this surface is showing. Callers must have checked @ref hasSession first.
    [[nodiscard]] virtual TerminalSession& session() = 0;

    /// Re-derives the font DPI, cell size and grid after the window's device-pixel ratio settled, so a
    /// caller about to compute geometry from @ref cellSize does not read pre-change metrics.
    virtual void applyContentScaleChange() = 0;
    // }}}
};

} // namespace contour::session
