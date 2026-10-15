// SPDX-License-Identifier: Apache-2.0
//
// Where a session is, for every way it can be placed. Qt-free on purpose: the decision lives in a header
// with no Qt in it, which is what lets contour_test (built in both frontend configurations) cover it.

#include <contour/platform/SessionVisibility.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>

using contour::platform::SessionVisibility;

TEST_CASE("SessionVisibility.the decision table", "[notification]")
{
    using contour::platform::PaneExposure;
    using contour::platform::SessionFocus;
    using contour::platform::SessionPlacement;
    using contour::platform::sessionVisibilityFor;
    using contour::platform::TabSelection;
    using contour::platform::WindowPresence;

    constexpr auto Here = SessionFocus::Here;
    constexpr auto Elsewhere = SessionFocus::Elsewhere;
    constexpr auto Active = TabSelection::Active;
    constexpr auto Background = TabSelection::Background;
    constexpr auto PaneShown = PaneExposure::Shown;
    constexpr auto Covered = PaneExposure::Covered;
    constexpr auto WindowShown = WindowPresence::Shown;
    constexpr auto Minimized = WindowPresence::Minimized;
    constexpr auto Focused = SessionVisibility::Focused;
    constexpr auto Visible = SessionVisibility::VisibleUnfocused;
    constexpr auto Hidden = SessionVisibility::Hidden;

    struct Row
    {
        SessionPlacement placement;
        SessionVisibility expected = SessionVisibility::Hidden;
    };

    // All sixteen placements, spelled out. A minimised window hides everything in it; otherwise focus
    // decides; without focus, only the shown tab's uncovered panes are on screen.
    auto const rows = std::array {
        Row { { Here, Active, PaneShown, WindowShown }, Focused },
        Row { { Here, Active, PaneShown, Minimized }, Hidden },
        Row { { Here, Active, Covered, WindowShown }, Focused },
        Row { { Here, Active, Covered, Minimized }, Hidden },
        Row { { Here, Background, PaneShown, WindowShown }, Focused },
        Row { { Here, Background, PaneShown, Minimized }, Hidden },
        Row { { Here, Background, Covered, WindowShown }, Focused },
        Row { { Here, Background, Covered, Minimized }, Hidden },
        Row { { Elsewhere, Active, PaneShown, WindowShown }, Visible },
        Row { { Elsewhere, Active, PaneShown, Minimized }, Hidden },
        Row { { Elsewhere, Active, Covered, WindowShown }, Hidden },
        Row { { Elsewhere, Active, Covered, Minimized }, Hidden },
        Row { { Elsewhere, Background, PaneShown, WindowShown }, Hidden },
        Row { { Elsewhere, Background, PaneShown, Minimized }, Hidden },
        Row { { Elsewhere, Background, Covered, WindowShown }, Hidden },
        Row { { Elsewhere, Background, Covered, Minimized }, Hidden },
    };

    for (auto const& row: rows)
    {
        INFO(std::format("focus={} tab={} pane={} window={}",
                         static_cast<int>(row.placement.focus),
                         static_cast<int>(row.placement.tab),
                         static_cast<int>(row.placement.pane),
                         static_cast<int>(row.placement.window)));
        CHECK(sessionVisibilityFor(row.placement) == row.expected);
    }

    // The default placement -- a session nothing places -- is off screen, and decided at compile time.
    static_assert(sessionVisibilityFor(SessionPlacement {}) == SessionVisibility::Hidden);
}
