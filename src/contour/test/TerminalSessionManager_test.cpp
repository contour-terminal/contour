// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure display-session bookkeeping helper used by TerminalSessionManager. The
// manager itself is not headless-constructible (needs ContourGuiApp/PTY/QQuickWindow), so the
// decision logic lives in DisplayState.h and is tested there directly.

#include <contour/DisplayState.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_map>

using contour::detachSessionFromState;
using contour::DisplayState;
using contour::isLastActiveDisplay;
using contour::TerminalSession;

namespace
{
/// A non-null TerminalSession* sentinel used purely as an identity token. detachSessionFromState
/// never dereferences the pointer, so distinct fake addresses are safe stand-ins for real sessions.
TerminalSession* fakeSession(std::uintptr_t tag)
{
    return reinterpret_cast<TerminalSession*>(tag);
}

/// A fake display-pointer identity token. isLastActiveDisplay only compares keys, never dereferences,
/// so distinct addresses stand in for TerminalDisplay* without constructing real displays.
using FakeDisplay = void*;
FakeDisplay fakeDisplay(std::uintptr_t tag)
{
    return reinterpret_cast<FakeDisplay>(tag);
}
} // namespace

TEST_CASE("detachSessionFromState clears only the matching session", "[contour][manager][display]")
{
    auto* sessionA = fakeSession(0x1000);
    auto* sessionB = fakeSession(0x2000);

    SECTION("clears currentSession when it is the detached one")
    {
        DisplayState state { .currentSession = sessionA, .previousSession = nullptr };
        CHECK(detachSessionFromState(state, sessionA));
        CHECK(state.currentSession == nullptr);
    }

    SECTION("clears previousSession too when it matches")
    {
        DisplayState state { .currentSession = sessionB, .previousSession = sessionA };
        CHECK(detachSessionFromState(state, sessionA));
        CHECK(state.currentSession == sessionB); // untouched
        CHECK(state.previousSession == nullptr); // cleared
    }

    SECTION("leaves a state that does not reference the session untouched")
    {
        DisplayState state { .currentSession = sessionB, .previousSession = sessionB };
        CHECK_FALSE(detachSessionFromState(state, sessionA));
        CHECK(state.currentSession == sessionB);
        CHECK(state.previousSession == sessionB);
    }

    SECTION("clears both fields when both reference the detached session")
    {
        DisplayState state { .currentSession = sessionA, .previousSession = sessionA };
        CHECK(detachSessionFromState(state, sessionA));
        CHECK(state.currentSession == nullptr);
        CHECK(state.previousSession == nullptr);
    }
}

TEST_CASE("isLastActiveDisplay decides when closeWindow may tear down shared state",
          "[contour][manager][display]")
{
    // Regression for the "closeWindow wipes all windows" finding: the manager is a singleton shared by
    // every in-process OS window, so the full teardown (remove the shared model window, clear the
    // registries) is only allowed when the LAST window closes. This predicate is the guard.
    auto* displayA = fakeDisplay(0x1000);
    auto* displayB = fakeDisplay(0x2000);

    SECTION("the sole display is the last one -> full teardown")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays { { displayA, {} } };
        CHECK(isLastActiveDisplay(displays, displayA));
    }

    SECTION("another active display remains -> do NOT tear down (would wipe the sibling window)")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays { { displayA, {} }, { displayB, {} } };
        CHECK_FALSE(isLastActiveDisplay(displays, displayA));
        CHECK_FALSE(isLastActiveDisplay(displays, displayB));
    }

    SECTION("the transient null-key entry is ignored (not a live window)")
    {
        // The manager stages a session under a null display key before its window exists; that null
        // key must not count as another window keeping the shared state alive.
        std::unordered_map<FakeDisplay, DisplayState> displays { { displayA, {} }, { nullptr, {} } };
        CHECK(isLastActiveDisplay(displays, displayA));
    }

    SECTION("only the null-key entry besides the closing display -> still the last real window")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays { { nullptr, {} }, { displayA, {} } };
        CHECK(isLastActiveDisplay(displays, displayA));
    }
}

TEST_CASE("isLastActiveDisplay groups split-pane displays by their OS window",
          "[contour][manager][display]")
{
    // Regression for the "closeWindow conflates split-pane displays with OS windows" finding: one OS
    // window with a split tab owns several displays (one per pane). Closing that window must still be
    // treated as closing the LAST window (full teardown), not "one of several windows". Grouping is by
    // the window-identity projection: displays sharing a window id belong to the same OS window.
    auto* displayA = fakeDisplay(0x1000); // pane 1 of window W1
    auto* displayB = fakeDisplay(0x2000); // pane 2 of window W1 (same OS window)
    auto* displayC = fakeDisplay(0x3000); // sole display of a second OS window W2

    auto* windowW1 = fakeDisplay(0xAA00);
    auto* windowW2 = fakeDisplay(0xBB00);

    auto const windowOf = [&](FakeDisplay display) -> FakeDisplay {
        if (display == displayA || display == displayB)
            return windowW1;
        if (display == displayC)
            return windowW2;
        return nullptr; // the transient null-key entry
    };

    SECTION("two panes of the only window -> closing it is the last window (full teardown)")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays { { displayA, {} }, { displayB, {} } };
        CHECK(isLastActiveDisplay(displays, displayA, windowOf));
        CHECK(isLastActiveDisplay(displays, displayB, windowOf));
    }

    SECTION("a split window plus a second window -> NOT the last window")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays {
            { displayA, {} }, { displayB, {} }, { displayC, {} }
        };
        // Closing W1 (a or b): W2's displayC remains -> keep shared state.
        CHECK_FALSE(isLastActiveDisplay(displays, displayA, windowOf));
        CHECK_FALSE(isLastActiveDisplay(displays, displayB, windowOf));
        // Closing W2 (c): W1's two panes remain -> keep shared state.
        CHECK_FALSE(isLastActiveDisplay(displays, displayC, windowOf));
    }

    SECTION("the transient null-key entry never counts as another window")
    {
        std::unordered_map<FakeDisplay, DisplayState> displays {
            { displayA, {} }, { displayB, {} }, { nullptr, {} }
        };
        CHECK(isLastActiveDisplay(displays, displayA, windowOf));
    }
}
