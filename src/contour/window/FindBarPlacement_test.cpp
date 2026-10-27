// SPDX-License-Identifier: Apache-2.0
#include <contour/window/FindBarPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <set>
#include <string_view>

using contour::config::FindBarHost;
using contour::window::FindBarAnchor;
using contour::window::FindBarAnchorCount;
using contour::window::FindBarInputs;
using contour::window::FindBarPlacement;
using contour::window::PaneHeaderPresence;
using contour::window::placeFindBar;
using contour::window::RowBand;

namespace
{
/// The canvas's pane: 800 x 400, a 380 x 32 bar 8 in from the edges.
constexpr auto Base = FindBarInputs {
    .paneWidth = 800, .paneHeight = 400, .barWidth = 380, .barHeight = 32, .insetX = 8, .insetY = 8
};
} // namespace

TEST_CASE("findBar.topRightOfAPane", "[findbar]")
{
    CHECK(placeFindBar(Base)
          == FindBarPlacement { .anchor = FindBarAnchor::TopRight, .x = 412, .y = 8, .width = 380 });
}

TEST_CASE("findBar.underTheHeaderIsTheTerminalsTop", "[findbar]")
{
    // R-i: the bar is placed in the pane's terminal, which the card places below its header and the card's
    // content inset; so "under the header" is the terminal's own top plus the inset, the same place as on a
    // pane with no header. In the card's coordinates that is the header, the inset and these 8 (F5).
    auto inputs = Base;
    inputs.header = PaneHeaderPresence::Present;
    CHECK(placeFindBar(inputs) == placeFindBar(Base));
}

TEST_CASE("findBar.movesBelowWhenTheCurrentMatchIsUnderIt", "[findbar]")
{
    auto inputs = Base;
    inputs.currentMatch = RowBand { .top = 10, .bottom = 30 };
    CHECK(placeFindBar(inputs)
          == FindBarPlacement { .anchor = FindBarAnchor::BottomRight, .x = 412, .y = 360, .width = 380 });

    inputs.currentMatch = RowBand { .top = 50, .bottom = 70 };
    CHECK(placeFindBar(inputs).anchor == FindBarAnchor::TopRight);

    // Nowhere better: the bottom place would overlap the top one.
    inputs.currentMatch = RowBand { .top = 10, .bottom = 30 };
    inputs.paneHeight = 60;
    CHECK(placeFindBar(inputs).anchor == FindBarAnchor::TopRight);
}

TEST_CASE("findBar.aMatchUnderBothPlacesKeepsTheBarOnTop", "[findbar]")
{
    // M2: a match tall enough to lie under the top place and the bottom one (360-392) as well -- a long
    // wrapped line -- gains nothing from the move, so the bar stays where it was.
    auto inputs = Base;
    inputs.currentMatch = RowBand { .top = 20, .bottom = 380 };
    CHECK(placeFindBar(inputs).anchor == FindBarAnchor::TopRight);
    // Ending just above the bottom place, it is moved there.
    inputs.currentMatch = RowBand { .top = 20, .bottom = 360 };
    CHECK(placeFindBar(inputs).anchor == FindBarAnchor::BottomRight);
}

TEST_CASE("findBar.aNarrowPaneShrinksTheBar", "[findbar]")
{
    auto inputs = Base;
    inputs.paneWidth = 200;
    CHECK(placeFindBar(inputs)
          == FindBarPlacement { .anchor = FindBarAnchor::TopRight, .x = 8, .y = 8, .width = 184 });
    inputs.paneWidth = 10;
    CHECK(placeFindBar(inputs).width == 0);
}

TEST_CASE("findBar.theTerminalRowUsesTheHeaderRow", "[findbar]")
{
    auto inputs = Base;
    inputs.host = FindBarHost::PaneHeaderRow;
    inputs.header = PaneHeaderPresence::Present;
    CHECK(placeFindBar(inputs)
          == FindBarPlacement { .anchor = FindBarAnchor::HeaderRow, .x = 0, .y = 0, .width = 800 });

    // A pane with no header (a single pane, Minimal) floats the bar at its top right (owner decision 2).
    inputs.header = PaneHeaderPresence::Absent;
    CHECK(placeFindBar(inputs).anchor == FindBarAnchor::TopRight);
}

TEST_CASE("findBar.insetsArePerAxis", "[findbar]")
{
    // F16: on the Terminal row one cell across is not one row down.
    auto inputs = Base;
    inputs.insetX = 7;
    inputs.insetY = 15;
    CHECK(placeFindBar(inputs)
          == FindBarPlacement { .anchor = FindBarAnchor::TopRight, .x = 413, .y = 15, .width = 380 });
}

TEST_CASE("findBar.rowBand", "[findbar]")
{
    CHECK(contour::window::rowBandOf(2, 3, 6, 20) == RowBand { .top = 46, .bottom = 86 });
}

TEST_CASE("findBar.names", "[findbar]")
{
    auto names = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, FindBarAnchorCount))
    {
        auto const name = contour::window::findBarAnchorName(static_cast<FindBarAnchor>(index));
        CHECK_FALSE(name.empty());
        names.insert(name);
    }
    CHECK(names.size() == FindBarAnchorCount);
}
