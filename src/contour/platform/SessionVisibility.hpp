// SPDX-License-Identifier: Apache-2.0
//
// Where a session sits relative to what the user is looking at, decided from how it is placed. Header-only,
// constexpr and std-only, so it is table-tested in contour_test; the session manager gathers the facts.
#pragma once

#include <cstdint>

namespace contour::platform
{

/// Where a session sits relative to what the user is looking at.
enum class SessionVisibility : uint8_t
{
    Focused = 0,      ///< The session holding terminal focus in the active window.
    VisibleUnfocused, ///< On screen -- its tab is shown, nothing covers its pane -- but focus is elsewhere.
    Hidden,           ///< Off screen: another tab is active, a sibling is zoomed, or the window is minimised.
};

/// Whether a session holds the application's single terminal focus.
enum class SessionFocus : uint8_t
{
    Elsewhere = 0, ///< Another session, or no session at all, holds the focus.
    Here,          ///< This session holds the focus.
};

/// Whether the tab hosting a session is the one its window shows.
enum class TabSelection : uint8_t
{
    Background = 0, ///< Another tab of the window is shown.
    Active,         ///< The session's tab is the window's shown tab.
};

/// Whether a session's pane is drawn within its tab. A zoomed sibling covers every other pane.
enum class PaneExposure : uint8_t
{
    Shown = 0, ///< The pane is drawn.
    Covered,   ///< A zoomed sibling fills the tab instead.
};

/// Whether the window hosting a session is on screen at all.
enum class WindowPresence : uint8_t
{
    Shown = 0, ///< The window is not minimised.
    Minimized, ///< The window is minimised (as far as the platform reports it).
};

/// The four facts SessionVisibility is decided from, as the session manager gathers them.
struct SessionPlacement
{
    SessionFocus focus = SessionFocus::Elsewhere;  ///< Whether the session holds terminal focus.
    TabSelection tab = TabSelection::Background;   ///< Whether its tab is the window's shown one.
    PaneExposure pane = PaneExposure::Shown;       ///< Whether a zoomed sibling covers its pane.
    WindowPresence window = WindowPresence::Shown; ///< Whether its window is minimised.

    bool operator==(SessionPlacement const&) const = default;
};

/// Decides where a session is from how it is placed.
///
/// A minimised window hides everything in it, even the pane holding focus (a window manager that
/// minimises without deactivating must not make a build in it count as watched). Otherwise focus
/// decides; and an unfocused pane is on screen only while its tab is the shown one and no zoomed
/// sibling covers it.
/// @param placement The facts, as the session manager gathers them.
/// @return Where the session is.
[[nodiscard]] constexpr SessionVisibility sessionVisibilityFor(SessionPlacement placement) noexcept
{
    if (placement.window == WindowPresence::Minimized)
        return SessionVisibility::Hidden;
    if (placement.focus == SessionFocus::Here)
        return SessionVisibility::Focused;
    if (placement.tab == TabSelection::Active && placement.pane == PaneExposure::Shown)
        return SessionVisibility::VisibleUnfocused;
    return SessionVisibility::Hidden;
}

} // namespace contour::platform
