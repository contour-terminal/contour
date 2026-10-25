// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TitleBandLayout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>

using namespace contour::window;
using contour::config::TabBarPosition;
using contour::config::TabShape;

namespace
{

struct BandCase
{
    TitleBandInputs inputs;
    TitleBand expected;
};

constexpr auto Pill = TabShape::Pill;
constexpr auto Flat = TabShape::Flat;
constexpr auto ClientBand = WindowFrame::ClientBand;
constexpr auto NativeFrame = WindowFrame::NativeFrame;
constexpr auto Windowed = WindowMode::Windowed;
constexpr auto GateHidden = TabStripGate::Hidden;
constexpr auto GateShown = TabStripGate::Shown;
constexpr auto Top = TabBarPosition::Top;
constexpr auto Bottom = TabBarPosition::Bottom;

// One row per R-e rule, under both gates and both edges where the rule reads them.
constexpr auto BandCases = std::array {
    // Pill on a client band: the band is always shown at the top, for the traffic lights (D1).
    BandCase { { Pill, ClientBand, Windowed, GateHidden, Top },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, Windowed, GateShown, Top },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    // Pill with the tabs at the bottom: the title band stays on top, the tabs get their own bar (D6).
    BandCase { { Pill, ClientBand, Windowed, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Shown } },
    BandCase { { Pill, ClientBand, Windowed, GateHidden, Bottom },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    // Flat on a client band: the band is the strip, per the gate, at its edge (today).
    BandCase { { Flat, ClientBand, Windowed, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateHidden, Top },
               { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateShown, Top },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateHidden, Bottom },
               { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    // A native frame: no band of ours, the strip per the gate at its edge, whatever the shape.
    BandCase { { Pill, NativeFrame, Windowed, GateShown, Top },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateHidden, Top },
               { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateHidden, Bottom },
               { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateHidden, Top },
               { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
};

} // namespace

TEST_CASE("titleBand.rules", "[titleband]")
{
    for (auto const& [inputs, expected]: BandCases)
    {
        INFO("shape " << int(inputs.shape) << ", frame " << int(inputs.frame) << ", gate "
                      << int(inputs.strip) << ", position " << int(inputs.position));
        CHECK(resolveTitleBand(inputs) == expected);
    }
    static_assert(resolveTitleBand({}) == TitleBand { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs });
}

TEST_CASE("titleBand.everyInputMatchesARow", "[titleband]")
{
    for (auto const shape: { Flat, Pill })
        for (auto const frame: { ClientBand, NativeFrame })
            for (auto const gate: { GateHidden, GateShown })
                for (auto const position: { Top, Bottom })
                    CHECK(findTitleBandRule({ shape, frame, Windowed, gate, position }) != nullptr);
}

TEST_CASE("titleBand.fullScreenReveals", "[titleband]")
{
    for (auto const& [inputs, windowed]: BandCases)
    {
        auto fullScreenInputs = inputs;
        fullScreenInputs.mode = WindowMode::FullScreen;
        auto expected = windowed;
        if (windowed.band == BarPresence::Shown && windowed.bandEdge == BarEdge::Top)
            expected.band = BarPresence::RevealOnEdge;
        auto const actual = resolveTitleBand(fullScreenInputs);
        CHECK(actual == expected);
        if (windowed.bandEdge == BarEdge::Bottom)
            CHECK(actual.band == windowed.band);
        CHECK(actual.tabBar == windowed.tabBar);
    }
    CHECK(resolveTitleBand({ Pill, ClientBand, WindowMode::FullScreen, GateShown, Bottom })
          == TitleBand { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Title, BarPresence::Shown });
    CHECK(resolveTitleBand({ Flat, ClientBand, WindowMode::FullScreen, GateShown, Bottom })
          == TitleBand { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden });
}

TEST_CASE("titleBand.names", "[titleband]")
{
    constexpr auto PresenceNames = std::array { "hidden", "shown", "reveal" };
    constexpr auto EdgeNames = std::array { "top", "bottom" };
    constexpr auto CentreNames = std::array { "empty", "tabs", "title" };
    REQUIRE(PresenceNames.size() == BarPresenceCount);
    REQUIRE(EdgeNames.size() == BarEdgeCount);
    REQUIRE(CentreNames.size() == BarCentreCount);
    for (auto const i: std::views::iota(size_t { 0 }, BarPresenceCount))
        CHECK(barPresenceName(BarPresence(i)) == PresenceNames[i]);
    for (auto const i: std::views::iota(size_t { 0 }, BarEdgeCount))
        CHECK(barEdgeName(BarEdge(i)) == EdgeNames[i]);
    for (auto const i: std::views::iota(size_t { 0 }, BarCentreCount))
        CHECK(barCentreName(BarCentre(i)) == CentreNames[i]);
}
