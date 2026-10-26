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

/// Whether an item says something of its own, or only separates or frames the items beside it.
enum class StatusLineItemKind : uint8_t
{
    Item = 0,  ///< A placeholder, or text the template styles: dropped by priority.
    Separator, ///< A bare literal, such as " │ " or "« ": never dropped on its own account, only with its
               ///< item.
};

/// What collapsing needs to know of one item: where it is, how wide it draws, and how much it matters.
struct StatusLineItemExtent
{
    StatusLineSide side;         ///< Its segment.
    ColumnCount width;           ///< The columns it draws in, unstyled; zero for an item with nothing to say.
    StatusLinePriority priority; ///< How much it matters.
    StatusLineItemKind kind = StatusLineItemKind::Item; ///< Whether it is an item or a separator.
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

namespace detail
{
    /// Drops each separator whose item went: one between two items with either of them, a leading one with
    /// the item after it, a trailing one with the item before it. Each collapse round measures the line with
    /// this pass applied, so it sees the line as it would be drawn.
    inline void dropOrphanedSeparators(std::span<StatusLineItemExtent const> items,
                                       std::span<StatusLineItemFate> fates)
    {
        auto const isDropped = [&](std::optional<std::size_t> index) {
            return index && fates[*index] == StatusLineItemFate::Dropped;
        };
        for (auto const side: { StatusLineSide::Left, StatusLineSide::Middle, StatusLineSide::Right })
        {
            auto run = std::vector<std::size_t> {}; // the separators since the item before
            auto itemBefore = std::optional<std::size_t> {};
            auto const dropRunIf = [&](bool drop) {
                if (drop)
                    for (auto const index: run)
                        fates[index] = StatusLineItemFate::Dropped;
                run.clear();
            };
            for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
            {
                if (items[index].side != side)
                    continue;
                if (items[index].kind == StatusLineItemKind::Separator)
                {
                    run.push_back(index);
                    continue;
                }
                dropRunIf(isDropped(itemBefore) || isDropped(index));
                itemBefore = index;
            }
            dropRunIf(isDropped(itemBefore));
        }
    }

    /// Sums the widths of the shown items on each side: the measure collapseStatusLine() uses when its caller
    /// gives none.
    struct SumOfItemWidths
    {
        std::span<StatusLineItemExtent const> items; ///< The items measured.

        /// @return The columns the items on @p side that @p fates show take.
        [[nodiscard]] ColumnCount operator()(StatusLineSide side,
                                             std::span<StatusLineItemFate const> fates) const
        {
            auto width = ColumnCount(0);
            for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
                if (items[index].side == side && fates[index] == StatusLineItemFate::Shown)
                    width += items[index].width;
            return width;
        }
    };
} // namespace detail

/// Which items of a status line @p columns wide to drop so that every segment is placed.
///
/// Drops whole items, never shortens one, while some non-empty segment would not be placed and more than one
/// item with a width remains -- so a single item wider than the line is shown, to be cut at the last column.
/// Only an item that can make an unplaced segment placed is a candidate: one of that segment, or one of a
/// segment before it (a placed middle helps the right) unless the segment is too wide on its own -- a middle
/// that cannot be centred, a right as wide as the line. The right segment's items never help the middle. The
/// lowest priority goes first; among equals, an item of a segment that would not be placed goes before one of
/// a segment that would, and then the right segment's last item before the middle's, before the left's. An
/// item of no width, and a separator, is never a candidate; a separator goes afterwards with its item.
/// @param items Every item of the line, each with its segment, width, priority and kind, segment by segment.
/// @param columns The line's width.
/// @param measureSegment Gives the width of the items on a side that the fates show, as the line will draw
///                       them; summing the items' own widths unless told otherwise.
/// @return One fate per item, in the order of @p items.
template <typename MeasureSegment>
[[nodiscard]] std::vector<StatusLineItemFate> collapseStatusLine(std::span<StatusLineItemExtent const> items,
                                                                 ColumnCount columns,
                                                                 MeasureSegment const& measureSegment)
{
    auto fates = std::vector(items.size(), StatusLineItemFate::Shown);
    auto const indices = std::views::iota(std::size_t { 0 }, items.size());
    auto const counts = [&](std::size_t index) {
        return fates[index] == StatusLineItemFate::Shown && items[index].width > ColumnCount(0);
    };
    auto const isItem = [&](std::size_t index) {
        return items[index].kind == StatusLineItemKind::Item;
    };

    // Every round drops one item, so there are at most as many rounds as items.
    for ([[maybe_unused]] auto const round: indices)
    {
        // Measured as it would be drawn: without the separators whose items are gone, which would otherwise
        // make the line look wider than it is and cost an item that fits.
        auto drawn = fates;
        detail::dropOrphanedSeparators(items, drawn);
        auto const widths = StatusLineSegmentWidths { .left = measureSegment(StatusLineSide::Left, drawn),
                                                      .middle = measureSegment(StatusLineSide::Middle, drawn),
                                                      .right = measureSegment(StatusLineSide::Right, drawn) };
        auto const placement = placeStatusLineSegments(widths, columns);

        // A non-empty middle or right segment that is not shown, or a left one wider than the line.
        auto const isUnplaced = [&](StatusLineSide side) {
            switch (side)
            {
                case StatusLineSide::Left: return widths.left > columns;
                case StatusLineSide::Middle: return widths.middle > ColumnCount(0) && !placement.middleStart;
                case StatusLineSide::Right: return widths.right > ColumnCount(0) && !placement.rightStart;
            }
            return false;
        };

        // Whether dropping an item on @p side can make the unplaced segment @p unplaced placed.
        auto const canHelp = [&](StatusLineSide side, StatusLineSide unplaced) {
            if (side == unplaced)
                return true;
            switch (unplaced)
            {
                case StatusLineSide::Left: return false;
                case StatusLineSide::Middle:
                    return side == StatusLineSide::Left
                           && columns / ColumnCount(2) - ColumnCount(1) > widths.middle / ColumnCount(2);
                case StatusLineSide::Right:
                    return widths.right < columns
                           && (side == StatusLineSide::Left
                               || (side == StatusLineSide::Middle && placement.middleStart.has_value()));
            }
            return false;
        };
        auto const sides = std::array { StatusLineSide::Left, StatusLineSide::Middle, StatusLineSide::Right };
        auto const isCandidate = [&](std::size_t index) {
            return counts(index) && isItem(index) && std::ranges::any_of(sides, [&](StatusLineSide unplaced) {
                       return isUnplaced(unplaced) && canHelp(items[index].side, unplaced);
                   });
        };

        auto candidates = indices | std::views::filter(isCandidate);
        if (std::ranges::empty(candidates) || std::ranges::count_if(indices, [&](std::size_t index) {
                                                  return counts(index) && isItem(index);
                                              }) <= 1)
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
        fates[*std::ranges::min_element(candidates, {}, order)] = StatusLineItemFate::Dropped;
    }

    detail::dropOrphanedSeparators(items, fates);
    return fates;
}

/// collapseStatusLine(), each segment measured as the sum of its items' widths.
/// @param items Every item of the line, each with its segment, width, priority and kind, segment by segment.
/// @param columns The line's width.
/// @return One fate per item, in the order of @p items.
[[nodiscard]] inline std::vector<StatusLineItemFate> collapseStatusLine(
    std::span<StatusLineItemExtent const> items, ColumnCount columns)
{
    return collapseStatusLine(items, columns, detail::SumOfItemWidths { .items = items });
}

} // namespace vtbackend
