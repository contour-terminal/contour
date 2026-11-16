// SPDX-License-Identifier: Apache-2.0
#include <core/tui/layout/Solve.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace core::tui::layout
{

namespace
{

    /// The limit a `maxSize` of 0 stands for.
    constexpr auto Unbounded = MaxCells;

    /// Which children take the cells a weighted share leaves over, one each.
    enum class RemainderOrder : std::uint8_t
    {
        Earliest, ///< From the first child on: where leftover cells go.
        Latest,   ///< From the last child back: where deficit cells are taken.
    };

    /// Whether a child's main size may still change in the current line.
    enum class ItemState : std::uint8_t
    {
        Flexible, ///< Takes part in the next share.
        Frozen,   ///< Held at a limit it was pushed past.
    };

    /// One child as its line sees it, every number non-negative and every basis within its limits.
    struct Item
    {
        int basis = 0;
        int minMain = 0;
        int maxMain = Unbounded;
        int grow = 0;
        int shrink = 0;
        int crossNatural = 0;
        int minCross = 0;
        int maxCross = Unbounded;
    };

    /// A run of children laid out on one line.
    struct Line
    {
        std::size_t first = 0;
        std::size_t count = 0;
        int cross = 0;
    };

    /// A size, gap or basis within [0, MaxCells].
    [[nodiscard]] constexpr int cells(int value) noexcept
    {
        return std::clamp(value, 0, MaxCells);
    }

    /// A grow or shrink factor within [0, MaxWeight].
    [[nodiscard]] constexpr int weight(int value) noexcept
    {
        return std::clamp(value, 0, MaxWeight);
    }

    /// A `maxSize` dimension: 0 or less is unbounded, and it is never below @p minimum.
    [[nodiscard]] constexpr int limitOf(int maximum, int minimum) noexcept
    {
        return maximum <= 0 ? Unbounded : std::max(cells(maximum), minimum);
    }

    /// `floor(a * b / c)` for `b <= c`, exact where `a * b` exceeds 64 bits: the product is formed in
    /// 32-bit halves and divided bit by bit. The quotient is at most @p a, so it fits.
    [[nodiscard]] constexpr std::uint64_t mulDiv(std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept
    {
        if (a == 0 || b <= std::numeric_limits<std::uint64_t>::max() / a)
            return a * b / c;

        constexpr auto Low = std::uint64_t { 0xFFFF'FFFF };
        auto const p0 = (a & Low) * (b & Low);
        auto const p1 = (a & Low) * (b >> 32U);
        auto const p2 = (a >> 32U) * (b & Low);
        auto const p3 = (a >> 32U) * (b >> 32U);
        auto const middle = (p0 >> 32U) + (p1 & Low) + (p2 & Low);
        auto const low = (p0 & Low) | (middle << 32U);
        auto const high = p3 + (p1 >> 32U) + (p2 >> 32U) + (middle >> 32U);

        // The quotient fits in 64 bits (it is at most a), so high < c: it is the remainder after the upper
        // half, and only the 64 bits of the lower half are left to divide. c is a sum of non-negative int64
        // weights, below 2^63, so the shifted remainder needs no 65th bit.
        auto quotient = std::uint64_t { 0 };
        auto remainder = high;
        for (auto const bit: std::views::iota(0U, 64U) | std::views::reverse)
        {
            remainder = (remainder << 1U) | ((low >> bit) & 1U);
            if (remainder >= c)
            {
                remainder -= c;
                quotient |= std::uint64_t { 1 } << bit;
            }
        }
        return quotient;
    }

    [[nodiscard]] constexpr int mainOf(Direction direction, Size size) noexcept
    {
        return direction == Direction::Row ? size.width : size.height;
    }

    [[nodiscard]] constexpr int crossOf(Direction direction, Size size) noexcept
    {
        return direction == Direction::Row ? size.height : size.width;
    }

    [[nodiscard]] constexpr int mainOf(Direction direction, Point point) noexcept
    {
        return direction == Direction::Row ? point.x : point.y;
    }

    [[nodiscard]] constexpr int crossOf(Direction direction, Point point) noexcept
    {
        return direction == Direction::Row ? point.y : point.x;
    }

    [[nodiscard]] constexpr Rect axisRect(
        Direction direction, int mainPos, int crossPos, int mainLen, int crossLen) noexcept
    {
        if (direction == Direction::Row)
            return { .x = mainPos, .y = crossPos, .width = mainLen, .height = crossLen };
        return { .x = crossPos, .y = mainPos, .width = crossLen, .height = mainLen };
    }

    /// A run of cells along one axis.
    struct Span
    {
        int start = 0;
        int length = 0;
    };

    /// `[start, start + length)` cut to `[0, limit]`: a part outside becomes empty at the nearest edge,
    /// never negative. Computed in 64 bits, so a child far past the edge does not overflow.
    [[nodiscard]] constexpr Span clipSpan(std::int64_t start, std::int64_t length, int limit) noexcept
    {
        auto const first = std::clamp<std::int64_t>(start, 0, limit);
        auto const last = std::clamp<std::int64_t>(start + length, first, limit);
        return { .start = static_cast<int>(first), .length = static_cast<int>(last - first) };
    }

    [[nodiscard]] Item itemOf(Node const& child, Direction direction) noexcept
    {
        auto const& style = child.style;
        auto const minMain = cells(mainOf(direction, style.minSize));
        auto const maxMain = limitOf(mainOf(direction, style.maxSize), minMain);
        auto const minCross = cells(crossOf(direction, style.minSize));
        auto const maxCross = limitOf(crossOf(direction, style.maxSize), minCross);
        auto const basis = cells(style.basis.value_or(mainOf(direction, child.preferred)));
        return { .basis = std::clamp(basis, minMain, maxMain),
                 .minMain = minMain,
                 .maxMain = maxMain,
                 .grow = weight(style.grow),
                 .shrink = weight(style.shrink),
                 .crossNatural = std::clamp(cells(crossOf(direction, child.preferred)), minCross, maxCross),
                 .minCross = minCross,
                 .maxCross = maxCross };
    }

    /// Shares @p amount in proportion to @p weights, in whole cells: each takes the whole part of its
    /// share, and the cells left over go one at a time to the weighted entries in @p order.
    /// All zeros when no weight is positive.
    [[nodiscard]] std::vector<std::int64_t> distribute(std::int64_t amount,
                                                       std::span<std::int64_t const> weights,
                                                       RemainderOrder order)
    {
        auto shares = std::vector<std::int64_t>(weights.size(), 0);
        // At most MaxChildren weights of at most MaxWeight x MaxCells each: the total cannot overflow.
        static_assert(MaxChildren * std::uint64_t { MaxWeight } * std::uint64_t { MaxCells }
                      < (std::uint64_t { 1 } << 63U));
        auto total = std::int64_t { 0 };
        for (auto const weight: weights)
            total += weight;
        if (total <= 0)
            return shares;

        auto given = std::int64_t { 0 };
        for (auto const i: std::views::iota(std::size_t { 0 }, weights.size()))
        {
            shares[i] = static_cast<std::int64_t>(mulDiv(static_cast<std::uint64_t>(amount),
                                                         static_cast<std::uint64_t>(weights[i]),
                                                         static_cast<std::uint64_t>(total)));
            given += shares[i];
        }

        // Each whole part loses less than one cell, so fewer cells are left than there are weighted entries.
        auto remainder = amount - given;
        auto const handOut = [&](std::size_t i) {
            if (remainder > 0 && weights[i] > 0)
            {
                ++shares[i];
                --remainder;
            }
        };
        auto const indices = std::views::iota(std::size_t { 0 }, weights.size());
        if (order == RemainderOrder::Earliest)
            std::ranges::for_each(indices, handOut);
        else
            std::ranges::for_each(indices | std::views::reverse, handOut);
        return shares;
    }

    /// The main sizes of one line's items within @p space: grown or shrunk from their bases, a child
    /// pushed past a limit frozen there and the rest shared again, until no limit is crossed.
    [[nodiscard]] std::vector<int> resolveMainSizes(std::span<Item const> items, int space)
    {
        auto sizes = std::vector<int>();
        sizes.reserve(items.size());
        for (auto const& item: items)
            sizes.push_back(item.basis);
        auto states = std::vector<ItemState>(items.size(), ItemState::Flexible);
        auto weights = std::vector<std::int64_t>(items.size(), 0);

        // Each pass either ends or freezes at least one item, so there are at most n + 1.
        for ([[maybe_unused]] auto const pass: std::views::iota(std::size_t { 0 }, items.size() + 1))
        {
            auto used = std::int64_t { 0 };
            for (auto const size: sizes)
                used += size;
            auto const free = std::int64_t { space } - used;
            if (free == 0)
                return sizes;

            auto const growing = free > 0;
            for (auto const i: std::views::iota(std::size_t { 0 }, items.size()))
            {
                auto const& item = items[i];
                if (states[i] == ItemState::Frozen)
                    weights[i] = 0;
                else if (growing)
                    weights[i] = item.grow;
                else
                    weights[i] = std::int64_t { item.shrink } * item.basis;
            }
            auto const shares = distribute(
                growing ? free : -free, weights, growing ? RemainderOrder::Earliest : RemainderOrder::Latest);

            auto targets = std::vector<std::int64_t>(items.size(), 0);
            auto anyFrozen = false;
            for (auto const i: std::views::iota(std::size_t { 0 }, items.size()))
            {
                if (states[i] == ItemState::Frozen)
                    continue;
                auto const& item = items[i];
                targets[i] = growing ? item.basis + shares[i] : item.basis - shares[i];
                auto const clamped = std::clamp<std::int64_t>(targets[i], item.minMain, item.maxMain);
                if (clamped != targets[i])
                {
                    sizes[i] = static_cast<int>(clamped);
                    states[i] = ItemState::Frozen;
                    anyFrozen = true;
                }
            }
            if (!anyFrozen)
            {
                for (auto const i: std::views::iota(std::size_t { 0 }, items.size()))
                    if (states[i] == ItemState::Flexible)
                        sizes[i] = static_cast<int>(targets[i]);
                return sizes;
            }
        }
        return sizes;
    }

    /// The children's lines: one for `Wrap::NoWrap`, else a new line wherever the next child's basis
    /// would not fit after the current line's.
    [[nodiscard]] std::vector<Line> breakLines(std::span<Item const> items, Wrap wrap, int space, int gap)
    {
        auto lines = std::vector<Line>();
        if (items.empty())
            return lines;
        if (wrap == Wrap::NoWrap)
        {
            lines.push_back({ .first = 0, .count = items.size() });
            return lines;
        }

        lines.push_back({ .first = 0, .count = 0 });
        auto length = std::int64_t { 0 };
        for (auto const i: std::views::iota(std::size_t { 0 }, items.size()))
        {
            auto const basis = items[i].basis;
            if (lines.back().count > 0 && length + gap + basis > space)
            {
                lines.push_back({ .first = i, .count = 0 });
                length = 0;
            }
            length += (lines.back().count > 0 ? gap : 0) + basis;
            ++lines.back().count;
        }
        return lines;
    }

    /// Gives each line its cross size: a single unwrapped line the container's, wrapped lines their
    /// tallest item's plus an equal part of what the container leaves over (earliest lines first).
    void sizeLines(std::span<Line> lines, std::span<Item const> items, Wrap wrap, int crossSpace, int gap)
    {
        if (wrap == Wrap::NoWrap)
        {
            for (auto& line: lines)
                line.cross = crossSpace;
            return;
        }

        auto used = std::int64_t { gap } * static_cast<std::int64_t>(lines.size() - 1);
        for (auto& line: lines)
        {
            for (auto const& item: items.subspan(line.first, line.count))
                line.cross = std::max(line.cross, item.crossNatural);
            used += line.cross;
        }
        auto const leftover = std::int64_t { crossSpace } - used;
        if (leftover <= 0)
            return;
        auto const weights = std::vector<std::int64_t>(lines.size(), 1);
        auto const shares = distribute(leftover, weights, RemainderOrder::Earliest);
        for (auto const i: std::views::iota(std::size_t { 0 }, lines.size()))
            lines[i].cross += static_cast<int>(shares[i]);
    }

    /// Where along the main axis a line's first item starts, and the extra cells after each gap.
    [[nodiscard]] std::pair<int, std::vector<std::int64_t>> justifyLine(Justify justify,
                                                                        std::size_t count,
                                                                        int leftover)
    {
        auto between = std::vector<std::int64_t>(count > 0 ? count - 1 : 0, 0);
        if (leftover <= 0)
            return { 0, between };
        switch (justify)
        {
            case Justify::Start: return { 0, between };
            case Justify::Center: return { leftover / 2, between };
            case Justify::End: return { leftover, between };
            case Justify::SpaceBetween: {
                auto const weights = std::vector<std::int64_t>(between.size(), 1);
                return { 0, distribute(leftover, weights, RemainderOrder::Earliest) };
            }
        }
        return { 0, between };
    }

    /// An item's cross offset and size within a line of @p lineCross cells.
    [[nodiscard]] constexpr std::pair<int, int> alignItem(Align align,
                                                          Item const& item,
                                                          int lineCross) noexcept
    {
        auto const natural =
            align == Align::Stretch ? std::clamp(lineCross, item.minCross, item.maxCross) : item.crossNatural;
        auto const size = std::min(natural, lineCross);
        switch (align)
        {
            case Align::Start:
            case Align::Stretch: return { 0, size };
            case Align::Center: return { (lineCross - size) / 2, size };
            case Align::End: return { lineCross - size, size };
        }
        return { 0, size };
    }

    /// Each child's rectangle within @p area, the content of a node styled @p style.
    [[nodiscard]] std::vector<Rect> layoutChildren(Node const& node, Rect area)
    {
        auto const& style = node.style;
        auto const direction = style.direction;
        auto const gap = cells(style.gap);
        auto const mainSpace = mainOf(direction, area.size());
        auto const crossSpace = crossOf(direction, area.size());

        // Children past MaxChildren are not laid out; their rectangles stay zero-size at the area's origin.
        auto const placed =
            std::span<Node const>(node.children).first(std::min(node.children.size(), MaxChildren));
        auto items = std::vector<Item>();
        items.reserve(placed.size());
        for (auto const& child: placed)
            items.push_back(itemOf(child, direction));

        auto lines = breakLines(items, style.wrap, mainSpace, gap);
        sizeLines(lines, items, style.wrap, crossSpace, gap);

        auto rects = std::vector<Rect>(node.children.size(),
                                       Rect { .x = area.x, .y = area.y, .width = 0, .height = 0 });
        auto const mainOrigin = mainOf(direction, area.position());
        auto const crossOrigin = crossOf(direction, area.position());
        auto lineOffset = std::int64_t { 0 };
        for (auto const& line: lines)
        {
            auto const lineItems = std::span<Item const>(items).subspan(line.first, line.count);
            auto const gaps = std::int64_t { gap } * static_cast<std::int64_t>(line.count - 1);
            auto const itemSpace = static_cast<int>(std::max<std::int64_t>(0, mainSpace - gaps));
            auto const sizes = resolveMainSizes(lineItems, itemSpace);

            auto used = gaps;
            for (auto const size: sizes)
                used += size;
            auto const leftover = static_cast<int>(std::max<std::int64_t>(0, mainSpace - used));
            auto const [lead, between] = justifyLine(style.justify, line.count, leftover);

            auto position = std::int64_t { lead };
            for (auto const i: std::views::iota(std::size_t { 0 }, line.count))
            {
                auto const [crossPos, crossSize] = alignItem(style.align, lineItems[i], line.cross);
                // Clipped relative to the area, so the last children lose cells first; area and origin are
                // both within MaxCells, so adding them back cannot overflow.
                auto const main = clipSpan(position, sizes[i], mainSpace);
                auto const cross = clipSpan(lineOffset + crossPos, crossSize, crossSpace);
                rects[line.first + i] = axisRect(
                    direction, mainOrigin + main.start, crossOrigin + cross.start, main.length, cross.length);
                position += sizes[i];
                if (i + 1 < line.count)
                    position += gap + between[i];
            }
            lineOffset += line.cross + gap;
        }
        return rects;
    }

    void solveInto(Node const& node, Rect area, std::vector<Rect>& out)
    {
        out.push_back(area);
        if (node.children.empty())
            return;
        auto const rects = layoutChildren(node, area);
        for (auto const i: std::views::iota(std::size_t { 0 }, node.children.size()))
            solveInto(node.children[i], rects[i], out);
    }

} // namespace

std::vector<Rect> solve(Node const& root, Rect available)
{
    auto out = std::vector<Rect>();
    available.x = std::clamp(available.x, -MaxCells, MaxCells);
    available.y = std::clamp(available.y, -MaxCells, MaxCells);
    available.width = cells(available.width);
    available.height = cells(available.height);
    solveInto(root, available, out);
    return out;
}

} // namespace core::tui::layout
