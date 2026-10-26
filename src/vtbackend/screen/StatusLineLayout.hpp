// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace vtbackend
{

/// How much a status-line item matters when the line is too narrow for all of it: the lowest goes first.
enum class StatusLinePriority : uint8_t
{
    Low = 0,   ///< A nicety: the clock, what is under the pointer.
    Normal,    ///< Context worth having: the branch, the title, a user's own text.
    High,      ///< What the user acts on: the directory, the last command, the selection.
    Essential, ///< What must not be missed: the input mode, a privilege boundary, a search.
};

/// The segment of the status line an item is in.
enum class StatusLineSide : uint8_t
{
    Left = 0,
    Middle,
    Right,
};

/// What collapsing needs to know of one item: where it is, how wide it draws, and how much it matters.
struct StatusLineItemExtent
{
    StatusLineSide side;         ///< Its segment.
    ColumnCount width;           ///< The columns it draws in, unstyled; zero for an item with nothing to say.
    StatusLinePriority priority; ///< How much it matters.
};

/// Whether an item is drawn or dropped to make the line fit.
enum class StatusLineItemFate : uint8_t
{
    Shown = 0,
    Dropped,
};

/// The width of each segment, in columns.
struct StatusLineSegmentWidths
{
    ColumnCount left {};   ///< The left segment's.
    ColumnCount middle {}; ///< The middle segment's.
    ColumnCount right {};  ///< The right segment's.
};

/// Where the middle and the right segment start; the left one always starts in the first column.
struct StatusLinePlacement
{
    std::optional<ColumnOffset> middleStart {}; ///< None while the middle segment is empty or not shown.
    std::optional<ColumnOffset> rightStart {};  ///< None while the right segment is empty or not shown.
};

/// Places the segments of a status line @p columns wide. The middle is centred, and not shown where it is too
/// wide or would overlap the left; the right ends in the last column, and is not shown where it is as wide as
/// the line or would overlap what the segments before it take.
/// @param widths Each segment's width.
/// @param columns The line's width.
/// @return Where the middle and the right segment start, if shown.
[[nodiscard]] constexpr StatusLinePlacement placeStatusLineSegments(StatusLineSegmentWidths widths,
                                                                    ColumnCount columns) noexcept
{
    auto placement = StatusLinePlacement {};

    // The columns, from the first, that the left segment and a middle one shown take.
    auto taken = widths.left;

    auto const middleStart = (columns - widths.middle) / ColumnCount(2);
    if (widths.middle > ColumnCount(0)
        && columns / ColumnCount(2) - ColumnCount(1) > widths.middle / ColumnCount(2) && taken <= middleStart)
    {
        placement.middleStart = ColumnOffset::cast_from(middleStart);
        taken = std::max(taken, middleStart + widths.middle);
    }

    if (widths.right > ColumnCount(0) && widths.right < columns && taken + widths.right <= columns)
        placement.rightStart = ColumnOffset::cast_from(columns - widths.right);

    return placement;
}

/// Which items of a status line @p columns wide to drop so that every segment is placed.
///
/// Drops whole items, never shortens one, while some non-empty segment would not be placed and more than one
/// item with a width remains -- so a single item wider than the line is shown, to be cut at the last column.
/// The lowest priority goes first; among equals, an item of a segment that would not be placed goes before
/// one of a segment that would, and then the right segment's last item before the middle's, before the
/// left's. An item of no width stays shown and never counts.
/// @param items Every item of the line, each with its segment, width and priority.
/// @param columns The line's width.
/// @return One fate per item, in the order of @p items.
[[nodiscard]] inline std::vector<StatusLineItemFate> collapseStatusLine(
    std::span<StatusLineItemExtent const> items, ColumnCount columns)
{
    auto fates = std::vector(items.size(), StatusLineItemFate::Shown);
    auto const indices = std::views::iota(std::size_t { 0 }, items.size());
    auto const counts = [&](std::size_t index) {
        return fates[index] == StatusLineItemFate::Shown && items[index].width > ColumnCount(0);
    };

    // Every round drops one item, so there are at most as many rounds as items.
    for ([[maybe_unused]] auto const round: indices)
    {
        auto widths = StatusLineSegmentWidths {};
        for (auto const index: indices | std::views::filter(counts))
        {
            switch (items[index].side)
            {
                case StatusLineSide::Left: widths.left += items[index].width; break;
                case StatusLineSide::Middle: widths.middle += items[index].width; break;
                case StatusLineSide::Right: widths.right += items[index].width; break;
            }
        }

        // A non-empty middle or right segment that is not shown, or a left one wider than the line.
        auto const placement = placeStatusLineSegments(widths, columns);
        auto const isUnplaced = [&](StatusLineSide side) {
            switch (side)
            {
                case StatusLineSide::Left: return widths.left > columns;
                case StatusLineSide::Middle: return widths.middle > ColumnCount(0) && !placement.middleStart;
                case StatusLineSide::Right: return widths.right > ColumnCount(0) && !placement.rightStart;
            }
            return false;
        };
        auto const sides = std::array { StatusLineSide::Left, StatusLineSide::Middle, StatusLineSide::Right };
        if (std::ranges::none_of(sides, isUnplaced) || std::ranges::count_if(indices, counts) <= 1)
            break;

        // Smallest first: the priority, then whether the item's segment is placed, then the side counted from
        // the right, then the position counted from the end.
        auto const order = [&](std::size_t index) {
            auto const& item = items[index];
            return std::tuple { item.priority,
                                !isUnplaced(item.side),
                                std::to_underlying(StatusLineSide::Right) - std::to_underlying(item.side),
                                items.size() - index };
        };
        auto shown = indices | std::views::filter(counts);
        fates[*std::ranges::min_element(shown, {}, order)] = StatusLineItemFate::Dropped;
    }
    return fates;
}

} // namespace vtbackend
