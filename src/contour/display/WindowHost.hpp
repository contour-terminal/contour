// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/TabBarMode.hpp>
#include <contour/session/DisplaySurface.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <cstdint>

namespace contour::display
{

/// The OS window, as a display sees it.
///
/// A display renders one pane inside a window it does not own: full-screening, resizing, the WM size
/// hints and the window-scoped chrome (title bar, tab strip) are all decisions for the window layer, and
/// a display forwards them rather than making them. This interface is the whole of that forwarding —
/// every call @c TerminalDisplay makes upwards is declared here and nowhere else.
///
/// Declared HERE, in the layer that CALLS it, and implemented by @c window::WindowController a layer
/// above (Dependency Inversion): the display therefore names no window type, and the two directions of
/// what used to be a cycle both point at this header. A display resolves its host through
/// @c session::TerminalSessionManager::windowHostForDisplay(), which routes by the display's OS window;
/// a display with no window yet (offscreen tests, pre-show) simply has no host, and every call site is
/// written to tolerate that.
class WindowHost
{
  public:
    /// How much of the WM size-hint set @ref updateSizeHintsFor may (re)write.
    ///
    /// The character-cell resize grid (base + increment, the X11 @c PResizeInc pair) is deliberately
    /// CLEARED while the window is maximized/fullscreen (see @c showWithoutSizeIncrements) so the WM
    /// fills the screen exactly rather than snapping to the nearest cell multiple. An INCIDENTAL hint
    /// refresh (a split's font reconcile, a DPR settle, a title-bar toggle) must therefore not re-arm
    /// the increment while the window is non-normal — on WMs that honor @c PResizeInc that re-writes a
    /// sub-cell gap around the maximized window, and can even drop the maximized state. The @c minimum
    /// hint is always safe (it never disturbs the maximized state) and is written unconditionally.
    enum class HintApplyMode : uint8_t
    {
        /// Incidental refresh: write the resize grid only while the window is @c Windowed; write
        /// @c minimum always. Used by font/DPI/chrome refreshes that may fire while maximized.
        RespectWindowState,
        /// Establishing the normal-state hints: write the full set unconditionally. Used by the
        /// restore-into-normal paths, which call this BEFORE @c showNormal() settles @c visibility()
        /// (so a live @c visibility() read would still see the old maximized value); they KNOW the
        /// window is becoming normal, so intent — not the not-yet-settled state — drives the write.
        Full,
    };

    virtual ~WindowHost() = default;

    // {{{ Window-scoped chrome
    /// Seeds this window's title-bar visibility from a profile, first-write-wins: re-applying the
    /// profile value on every session rebind would silently revert a runtime toggle on each tab switch.
    /// @param visible The profile's @c show_title_bar value.
    virtual void seedTitleBarVisible(bool visible) = 0;

    /// Flips this window's title-bar visibility. Window state, so a toggle from any pane applies to the
    /// whole window and survives pane-focus changes and tab switches.
    virtual void toggleTitleBar() = 0;

    /// Seeds this window's tab-strip placement from the configuration. @see seedTitleBarVisible.
    /// @param position The configured placement.
    virtual void seedTabBarPosition(config::TabBarPosition position) = 0;

    /// Seeds this window's tab-strip visibility mode from the configuration. @see seedTitleBarVisible.
    /// @param visibility The configured visibility mode.
    virtual void seedTabBarVisibility(config::TabBarVisibility visibility) = 0;

    /// Sets this window's tab-strip placement for as long as the window lives (a configuration reload
    /// supersedes it).
    /// @param position The requested placement.
    virtual void setTabBarPosition(config::TabBarPosition position) = 0;

    /// Sets this window's tab-strip visibility mode for as long as the window lives.
    /// @param visibility The requested visibility mode.
    virtual void setTabBarVisibility(config::TabBarVisibility visibility) = 0;
    // }}}

    // {{{ Window geometry — the display never touches QWindow geometry itself
    /// Recomputes and applies the WM size hints (minimum/base/increment) for this window from
    /// @p requester's cell geometry, profile margins, content scale and the declared chrome.
    /// Refresh triggers: font/DPI reconfiguration, chrome changes, restore-from-fullscreen/maximize.
    /// NEVER called from a resize path.
    /// @param requester The display whose cell geometry defines the hints (the active pane).
    /// @param mode Whether to gate the resize grid on the live window state (@ref HintApplyMode).
    virtual void updateSizeHintsFor(session::DisplaySurface& requester,
                                    HintApplyMode mode = HintApplyMode::RespectWindowState) = 0;

    /// Shows this window fullscreen (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    virtual void setWindowFullScreen(session::DisplaySurface& requester) = 0;

    /// Shows this window maximized (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    virtual void setWindowMaximized(session::DisplaySurface& requester) = 0;

    /// Restores this window to normal state and re-applies the WM size hints.
    /// @param requester The display forwarding the request (resolves the OS window).
    virtual void setWindowNormal(session::DisplaySurface& requester) = 0;

    /// Toggles fullscreen, restoring the previous maximized state on exit.
    /// @param requester The display forwarding the request (resolves the OS window).
    virtual void toggleFullScreen(session::DisplaySurface& requester) = 0;

    /// THE grid->window choke point: every programmatic window resize lands here; nothing else may
    /// resize the window. The resulting WM resize event drives the grid through the normal window->grid
    /// path; the WM is free to refuse.
    /// @param requester     The display whose pane the grid request targets.
    /// @param totalPageSize The requested total page (main page + status line).
    /// @return True if a window resize was issued.
    virtual bool resizeWindowForPage(session::DisplaySurface& requester,
                                     vtbackend::PageSize totalPageSize) = 0;

    /// Pixel-flavored choke-point entry (CSI 4 t); otherwise identical to @ref resizeWindowForPage.
    /// @param requester       The display whose pane the request targets.
    /// @param contentDevicePx The requested pane content extent in device pixels.
    /// @return True if a window resize was issued.
    virtual bool resizeWindowForContentPixels(session::DisplaySurface& requester,
                                              vtbackend::ImageSize contentDevicePx) = 0;
    // }}}
};

} // namespace contour::display
