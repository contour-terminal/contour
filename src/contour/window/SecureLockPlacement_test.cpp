// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SecureLockPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <vector>

using contour::window::FindBarPresence;
using contour::window::FindBarPresenceCount;
using contour::window::LinkPillAnchor;
using contour::window::LinkPillAnchorCount;
using contour::window::placeSecureLock;
using contour::window::SecureLockAnchor;
using contour::window::SecureLockInputs;
using contour::window::SecureLockPlacement;

namespace
{

constexpr auto Columns = 100;
constexpr auto Rows = 20;

/// A pane of 800 × 400 with cells of 8 × 20 and no margins, 100 × 20 cells, every one blank, and a lock pill
/// of 90 × 22 inset 8 from the top and 10 from the right.
struct Base
{
    std::vector<int> trailingBlank = std::vector<int>(Rows, Columns);
    std::vector<int> avoidRows {};
    FindBarPresence findBar = FindBarPresence::Closed;
    LinkPillAnchor linkPill = LinkPillAnchor::Hidden;

    [[nodiscard]] SecureLockInputs inputs() const
    {
        return SecureLockInputs { .findBar = findBar,
                                  .linkPill = linkPill,
                                  .paneWidth = 800,
                                  .paneHeight = 400,
                                  .lockWidth = 90,
                                  .lockHeight = 22,
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

constexpr auto TopRight = SecureLockPlacement { .anchor = SecureLockAnchor::TopRight, .x = 700, .y = 8 };
constexpr auto BottomRight =
    SecureLockPlacement { .anchor = SecureLockAnchor::BottomRight, .x = 700, .y = 370 };
constexpr auto TopRightOver =
    SecureLockPlacement { .anchor = SecureLockAnchor::TopRightOver, .x = 700, .y = 8 };

} // namespace

TEST_CASE("secureLock.topRightOverBlankCells", "[securelock]")
{
    CHECK(placeSecureLock(Base {}.inputs()) == TopRight);
}

TEST_CASE("secureLock.movesDownWhenTheTopIsNotBlank", "[securelock]")
{
    // 90 wide from x 700: columns 87..99 of rows 0 and 1, 13 cells at the right end.
    auto base = Base {};
    base.trailingBlank[1] = 12;
    CHECK(placeSecureLock(base.inputs()) == BottomRight);
    base.trailingBlank[1] = 13;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
}

TEST_CASE("secureLock.neverOnThePromptRow", "[securelock]")
{
    auto base = Base {};
    base.avoidRows = { 0 };
    CHECK(placeSecureLock(base.inputs()) == BottomRight);
    // Both blank places taken by the prompt: the last resort covers the top right rather than vanish.
    base.avoidRows = { 0, Rows - 1 };
    CHECK(placeSecureLock(base.inputs()) == TopRightOver);
}

TEST_CASE("secureLock.coversCellsAsALastResortWhenNoneAreBlank", "[securelock]")
{
    // A full-screen program: nothing is blank, and the lock still shows (review I1).
    auto base = Base {};
    base.trailingBlank.assign(Rows, 0);
    CHECK(placeSecureLock(base.inputs()) == TopRightOver);
}

TEST_CASE("secureLock.yieldsToTheFindBar", "[securelock]")
{
    auto base = Base {};
    base.findBar = FindBarPresence::Top;
    CHECK(placeSecureLock(base.inputs()) == BottomRight);
    base.findBar = FindBarPresence::Bottom;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
    base.findBar = FindBarPresence::Closed;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
}

TEST_CASE("secureLock.yieldsToTheLinkPill", "[securelock]")
{
    auto base = Base {};
    base.linkPill = LinkPillAnchor::TopRight;
    CHECK(placeSecureLock(base.inputs()) == BottomRight);
    base.linkPill = LinkPillAnchor::BottomRight;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
    // A link pill in a header, or none, holds no cell of the grid.
    base.linkPill = LinkPillAnchor::HeaderTrailing;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
    base.linkPill = LinkPillAnchor::Hidden;
    CHECK(placeSecureLock(base.inputs()) == TopRight);
}

TEST_CASE("secureLock.coversAPillWhenTheOtherTwoHoldBothPlaces", "[securelock]")
{
    auto base = Base {};
    base.findBar = FindBarPresence::Top;
    base.linkPill = LinkPillAnchor::BottomRight;
    CHECK(placeSecureLock(base.inputs()) == TopRightOver);
}

TEST_CASE("secureLock.isNeverInvisibleAndCoversOnlyWithNoBlankPlace", "[securelock]")
{
    // Every occupant pair, with blank cells and with none: the lock shows, and sits over something only when
    // neither blank place is free.
    for (auto const find: std::views::iota(size_t { 0 }, FindBarPresenceCount))
        for (auto const link: std::views::iota(size_t { 0 }, LinkPillAnchorCount))
            for (auto const blank: { true, false })
            {
                auto base = Base {};
                base.findBar = static_cast<FindBarPresence>(find);
                base.linkPill = static_cast<LinkPillAnchor>(link);
                base.trailingBlank.assign(Rows, blank ? Columns : 0);
                auto const placed = placeSecureLock(base.inputs());
                INFO("find bar " << find << ", link pill " << link << ", blank " << blank);
                CHECK(placed.anchor != SecureLockAnchor::Hidden);
                auto const topFree =
                    base.findBar != FindBarPresence::Top && base.linkPill != LinkPillAnchor::TopRight;
                auto const bottomFree =
                    base.findBar != FindBarPresence::Bottom && base.linkPill != LinkPillAnchor::BottomRight;
                CHECK((placed.anchor == SecureLockAnchor::TopRightOver)
                      == (!blank || (!topFree && !bottomFree)));
            }
}

TEST_CASE("secureLock.neverOverlapsAnythingWhileABlankPlaceIsFree", "[securelock]")
{
    for (auto const find: std::views::iota(size_t { 0 }, FindBarPresenceCount))
        for (auto const link: std::views::iota(size_t { 0 }, LinkPillAnchorCount))
        {
            auto base = Base {};
            base.findBar = static_cast<FindBarPresence>(find);
            base.linkPill = static_cast<LinkPillAnchor>(link);
            auto const placed = placeSecureLock(base.inputs());
            INFO("find bar " << find << ", link pill " << link);
            CHECK_FALSE(
                (placed.anchor == SecureLockAnchor::TopRight
                 && (base.findBar == FindBarPresence::Top || base.linkPill == LinkPillAnchor::TopRight)));
            CHECK_FALSE((placed.anchor == SecureLockAnchor::BottomRight
                         && (base.findBar == FindBarPresence::Bottom
                             || base.linkPill == LinkPillAnchor::BottomRight)));
        }
}

TEST_CASE("secureLock.hidesWhereThePaneIsTooNarrow", "[securelock]")
{
    auto inputs = Base {}.inputs();
    inputs.paneWidth = 60; // narrower than the lock plus its insets
    CHECK(placeSecureLock(inputs).anchor == SecureLockAnchor::Hidden);
}

TEST_CASE("secureLock.anchorNamesAreTheQmlOnes", "[securelock]")
{
    CHECK(contour::window::secureLockAnchorName(SecureLockAnchor::Hidden) == "hidden");
    CHECK(contour::window::secureLockAnchorName(SecureLockAnchor::TopRight) == "topRight");
    CHECK(contour::window::secureLockAnchorName(SecureLockAnchor::BottomRight) == "bottomRight");
    CHECK(contour::window::secureLockAnchorName(SecureLockAnchor::TopRightOver) == "topRightOver");
}
