// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string_view>

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

} // namespace orchestrator
