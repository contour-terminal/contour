// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Agents.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <utility>

namespace orchestrator
{

// agentRowOf indexes AgentRows by its kind, so each row stands at its kind's value.
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, AgentRows.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(AgentRows[index].kind) == index;
                                  }),
              "AgentRows must hold one row per AgentKind, in its order");

AgentRow const& agentRowOf(AgentKind kind) noexcept
{
    return AgentRows[static_cast<std::size_t>(kind)];
}

AgentRow const* findAgent(std::string_view name) noexcept
{
    auto const row = std::ranges::find(AgentRows, name, &AgentRow::name);
    return row != AgentRows.end() ? &*row : nullptr;
}

} // namespace orchestrator
