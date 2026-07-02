// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

namespace contour
{

class TerminalSession;

/// What a single display currently and previously showed. The manager keeps one per display so it
/// can restore/activate the right session on focus changes.
///
/// Kept in its own dependency-free header (only a forward-declared TerminalSession) so the pure
/// session-bookkeeping logic below is unit-testable without the full Qt/app manager.
struct DisplayState
{
    TerminalSession* currentSession = nullptr;
    TerminalSession* previousSession = nullptr;
};

/// Clears any reference to @p session from @p state, used when a display loses @p session to another
/// display (TerminalSession::attachDisplay). Pure pointer-identity logic (never dereferences @p
/// session), so a later FocusOnDisplay on the losing display does not re-activate a session it no
/// longer owns.
/// @param state The display state to scrub.
/// @param session The session being detached.
/// @return true if @p state referenced @p session (and was changed), false otherwise.
[[nodiscard]] inline bool detachSessionFromState(DisplayState& state, TerminalSession const* session) noexcept
{
    bool changed = false;
    if (state.currentSession == session)
    {
        state.currentSession = nullptr;
        changed = true;
    }
    if (state.previousSession == session)
    {
        state.previousSession = nullptr;
        changed = true;
    }
    return changed;
}

/// Whether closing @p closingDisplay should tear down the manager's process-wide shared state (the
/// model window, session list and registries) rather than just detach that one display.
///
/// The manager is a singleton shared by every in-process OS window, so the shared state may only be
/// destroyed when the LAST OS WINDOW closes. The catch is that one OS window can own SEVERAL displays
/// at once — a split tab renders one TerminalDisplay per visible pane, all in the same OS window — so
/// a raw "any other display remains" test wrongly keeps the shared state alive when the only window
/// has split panes (the "closeWindow skips teardown / leaks sessions" finding). Grouping is therefore
/// by OS WINDOW, not by display: @p windowOf maps a display to an opaque per-OS-window identity (e.g.
/// QQuickItem::window()), and a full teardown is correct exactly when no OTHER display belonging to a
/// DIFFERENT window than @p closingDisplay's remains.
///
/// The map also holds a transient null-key entry (a session staged for a not-yet-created display);
/// that null key is not a live window and is ignored. Pure identity logic over the key set and the
/// projected window ids (neither is dereferenced), so it is unit-testable with fake pointers.
/// @tparam DisplayStates A map type whose keys are display pointers (e.g. TerminalDisplay*).
/// @tparam Display The display pointer type.
/// @tparam WindowOf A callable mapping a display key to its OS-window identity.
/// @param displayStates The manager's per-display state map.
/// @param closingDisplay The display whose window is being closed (must be non-null).
/// @param windowOf Projects a display to the (comparable) identity of its OS window.
/// @return true if this is the last active window (do a full teardown), false otherwise.
template <typename DisplayStates, typename Display, typename WindowOf>
[[nodiscard]] bool isLastActiveDisplay(DisplayStates const& displayStates,
                                       Display closingDisplay,
                                       WindowOf windowOf) noexcept
{
    auto const closingWindow = windowOf(closingDisplay);
    return !std::ranges::any_of(displayStates, [&](auto const& entry) {
        return entry.first != nullptr && windowOf(entry.first) != closingWindow;
    });
}

/// Display-identity overload: treats each display as its own window (no split-pane grouping). Used by
/// callers without a window projection and by the unit tests that exercise the pure key logic.
/// @param displayStates The manager's per-display state map.
/// @param closingDisplay The display whose window is being closed (must be non-null).
/// @return true if no other non-null display remains, false otherwise.
template <typename DisplayStates, typename Display>
[[nodiscard]] bool isLastActiveDisplay(DisplayStates const& displayStates, Display closingDisplay) noexcept
{
    return isLastActiveDisplay(displayStates, closingDisplay, [](Display display) { return display; });
}

} // namespace contour
