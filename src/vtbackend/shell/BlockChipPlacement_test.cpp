// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockChipPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;

namespace
{
constexpr auto Chip = ColumnCount(20);
constexpr auto Page = ColumnCount(80);
constexpr auto CommandStart = ColumnOffset(16);

/// A blank run from column @p from to column @p to, both included.
[[nodiscard]] constexpr ColumnRange run(int from, int to)
{
    return ColumnRange { .line = LineOffset(0),
                         .fromColumn = ColumnOffset(from),
                         .toColumn = ColumnOffset(to) };
}

/// Where a 20-column chip goes on a row of @p page columns whose command starts at column 16, among @p runs.
[[nodiscard]] BlockChipPlacement place(std::vector<ColumnRange> const& runs, ColumnCount page = Page)
{
    return placeBlockChip(
        BlockChipRowFacts { .pageColumns = page, .commandStart = CommandStart, .blankRuns = runs }, Chip);
}

/// A chip placed in columns @p from to @p to, both included.
[[nodiscard]] constexpr BlockChipPlacement inRow(int from, int to)
{
    return BlockChipPlacement { .kind = BlockChipPlacementKind::InRow, .columns = run(from, to) };
}
} // namespace

TEST_CASE("blockChip.withoutAnRprompt", "[blockchip]")
{
    // The blank tail of the row: right-aligned, StickyHeaderChipInset from the page's edge.
    CHECK(place({ run(39, 79) }) == inRow(58, 77));
}

TEST_CASE("blockChip.rpromptKeepsItsCells", "[blockchip]")
{
    // Review Focus 1: an RPROMPT at 70..79 keeps its cells and BlockChipGap of blanks before them.
    auto const placed = place({ run(39, 69) });
    CHECK(placed == inRow(48, 67));
    CHECK(placed.columns.toColumn + boxed_cast<ColumnOffset>(BlockChipGap) < ColumnOffset(70));
}

TEST_CASE("blockChip.p10kFullRowCollapses", "[blockchip]")
{
    // Review Focus 1: powerlevel10k fills the row, leaving single blanks between its segments.
    CHECK(place({ run(30, 30), run(45, 45) }).kind == BlockChipPlacementKind::CollapsedToRail);
}

TEST_CASE("blockChip.narrowPaneCollapses", "[blockchip]")
{
    CHECK(place({ run(20, 29) }, ColumnCount(30)).kind == BlockChipPlacementKind::CollapsedToRail);
}

TEST_CASE("blockChip.neverLeftOfTheCommand", "[blockchip]")
{
    // A wide blank run inside the prompt is no place for it: the chip goes right of the command or nowhere.
    CHECK(place({ run(2, 13) }).kind == BlockChipPlacementKind::CollapsedToRail);
    // Nor a run that starts in the prompt and reaches past the command's start: wide as it is, it begins
    // left of the command.
    CHECK(place({ run(10, 40) }).kind == BlockChipPlacementKind::CollapsedToRail);
}

TEST_CASE("blockChip.theLastWideRunWins", "[blockchip]")
{
    CHECK(place({ run(20, 49), run(52, 79) }) == inRow(58, 77));
}

TEST_CASE("blockChip.exactlyWideEnough", "[blockchip]")
{
    // A run of the chip plus a gap each side holds it; one column less does not.
    auto const fits = Chip + BlockChipGap + BlockChipGap;
    CHECK(place({ run(40, 40 + unbox<int>(fits) - 1) }).kind == BlockChipPlacementKind::InRow);
    CHECK(place({ run(40, 40 + unbox<int>(fits) - 2) }).kind == BlockChipPlacementKind::CollapsedToRail);
}

TEST_CASE("blockChip.aFoldChipTakesItsColumns", "[blockchip]")
{
    // A fold chip drawn at 10..25 leaves the blanks around it, and the status chip keeps its gap from it.
    auto const runs = withoutColumns(std::vector { run(9, 79) }, run(10, 25));
    CHECK(runs == std::vector { run(9, 9), run(26, 79) });
    CHECK(withoutColumns(std::vector { run(30, 40) }, run(10, 25)) == std::vector { run(30, 40) });
    CHECK(withoutColumns(std::vector { run(10, 25) }, run(10, 25)).empty());
    CHECK(place(runs) == inRow(58, 77));
}

TEST_CASE("blockChip.aFoldChipTakesTheRunBeforeAnRprompt", "[blockchip]")
{
    // m6: the fold chip follows the command into the blanks before an RPROMPT, as the status chip keeps
    // clear of it -- not after the RPROMPT, where the row's last text ends.
    auto const fold = [](std::vector<ColumnRange> const& runs) {
        return foldChipTextEnd(
            BlockChipRowFacts { .pageColumns = Page, .commandStart = ColumnOffset(16), .blankRuns = runs },
            Chip);
    };

    // A command ending at 24, an RPROMPT at 70..79: the run 24..69 holds 1 + 20 + 2.
    CHECK(fold({ run(19, 19), run(24, 69) }) == ColumnCount(24));
    // Exactly 1 + 20 + 2 columns hold it; one fewer does not.
    CHECK(fold({ run(19, 19), run(24, 46) }) == ColumnCount(24));
    CHECK_FALSE(fold({ run(19, 19), run(24, 45) }).has_value());
    // No run holds it: the chip follows the row's last text, as it always did.
    CHECK_FALSE(fold({ run(19, 19), run(24, 40) }).has_value());
    // A run reaching the page's edge needs no gap after it.
    CHECK(fold({ run(59, 79) }) == ColumnCount(59));
    // Never inside the prompt.
    CHECK_FALSE(fold({ run(0, 15) }).has_value());
}
