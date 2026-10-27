// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LinkPillPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>
#include <vector>

using contour::window::FindBarPresence;
using contour::window::LinkPillAnchor;
using contour::window::LinkPillHost;
using contour::window::LinkPillInputs;
using contour::window::LinkPillPlacement;
using contour::window::placeLinkPill;

namespace
{

constexpr auto Columns = 100;
constexpr auto Rows = 20;

/// The base case: a pane of 800 × 400 with cells of 8 × 20 and no margins, 100 × 20 cells, every one blank, a
/// pill of 300 × 22 inset 8 from the top and 10 from the right.
struct Base
{
    std::vector<int> trailingBlank = std::vector<int>(Rows, Columns);
    std::vector<int> avoidRows {};

    [[nodiscard]] LinkPillInputs inputs() const
    {
        return LinkPillInputs { .host = LinkPillHost::Grid,
                                .findBar = FindBarPresence::Closed,
                                .paneWidth = 800,
                                .paneHeight = 400,
                                .pillWidth = 300,
                                .pillMinimumWidth = 120,
                                .pillHeight = 22,
                                .headerSlotWidth = 400,
                                .insetTop = 8,
                                .insetRight = 10,
                                .grid = { .marginLeft = 0,
                                          .marginTop = 0,
                                          .cellWidth = 8,
                                          .cellHeight = 20,
                                          .columns = Columns,
                                          .trailingBlankColumns = trailingBlank,
                                          .avoidRows = avoidRows } };
    }
};

constexpr auto TopRight =
    LinkPillPlacement { .anchor = LinkPillAnchor::TopRight, .x = 490, .y = 8, .width = 300 };
constexpr auto BottomRight =
    LinkPillPlacement { .anchor = LinkPillAnchor::BottomRight, .x = 490, .y = 370, .width = 300 };

} // namespace

TEST_CASE("linkPill.topRightOverBlankCells", "[linkpill]")
{
    CHECK(placeLinkPill(Base {}.inputs()) == TopRight);
}

TEST_CASE("linkPill.movesDownWhenTheTopIsNotBlank", "[linkpill]")
{
    // The pill covers columns 61..99 of rows 0 and 1: 39 cells at the right end.
    auto base = Base {};
    base.trailingBlank[1] = 38;
    CHECK(placeLinkPill(base.inputs()) == BottomRight);
    base.trailingBlank[1] = 39;
    CHECK(placeLinkPill(base.inputs()) == TopRight);
}

TEST_CASE("linkPill.neverOnThePromptRow", "[linkpill]")
{
    auto base = Base {};
    base.avoidRows = { 0 };
    CHECK(placeLinkPill(base.inputs()) == BottomRight);
    base.avoidRows = { 0, Rows - 1 };
    CHECK(placeLinkPill(base.inputs()) == LinkPillPlacement {});
}

TEST_CASE("linkPill.noBlankCellsNoPill", "[linkpill]")
{
    auto base = Base {};
    base.trailingBlank.assign(Rows, 0);
    CHECK(placeLinkPill(base.inputs()).anchor == LinkPillAnchor::Hidden);
}

TEST_CASE("linkPill.neverOnTheStatusLine", "[linkpill]")
{
    // The status line's row is never blank (vtbackend::OverlayClearance), so a bottom status line refuses the
    // bottom right.
    auto base = Base {};
    base.trailingBlank.back() = 0;
    base.trailingBlank[0] = 0;
    CHECK(placeLinkPill(base.inputs()).anchor == LinkPillAnchor::Hidden);
}

TEST_CASE("linkPill.inThePaneHeader", "[linkpill]")
{
    auto base = Base {};
    base.trailingBlank.assign(Rows, 0);
    auto inputs = base.inputs();
    inputs.host = LinkPillHost::PaneHeader;
    // Flush with the slot's trailing edge.
    CHECK(placeLinkPill(inputs)
          == LinkPillPlacement { .anchor = LinkPillAnchor::HeaderTrailing, .x = 100, .y = 0, .width = 300 });
}

TEST_CASE("linkPill.aNarrowHeaderSlotCutsThePillOrSendsItToTheGrid", "[linkpill]")
{
    // A slot narrower than the pill cuts it to the slot, flush with its trailing edge; one narrower than the
    // pill's minimum keeps it in the grid rather than squeezed over the header's actions.
    auto const base = Base {}; // owns the rows the inputs' spans view
    auto inputs = base.inputs();
    inputs.host = LinkPillHost::PaneHeader;
    inputs.headerSlotWidth = 200;
    CHECK(placeLinkPill(inputs)
          == LinkPillPlacement { .anchor = LinkPillAnchor::HeaderTrailing, .x = 0, .y = 0, .width = 200 });
    inputs.headerSlotWidth = 120;
    CHECK(placeLinkPill(inputs).anchor == LinkPillAnchor::HeaderTrailing);
    inputs.headerSlotWidth = 119;
    CHECK(placeLinkPill(inputs) == TopRight);
}

TEST_CASE("linkPill.theFindBarOwnsTheHeaderAndTheTopRight", "[linkpill]")
{
    // While the find bar is at the top it holds the header's trailing area and the grid's top right: the pill
    // falls back to the bottom right, or nowhere.
    auto base = Base {};
    auto inputs = base.inputs();
    inputs.findBar = FindBarPresence::Top;
    CHECK(placeLinkPill(inputs) == BottomRight);
    inputs.host = LinkPillHost::PaneHeader;
    CHECK(placeLinkPill(inputs) == BottomRight);

    base.trailingBlank.back() = 0;
    inputs = base.inputs();
    inputs.findBar = FindBarPresence::Top;
    CHECK(placeLinkPill(inputs) == LinkPillPlacement {});
}

TEST_CASE("linkPill.aFindBarAtTheBottomLeavesTheTop", "[linkpill]")
{
    // Moved out of the current match's way, the bar holds the bottom right: the pill keeps the header and the
    // top right, and where the top is not blank it shows nowhere rather than over the bar.
    auto base = Base {};
    auto inputs = base.inputs();
    inputs.findBar = FindBarPresence::Bottom;
    CHECK(placeLinkPill(inputs) == TopRight);
    inputs.host = LinkPillHost::PaneHeader;
    CHECK(placeLinkPill(inputs).anchor == LinkPillAnchor::HeaderTrailing);

    base.trailingBlank.front() = 0;
    inputs = base.inputs();
    inputs.findBar = FindBarPresence::Bottom;
    CHECK(placeLinkPill(inputs) == LinkPillPlacement {});
}

TEST_CASE("linkPill.everyFindBarAnchorHoldsAPlace", "[linkpill]")
{
    using contour::window::FindBarAnchor;
    using contour::window::findBarPresenceAt;
    CHECK(findBarPresenceAt(FindBarAnchor::TopRight) == FindBarPresence::Top);
    CHECK(findBarPresenceAt(FindBarAnchor::HeaderRow) == FindBarPresence::Top);
    CHECK(findBarPresenceAt(FindBarAnchor::BottomRight) == FindBarPresence::Bottom);
    // A shown bar always holds a place: no anchor reads as a closed one.
    CHECK(std::ranges::none_of(contour::window::FindBarPresenceAt,
                               [](FindBarPresence presence) { return presence == FindBarPresence::Closed; }));
}

TEST_CASE("linkPill.aNarrowPaneShrinksThePill", "[linkpill]")
{
    auto base = Base {};
    auto inputs = base.inputs();
    inputs.paneWidth = 200;
    CHECK(placeLinkPill(inputs)
          == LinkPillPlacement { .anchor = LinkPillAnchor::TopRight, .x = 10, .y = 8, .width = 180 });
    inputs.paneWidth = 10;
    CHECK(placeLinkPill(inputs) == LinkPillPlacement {});
}

TEST_CASE("linkPill.marginsAreClear", "[linkpill]")
{
    // With a top margin of 30 the pill sits in the margin, over no row of the grid.
    auto base = Base {};
    base.trailingBlank[0] = 0;
    auto inputs = base.inputs();
    inputs.grid.marginTop = 30;
    CHECK(placeLinkPill(inputs).anchor == LinkPillAnchor::TopRight);
}

TEST_CASE("linkPill.anEmptyPillShowsNothing", "[linkpill]")
{
    auto inputs = Base {}.inputs();
    inputs.pillWidth = 0;
    CHECK(placeLinkPill(inputs) == LinkPillPlacement {});
    inputs = Base {}.inputs();
    inputs.pillHeight = 0;
    CHECK(placeLinkPill(inputs) == LinkPillPlacement {});
}

TEST_CASE("linkPill.names", "[linkpill]")
{
    auto names = std::set<std::string_view> {};
    for (auto const anchor: std::views::iota(size_t { 0 }, contour::window::LinkPillAnchorCount))
    {
        auto const name = contour::window::linkPillAnchorName(static_cast<LinkPillAnchor>(anchor));
        CHECK_FALSE(name.empty());
        names.insert(name);
    }
    CHECK(names.size() == contour::window::LinkPillAnchorCount);
    CHECK(contour::window::linkPillAnchorName(LinkPillAnchor::TopRight) == "topRight");
}

TEST_CASE("linkPill.aBandEndingOnARowEdgeLeavesTheNextRowBe", "[linkpill]")
{
    // A pill one row high on that row's top edge covers that row alone: its bottom is the next row's top, and
    // the cell there is not under it.
    auto base = Base {};
    base.trailingBlank[2] = 0;
    auto inputs = base.inputs();
    inputs.insetTop = 20;
    inputs.pillHeight = 20;
    CHECK(placeLinkPill(inputs).anchor == LinkPillAnchor::TopRight);

    // A hair lower, and it reaches into the next row.
    inputs.insetTop = 21;
    CHECK(placeLinkPill(inputs).anchor != LinkPillAnchor::TopRight);
}
