// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ScrollableSelection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using namespace core::tui;

TEST_CASE("ScrollableSelection.initial_state")
{
    ScrollableSelection const sel(10);
    CHECK(sel.selected() == 0);
    CHECK(sel.scrollOffset() == 0);
    CHECK(sel.maxVisible() == 10);
}

TEST_CASE("ScrollableSelection.selectNext_wraps_around")
{
    ScrollableSelection sel(10);
    sel.selectNext(3); // 0 -> 1
    CHECK(sel.selected() == 1);
    sel.selectNext(3); // 1 -> 2
    CHECK(sel.selected() == 2);
    sel.selectNext(3); // 2 -> 0 (wrap)
    CHECK(sel.selected() == 0);
}

TEST_CASE("ScrollableSelection.selectPrev_wraps_around")
{
    ScrollableSelection sel(10);
    CHECK(sel.selectPrev(3)); // 0 -> 2 (wrap)
    CHECK(sel.selected() == 2);
    CHECK(sel.selectPrev(3)); // 2 -> 1
    CHECK(sel.selected() == 1);
}

TEST_CASE("ScrollableSelection.selectNext_returns_false_on_empty")
{
    ScrollableSelection sel(10);
    CHECK_FALSE(sel.selectNext(0));
    CHECK(sel.selected() == 0);
}

TEST_CASE("ScrollableSelection.selectPrev_returns_false_on_empty")
{
    ScrollableSelection sel(10);
    CHECK_FALSE(sel.selectPrev(0));
}

TEST_CASE("ScrollableSelection.pageDown_clamps_to_last")
{
    ScrollableSelection sel(5);
    sel.selectNext(20);      // at 1
    CHECK(sel.pageDown(20)); // 1 -> 6
    CHECK(sel.selected() == 6);
    // Jump near end
    for ([[maybe_unused]] auto const i: std::views::iota(0, 3))
        sel.pageDown(20);
    CHECK(sel.selected() <= 19);
    // Page down past end clamps
    sel.selectLast(20);
    CHECK_FALSE(sel.pageDown(20)); // already at 19
    CHECK(sel.selected() == 19);
}

TEST_CASE("ScrollableSelection.pageUp_clamps_to_first")
{
    ScrollableSelection sel(5);
    CHECK_FALSE(sel.pageUp(20)); // already at 0
    CHECK(sel.selected() == 0);

    // Move to middle, then page up
    sel.selectLast(20);    // at 19
    CHECK(sel.pageUp(20)); // 19 -> 14
    CHECK(sel.selected() == 14);
}

TEST_CASE("ScrollableSelection.selectFirst_selectLast")
{
    ScrollableSelection sel(5);
    sel.selectLast(10);
    CHECK(sel.selected() == 9);
    sel.selectFirst(10);
    CHECK(sel.selected() == 0);
}

TEST_CASE("ScrollableSelection.selectFirst_returns_false_when_already_first")
{
    ScrollableSelection sel(5);
    CHECK_FALSE(sel.selectFirst(10)); // already at 0
}

TEST_CASE("ScrollableSelection.selectLast_returns_false_when_already_last")
{
    ScrollableSelection sel(5);
    sel.selectLast(10);
    CHECK_FALSE(sel.selectLast(10)); // already at 9
}

TEST_CASE("ScrollableSelection.ensureSelectedVisible_scrolls_down")
{
    ScrollableSelection sel(3);
    // Select item 5 (beyond visible window 0-2)
    for ([[maybe_unused]] auto const i: std::views::iota(0, 5))
        sel.selectNext(10);
    CHECK(sel.selected() == 5);
    // Scroll should have adjusted so item 5 is visible
    CHECK(sel.scrollOffset() == 3); // window shows items 3,4,5
}

TEST_CASE("ScrollableSelection.ensureSelectedVisible_scrolls_up")
{
    ScrollableSelection sel(3);
    // Go to end then wrap to beginning
    sel.selectLast(10); // at 9, scrollOffset adjusted
    sel.selectNext(10); // wraps to 0
    CHECK(sel.selected() == 0);
    CHECK(sel.scrollOffset() == 0);
}

TEST_CASE("ScrollableSelection.reset")
{
    ScrollableSelection sel(5);
    sel.selectLast(10);
    CHECK(sel.selected() == 9);
    sel.reset();
    CHECK(sel.selected() == 0);
    CHECK(sel.scrollOffset() == 0);
}

TEST_CASE("ScrollableSelection.single_item")
{
    ScrollableSelection sel(10);
    CHECK_FALSE(sel.selectNext(1)); // wraps to same position
    CHECK(sel.selected() == 0);
    CHECK_FALSE(sel.selectPrev(1));
    CHECK(sel.selected() == 0);
}

TEST_CASE("ScrollableSelection.setMaxVisible")
{
    ScrollableSelection sel(5);
    sel.setMaxVisible(3);
    CHECK(sel.maxVisible() == 3);
}
