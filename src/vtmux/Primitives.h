// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <boxed-cpp/boxed.hpp>

namespace vtmux
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

/// Returns the split axis you must cross to move along @p direction.
///
/// Moving Left/Right crosses a Vertical split (children are side by side); moving Up/Down crosses a
/// Horizontal split (children are stacked).
[[nodiscard]] constexpr SplitState crossingSplitFor(FocusDirection direction) noexcept
{
    switch (direction)
    {
        case FocusDirection::Left:
        case FocusDirection::Right: return SplitState::Vertical;
        case FocusDirection::Up:
        case FocusDirection::Down: return SplitState::Horizontal;
    }
    return SplitState::None;
}

/// Returns whether @p direction points toward the second child of a split (Right/Down) as opposed
/// to the first child (Left/Up).
[[nodiscard]] constexpr bool pointsTowardSecondChild(FocusDirection direction) noexcept
{
    return direction == FocusDirection::Right || direction == FocusDirection::Down;
}

} // namespace vtmux
