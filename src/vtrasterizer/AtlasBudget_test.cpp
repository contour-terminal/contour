// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the glyph atlas sizing policy (AtlasBudget.h).
//
// The budget is a correctness bound rather than a tuning knob: a tile's normalized location is baked into
// vertex data when the tile is created, while its pixels are uploaded at the end of the frame. An atlas too
// small for the page therefore recycles a tile slot *within* a frame, and every quad already recorded
// against that slot samples whichever glyph won the race. The bound this file pins is what keeps that from
// happening.

#include <vtrasterizer/AtlasBudget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>

using namespace vtrasterizer::atlasbudget;
using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::PageSize;

TEST_CASE("atlas budget: the page decides once it outgrows the configured floor", "[atlas][budget]")
{
    auto const page = PageSize { LineCount(24), ColumnCount(80) }; // 1920 cells

    // Configured well below the page: the page wins, with headroom for sixel tiles on top of the glyphs.
    CHECK(tileCountFor(crispy::LRUCapacity { 64 }, page).value == 1920 * PageHeadroomFactor);

    // Configured above the page requirement: the configuration wins and is never lowered.
    CHECK(tileCountFor(crispy::LRUCapacity { 100'000 }, page).value == 100'000);
}

TEST_CASE("atlas budget: a grown page requires more tiles than the page it grew from", "[atlas][budget]")
{
    // The regression this policy exists for. A freshly split pane starts small; the window is then
    // resized and the pane grows. Sizing the atlas once, at construction, left it holding a fraction of
    // what the grown page needs.
    auto const configured = crispy::LRUCapacity { 256 };
    auto const small = PageSize { LineCount(10), ColumnCount(20) };
    auto const grown = PageSize { LineCount(60), ColumnCount(200) };

    auto const smallBudget = tileCountFor(configured, small);
    auto const grownBudget = tileCountFor(configured, grown);

    auto const grownCells = static_cast<uint32_t>(grown.area());
    CHECK(grownBudget.value > smallBudget.value);
    CHECK(grownBudget.value >= grownCells);
}

TEST_CASE("atlas budget: a degenerate page falls back to the configured floor", "[atlas][budget]")
{
    // Before the first real geometry arrives a renderer can be sized for an empty page; the configured
    // floor is the only sensible answer, and it must not be multiplied down to zero.
    CHECK(tileCountFor(crispy::LRUCapacity { 256 }, PageSize { LineCount(0), ColumnCount(0) }).value == 256);
    CHECK(tileCountFor(crispy::LRUCapacity { 256 }, PageSize { LineCount(0), ColumnCount(80) }).value == 256);
}

TEST_CASE("atlas budget: the hashtable stays proportional to the tile count", "[atlas][budget]")
{
    // It used to stay pinned at the configured value while the tile count climbed with the page, which
    // turned a large page into one long hash chain per lookup.
    auto const tiles = crispy::LRUCapacity { 5760 };
    auto const slots = slotCountFor(crispy::StrongHashtableSize { 4096 }, tiles);

    CHECK(slots.value >= tiles.value);
    CHECK(std::has_single_bit(slots.value)); // the atlas requires a power of two
}

TEST_CASE("atlas budget: the configured slot count is a floor, and is rounded up", "[atlas][budget]")
{
    // A small page must not shrink the table below what was configured...
    auto const slots = slotCountFor(crispy::StrongHashtableSize { 4096 }, crispy::LRUCapacity { 10 });
    CHECK(slots.value == 4096);

    // ... and a non-power-of-two configuration is raised, never truncated.
    auto const rounded = slotCountFor(crispy::StrongHashtableSize { 3000 }, crispy::LRUCapacity { 10 });
    CHECK(rounded.value == 4096);
    CHECK(rounded.value >= 3000);
}
