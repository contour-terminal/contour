// SPDX-License-Identifier: Apache-2.0
//
// Where an input method's preedit is drawn on the cursor's line: from the cursor when it fits, moved left
// until its end meets the right edge when it does not, and cut only when it is wider than the whole line.

#include <vtbackend/screen/PreeditPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::placePreedit;

namespace
{

/// The clusters of @p placement, as text.
[[nodiscard]] std::vector<std::u32string> textsOf(vtbackend::PreeditPlacement const& placement)
{
    // A loop, not `| std::ranges::to<std::vector>()`: Clang 22 cannot instantiate libstdc++ 14's
    // range-adaptor pipe for it (forward_like), which is what CI's Ubuntu runners pair.
    auto texts = std::vector<std::u32string> {};
    texts.reserve(placement.clusters.size());
    for (auto const& cluster: placement.clusters)
        texts.push_back(cluster.text);
    return texts;
}

constexpr auto Line = ColumnCount(10);

} // namespace

TEST_CASE("placePreedit.aPreeditThatFitsStartsAtTheCursor", "[renderbuffer][ime]")
{
    auto const placement = placePreedit("にほ", ColumnOffset(2), Line);
    CHECK(placement.start == ColumnOffset(2));
    CHECK(placement.columns == ColumnCount(4));
    CHECK(textsOf(placement) == std::vector<std::u32string> { U"に", U"ほ" });
}

TEST_CASE("placePreedit.aWideFirstClusterInTheLastColumnMovesLeft", "[renderbuffer][ime]")
{
    // Typed after a full line: the cursor is on the last cell, where に does not fit. It moves left until its
    // end meets the edge, so the composition is seen at all.
    auto const placement = placePreedit("に", ColumnOffset(9), Line);
    CHECK(placement.start == ColumnOffset(8));
    CHECK(textsOf(placement) == std::vector<std::u32string> { U"に" });
}

TEST_CASE("placePreedit.aPreeditRunningPastTheEdgeMovesLeftWhole", "[renderbuffer][ime]")
{
    auto const placement = placePreedit("にほんご", ColumnOffset(6), Line);
    CHECK(placement.start == ColumnOffset(2));
    CHECK(placement.columns == ColumnCount(8));
    CHECK(placement.clusters.size() == 4);
}

TEST_CASE("placePreedit.onlyAPreeditWiderThanTheLineIsCut", "[renderbuffer][ime]")
{
    SECTION("whole clusters up to the edge")
    {
        auto const placement = placePreedit("にほんごにほん", ColumnOffset(4), Line); // 14 columns
        CHECK(placement.start == ColumnOffset(0));
        CHECK(placement.columns == ColumnCount(10));
        CHECK(textsOf(placement) == std::vector<std::u32string> { U"に", U"ほ", U"ん", U"ご", U"に" });
    }

    SECTION("a wide cluster that would straddle the edge is left out, with what follows")
    {
        auto const placement = placePreedit("abcdefghiにj", ColumnOffset(0), Line); // 12 columns
        CHECK(placement.start == ColumnOffset(0));
        CHECK(placement.columns == ColumnCount(9));
        CHECK(placement.clusters.back().text == U"i");
    }
}

TEST_CASE("placePreedit.aClusterOfNoWidthStillTakesAColumn", "[renderbuffer][ime]")
{
    // U+200B is a cluster of its own and has no width; drawn without a column it would leave the preedit's
    // cells and the columns they cover out of step.
    auto const placement = placePreedit("​ab", ColumnOffset(2), Line);
    REQUIRE(placement.clusters.size() == 3);
    CHECK(std::ranges::all_of(placement.clusters,
                              [](auto const& cluster) { return cluster.width >= ColumnCount(1); }));
    CHECK(placement.columns == ColumnCount(3));
}

TEST_CASE("placePreedit.anEmptyPreeditIsDrawnNowhere", "[renderbuffer][ime]")
{
    auto const placement = placePreedit("", ColumnOffset(9), Line);
    CHECK(placement.start == ColumnOffset(9));
    CHECK(placement.clusters.empty());
}

TEST_CASE("placePreedit.aMovedPreeditDoesNotStartOnAWideGlyphsRightHalf", "[renderbuffer][ime]")
{
    // A wide glyph covers columns 3 and 4. Moved left to column 4, the preedit would begin on its right half,
    // and the glyph would draw over the preedit's first column; it starts one further left instead.
    auto const continuesAWideGlyph = [](ColumnOffset column) {
        return column == ColumnOffset(4);
    };
    auto const placement = placePreedit("にほん", ColumnOffset(9), Line, continuesAWideGlyph);
    CHECK(placement.start == ColumnOffset(3));
    CHECK(placement.columns == ColumnCount(6));

    SECTION("at the cursor it never steps, as the cursor never stands on a continuation")
    {
        CHECK(placePreedit("に", ColumnOffset(4), Line, continuesAWideGlyph).start == ColumnOffset(4));
    }
}
