// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Agents.hpp>

#include <algorithm>
#include <cstddef>

namespace orchestrator
{

AgentRow const& agentRowOf(AgentKind kind) noexcept
{
    // AgentRows holds one row per kind, in the order of AgentKind.
    return AgentRows[static_cast<std::size_t>(kind)];
}

AgentRow const* findAgent(std::string_view name) noexcept
{
    auto const row = std::ranges::find(AgentRows, name, &AgentRow::name);
    return row != AgentRows.end() ? &*row : nullptr;
}

} // namespace orchestrator
