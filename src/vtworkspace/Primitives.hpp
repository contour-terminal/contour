// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

#include <boxed-cpp/boxed.hpp>

namespace vtworkspace
{

namespace detail::tags
{
    // clang-format off
    struct SessionId {};
    struct PaneId {};
    struct TabId {};
    struct WindowId {};
    // clang-format on
} // namespace detail::tags

/// Identifies a terminal session (one PTY + vtbackend::Terminal).
///
/// The layout model (this library) only ever refers to a session by this id; it never owns or
/// depends on the host's session object. The GUI maps a SessionId to its TerminalSession; a future
/// daemon maps it to an owned Terminal+Pty. Ids are stable for the lifetime of the session so a
/// client may reattach to it.
using SessionId = boxed::boxed<uint64_t, detail::tags::SessionId>;

/// Identifies a pane (a node in a tab's split tree). Stable across split/close so that event
/// subscribers (Qt proxies today, network clients later) can address a node by id even as the tree
/// is reshaped around it.
using PaneId = boxed::boxed<uint64_t, detail::tags::PaneId>;

/// Identifies a tab within the model.
using TabId = boxed::boxed<uint64_t, detail::tags::TabId>;

/// Identifies a logical window within the model.
using WindowId = boxed::boxed<uint64_t, detail::tags::WindowId>;

/// The split orientation of a pane node.
///
/// A pane is a leaf if and only if its split state is None; otherwise it is an internal node with
/// exactly two children laid out along the given axis.
enum class SplitState : uint8_t
{
    None = 0,   //!< Leaf node: holds a session, no children.
    Horizontal, //!< Internal node: children stacked top/bottom (divider is horizontal).
    Vertical,   //!< Internal node: children placed left/right (divider is vertical).
};

/// A direction for directional pane focus navigation and pane resizing.
enum class FocusDirection : uint8_t
{
    Left = 0,
    Right,
    Up,
    Down,
};

/// The side of a pane that a new half is placed on when a whole tab is grafted beside it.
enum class SplitSide : uint8_t
{
    Left = 0,
    Right,
    Top,
    Bottom,
};

/// Which child a grafted subtree becomes, relative to the leaf it is grafted beside.
enum class GraftOrder : uint8_t
{
    GraftSecond = 0, //!< The subtree is the second child; the existing pane stays first (Right, Bottom).
    GraftFirst,      //!< The subtree is the first child; the existing pane becomes second (Left, Top).
};

/// Returns the split axis that places a grafted half on @p side: Left/Right split Vertical (children
/// side by side), Top/Bottom split Horizontal (children stacked).
[[nodiscard]] constexpr SplitState splitStateOf(SplitSide side) noexcept
{
    return (side == SplitSide::Left || side == SplitSide::Right) ? SplitState::Vertical
                                                                 : SplitState::Horizontal;
}

/// Returns whether a half grafted on @p side comes first (Left, Top) or second (Right, Bottom).
[[nodiscard]] constexpr GraftOrder graftOrderOf(SplitSide side) noexcept
{
    return (side == SplitSide::Left || side == SplitSide::Top) ? GraftOrder::GraftFirst
                                                               : GraftOrder::GraftSecond;
}

/// Every side a half can be grafted on.
inline constexpr auto AllSplitSides =
    std::array { SplitSide::Left, SplitSide::Right, SplitSide::Top, SplitSide::Bottom };

/// Returns the side that grafts a half along @p state in @p order: the inverse of splitStateOf() and
/// graftOrderOf() together.
/// @return The side; none for SplitState::None, which no side splits along.
[[nodiscard]] constexpr std::optional<SplitSide> sideOf(SplitState state, GraftOrder order) noexcept
{
    auto const side = std::ranges::find_if(AllSplitSides, [&](SplitSide candidate) {
        return splitStateOf(candidate) == state && graftOrderOf(candidate) == order;
    });
    return side != AllSplitSides.end() ? std::optional { *side } : std::nullopt;
}

/// Returns the split axis you must cross to move along @p direction.
///
/// Moving Left/Right crosses a Vertical split (children are side by side); moving Up/Down crosses a
/// Horizontal split (children are stacked).
[[nodiscard]] constexpr SplitState crossingSplitFor(FocusDirection direction) noexcept
{
    // Total over the four directions — expressed as the horizontal/vertical dichotomy (mirroring
    // pointsTowardSecondChild) so there is no dead out-of-domain fallback to maintain or test.
    return (direction == FocusDirection::Left || direction == FocusDirection::Right) ? SplitState::Vertical
                                                                                     : SplitState::Horizontal;
}

/// Returns whether @p direction points toward the second child of a split (Right/Down) as opposed
/// to the first child (Left/Up).
[[nodiscard]] constexpr bool pointsTowardSecondChild(FocusDirection direction) noexcept
{
    return direction == FocusDirection::Right || direction == FocusDirection::Down;
}

} // namespace vtworkspace
