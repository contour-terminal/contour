// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Style.hpp
/// @brief A flexbox layout node: how it lays out its children, and how it sizes itself in its parent.

#include <core/tui/Rect.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace core::tui::layout
{

/// @brief The axis a node's children are placed along: the main axis.
enum class Direction : std::uint8_t
{
    Row,    ///< Left to right; the main axis is the width.
    Column, ///< Top to bottom; the main axis is the height.
};

/// @brief Whether children that do not fit on one line start another.
enum class Wrap : std::uint8_t
{
    NoWrap, ///< One line; children that do not fit shrink, then overflow.
    Wrap,   ///< A child that would not fit on the current line starts the next one.
};

/// @brief Where the cells a line leaves over go, along the main axis.
enum class Justify : std::uint8_t
{
    Start,        ///< Children packed at the start; the leftover after them.
    Center,       ///< Children in the middle; the odd cell of leftover goes after them.
    End,          ///< Children packed at the end; the leftover before them.
    SpaceBetween, ///< The leftover between the children; the first and last touch the edges.
};

/// @brief Where a child sits across the main axis, within its line.
enum class Align : std::uint8_t
{
    Stretch, ///< The line's whole cross size, within the child's min and max.
    Start,   ///< At the line's start, at its preferred cross size.
    Center,  ///< In the line's middle; the odd cell goes after it.
    End,     ///< At the line's end.
};

/// @brief The largest size, gap or position, in cells, the layout works with; larger ones are capped to it.
inline constexpr int MaxCells = 1 << 20;

/// @brief The largest `grow` or `shrink` the layout works with; larger ones are capped to it.
inline constexpr int MaxWeight = 1 << 16;

/// @brief The most children of one node the layout places. Children past it are not laid out: each of them,
/// and every node beneath it, gets a zero-size rectangle at the parent's origin.
///
/// With `MaxCells` and `MaxWeight` it bounds every sum the layout forms: a total of shrink weights is at most
/// `MaxChildren × MaxWeight × MaxCells` = 2^56.
inline constexpr std::size_t MaxChildren = std::size_t { 1 } << 20U;

/// @brief One node's layout properties, in integer cells.
///
/// The container half (`direction` to `gap`) says how the node lays out its own children; the item
/// half (`grow` to `maxSize`) says how the node is sized by its parent. A negative number counts as 0, a
/// size above `MaxCells` as `MaxCells`, and a `grow` or `shrink` above `MaxWeight` as `MaxWeight`, so no sum
/// or product the layout forms can overflow. Every enumeration defaults to its first enumerator.
struct Style
{
    Direction direction = Direction::Row; ///< The main axis of the children.
    Wrap wrap = Wrap::NoWrap;             ///< Whether the children break into lines.
    Justify justify = Justify::Start;     ///< Where a line's leftover goes along the main axis.
    Align align = Align::Stretch;         ///< Where each child sits across the main axis.
    int gap = 0;                          ///< Cells between two children, and between two lines.

    int grow = 0;   ///< Share of a line's leftover cells this node takes; 0 takes none.
    int shrink = 1; ///< Weight, times the basis, of a line's deficit this node gives up; 0 gives none.
    std::optional<int> basis =
        std::nullopt;  ///< Main size before growing or shrinking; the preferred size's when absent.
    Size minSize = {}; ///< The node never shrinks below this, unless the parent itself is smaller.
    Size maxSize = {}; ///< The node never grows above this; 0 in a dimension is unbounded.
};

/// @brief A node of the tree that `solve()` lays out.
struct Node
{
    Style style = {};                ///< How this node is sized and how it places its children.
    std::vector<Node> children = {}; ///< Laid out in order along `style.direction`.
    Size preferred = {};             ///< The size the node would take if nothing grew or shrank it.
};

} // namespace core::tui::layout
