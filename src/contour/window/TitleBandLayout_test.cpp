// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TitleBandLayout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>

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
    BandCase { { Flat, NativeFrame, Windowed, GateShown, Top },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateHidden, Bottom },
               { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
};

constexpr auto FullScreen = WindowMode::FullScreen;
constexpr auto Interrupted = HostNotice::Interrupted;

// In full screen a band shown at the top reveals on the edge (D3); a bottom band and the tab bar stay as
// they are windowed. A remote window whose connection is interrupted keeps its top band pinned instead,
// overlaying the top rows: its chip is all that says why the screen stopped.
constexpr auto FullScreenCases = std::array {
    BandCase { { Pill, ClientBand, FullScreen, GateShown, Top },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, FullScreen, GateHidden, Top },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, FullScreen, GateShown, Bottom },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Title, BarPresence::Shown } },
    BandCase { { Pill, ClientBand, FullScreen, GateHidden, Bottom },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateShown, Top },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateHidden, Top },
               { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateHidden, Bottom },
               { BarPresence::Hidden, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, FullScreen, GateShown, Top },
               { BarPresence::RevealOnEdge, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, FullScreen, GateShown, Bottom },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, FullScreen, GateHidden, Top },
               { BarPresence::Hidden, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateHidden, Top, Interrupted },
               { BarPresence::Pinned, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, FullScreen, GateShown, Top, Interrupted },
               { BarPresence::Pinned, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, FullScreen, GateHidden, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
};

// A remote window whose connection is not up: wherever the band is the strip, it shows whatever the gate
// says, since it is the host chip's home (R-d). The Pill band on a client band shows anyway.
constexpr auto InterruptedCases = std::array {
    BandCase { { Pill, ClientBand, Windowed, GateHidden, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, Windowed, GateShown, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, ClientBand, Windowed, GateShown, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Shown } },
    BandCase { { Pill, ClientBand, Windowed, GateHidden, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Title, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateShown, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateHidden, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateShown, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, ClientBand, Windowed, GateHidden, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateShown, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateHidden, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateShown, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Pill, NativeFrame, Windowed, GateHidden, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateShown, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateHidden, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateShown, Top, Interrupted },
               { BarPresence::Shown, BarEdge::Top, BarCentre::Tabs, BarPresence::Hidden } },
    BandCase { { Flat, NativeFrame, Windowed, GateHidden, Bottom, Interrupted },
               { BarPresence::Shown, BarEdge::Bottom, BarCentre::Tabs, BarPresence::Hidden } },
};

/// @return Whether @p cases holds a row for @p inputs.
[[nodiscard]] bool hasRowFor(std::span<BandCase const> cases, TitleBandInputs const& inputs)
{
    return std::ranges::any_of(cases, [&](BandCase const& row) {
        return row.inputs.shape == inputs.shape && row.inputs.frame == inputs.frame
               && row.inputs.mode == inputs.mode && row.inputs.strip == inputs.strip
               && row.inputs.position == inputs.position && row.inputs.notice == inputs.notice;
    });
}

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

TEST_CASE("titleBand.casesCoverEveryInput", "[titleband]")
{
    // Every windowed input has a row above: a rule change cannot slip past the table.
    detail::forEachTitleBandInput([](TitleBandInputs const& inputs) {
        INFO("shape " << int(inputs.shape) << ", frame " << int(inputs.frame) << ", gate "
                      << int(inputs.strip) << ", position " << int(inputs.position) << ", notice "
                      << int(inputs.notice));
        CHECK(hasRowFor(inputs.notice == HostNotice::None ? std::span<BandCase const> { BandCases }
                                                          : std::span<BandCase const> { InterruptedCases },
                        inputs));
    });
}

TEST_CASE("titleBand.anInterruptedConnectionShowsTheStrip", "[titleband]")
{
    for (auto const& [inputs, expected]: InterruptedCases)
    {
        INFO("shape " << int(inputs.shape) << ", frame " << int(inputs.frame) << ", gate "
                      << int(inputs.strip) << ", position " << int(inputs.position));
        CHECK(resolveTitleBand(inputs) == expected);
    }
    // In full screen the forced band stays pinned rather than revealing on the edge: nothing else says why
    // the screen stopped.
    CHECK(resolveTitleBand({ Flat, ClientBand, FullScreen, GateHidden, Top, Interrupted }).band
          == BarPresence::Pinned);
}

TEST_CASE("titleBand.fullScreenReveals", "[titleband]")
{
    for (auto const& [inputs, expected]: FullScreenCases)
    {
        INFO("shape " << int(inputs.shape) << ", frame " << int(inputs.frame) << ", gate "
                      << int(inputs.strip) << ", position " << int(inputs.position));
        CHECK(resolveTitleBand(inputs) == expected);
    }
}

TEST_CASE("titleBand.names", "[titleband]")
{
    constexpr auto PresenceNames = std::array { "hidden", "shown", "reveal", "pinned" };
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
