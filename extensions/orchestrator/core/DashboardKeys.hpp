// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string_view>

#include <orchestrator/core/NavKeys.hpp>

namespace orchestrator
{

/// One key the dashboard binds itself: no task may be started by it.
struct DashboardKey
{
    std::string_view name; ///< What the view's key table calls it, such as `enter`, `j` or `up`.
    char character;        ///< The character it types; '\0' for a key that types none, such as an arrow.
};

/// Every key the dashboard binds or reserves, a task's key aside: the configuration refuses a task key among
/// them, and the view binds no other. In the core, so that the configuration needs no view to know them.
/// @return The keys, in the order a problem line lists them.
[[nodiscard]] std::span<DashboardKey const> dashboardKeys() noexcept;

/// One key a view binds to a command: no navigation binding may take it in its context.
struct CommandKey
{
    std::string_view name; ///< Its spelling, as `keys` writes it (keyShapeOf()).
    std::string_view does; ///< What it does, as a problem line ends: "the dashboard's key to <does>".
    NavContext context;    ///< Where the views bind it.
};

/// The keys the views bind to commands, as the clash rules of `keys` check them (Decision 8). Unlike
/// dashboardKeys(), it holds no navigation key and every key a view binds to a command.
/// @return Every key the views bind to a command, per context, in the order a problem line would name them.
[[nodiscard]] std::span<CommandKey const> commandKeys() noexcept;

/// One key the message box passes on to the Focus while it is typed, and what it does there.
struct MessagePassKey
{
    std::string_view name; ///< Its spelling, as `keys` writes it.
    std::string_view does; ///< What it does in the Focus.
};

/// @return pgup, pgdn, end, left, right (tab is a command key of its own); commandKeys()' Typing rows include
///         them.
[[nodiscard]] std::span<MessagePassKey const> messagePassKeys() noexcept;

} // namespace orchestrator
