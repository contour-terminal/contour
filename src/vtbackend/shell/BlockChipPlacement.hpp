// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a command block's status chip goes on its command row: only over blank cells right of the command,
/// clear of an RPROMPT, or nowhere -- collapsed to the rail's mark. Pure, so the table is testable without a
/// grid.
///
/// Column ranges here are ColumnRange's: INCLUSIVE at both ends.

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace vtbackend
{

/// The blank columns a chip keeps between itself and any text beside it.
inline constexpr auto BlockChipGap = ColumnCount(2);

/// What the placement reads about the command row.
struct BlockChipRowFacts
{
    ColumnCount pageColumns {};             ///< The page's width.
    ColumnOffset commandStart {};           ///< Where the command begins: the prompt's end.
    std::span<ColumnRange const> blankRuns; ///< The row's maximal blank runs, ascending (blankRunsOf()).
};

/// Whether the chip is drawn in the row, or its news left to the rail's mark.
enum class BlockChipPlacementKind : uint8_t
{
    CollapsedToRail = 0, ///< No blank run holds it: the rail's head mark is the only sign.
    InRow,               ///< Drawn over blank cells of the command row.
};

/// Where the chip goes.
struct BlockChipPlacement
{
    BlockChipPlacementKind kind = BlockChipPlacementKind::CollapsedToRail; ///< In the row, or not.
    ColumnRange columns {};                                                ///< Its columns, when InRow.

    bool operator==(BlockChipPlacement const&) const = default;
};

/// @p runs less the columns of @p taken: what is left blank once something else -- a fold chip -- is drawn
/// over part of them.
/// @param runs Blank runs, ascending.
/// @param taken The columns drawn over.
/// @return The runs that remain, ascending.
[[nodiscard]] inline std::vector<ColumnRange> withoutColumns(std::span<ColumnRange const> runs,
                                                             ColumnRange taken)
{
    auto remaining = std::vector<ColumnRange> {};
    for (auto const& run: runs)
    {
        if (run.toColumn < taken.fromColumn || taken.toColumn < run.fromColumn)
        {
            remaining.push_back(run);
            continue;
        }
        if (run.fromColumn < taken.fromColumn)
            remaining.push_back(ColumnRange { .line = run.line,
                                              .fromColumn = run.fromColumn,
                                              .toColumn = taken.fromColumn - ColumnOffset(1) });
        if (taken.toColumn < run.toColumn)
            remaining.push_back(ColumnRange {
                .line = run.line, .fromColumn = taken.toColumn + ColumnOffset(1), .toColumn = run.toColumn });
    }
    return remaining;
}

/// The blank columns between a command and the fold chip after it.
inline constexpr auto FoldChipLeadGap = ColumnCount(1);

/// Where a fold chip @p chipColumns wide goes on the row @p facts describe: the FIRST blank run starting at
/// or after the command that holds it FoldChipLeadGap after the command's text and BlockChipGap clear of
/// whatever follows -- an RPROMPT -- or anywhere up to the page's edge. The chip keeps to the command it
/// folds, and an RPROMPT keeps its cells, as placeBlockChip() keeps them for the status chip.
/// @param facts The row.
/// @param chipColumns The chip's width.
/// @return How many columns precede the chip's lead gap -- the run's start -- or nullopt where no run holds
///         it, and the chip follows the row's last text, clipped at the page's edge, as it always did.
[[nodiscard]] constexpr std::optional<ColumnCount> foldChipTextEnd(BlockChipRowFacts const& facts,
                                                                   ColumnCount chipColumns) noexcept
{
    auto const lastColumn = boxed_cast<ColumnOffset>(facts.pageColumns) - ColumnOffset(1);
    for (auto const& run: facts.blankRuns)
    {
        if (run.fromColumn < facts.commandStart)
            continue;
        auto const trailing = run.toColumn >= lastColumn ? ColumnCount(0) : BlockChipGap;
        if (run.length() >= FoldChipLeadGap + chipColumns + trailing)
            return boxed_cast<ColumnCount>(run.fromColumn);
    }
    return std::nullopt;
}

/// Where a chip @p chipColumns wide goes on the row @p facts describe (R-e).
///
/// The LAST blank run starting at or after the command that holds the chip with BlockChipGap of blanks on
/// either side -- StickyHeaderChipInset on the side where the run meets the page's edge -- right-aligned in
/// it. A run inside the prompt is never one: the chip says something about the command, so it goes right of
/// it, or nowhere.
/// @param facts The command row.
/// @param chipColumns The chip's width.
/// @return The placement.
[[nodiscard]] constexpr BlockChipPlacement placeBlockChip(BlockChipRowFacts const& facts,
                                                          ColumnCount chipColumns) noexcept
{
    auto const lastColumn = boxed_cast<ColumnOffset>(facts.pageColumns) - ColumnOffset(1);
    for (auto const& run: facts.blankRuns | std::views::reverse)
    {
        if (run.fromColumn < facts.commandStart)
            continue;
        auto const trailing = run.toColumn >= lastColumn ? StickyHeaderChipInset : BlockChipGap;
        if (run.length() < BlockChipGap + chipColumns + trailing)
            continue;
        auto const to = run.toColumn - boxed_cast<ColumnOffset>(trailing);
        return BlockChipPlacement { .kind = BlockChipPlacementKind::InRow,
                                    .columns =
                                        ColumnRange { .line = run.line,
                                                      .fromColumn = to - boxed_cast<ColumnOffset>(chipColumns)
                                                                    + ColumnOffset(1),
                                                      .toColumn = to } };
    }
    return {};
}

} // namespace vtbackend
