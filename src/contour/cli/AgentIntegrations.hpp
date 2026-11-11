// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The coding agents whose hooks `contour agent report` understands, one row each: how a hook's payload (JSON
/// on standard input) maps onto a pane's program status (`pane.reportStatus` / `pane.clearStatus`).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <vtcontrol/Json.hpp>

namespace contour::cli
{

/// The agents with a row in the table.
enum class AgentId : uint8_t
{
    ClaudeCode = 0,
    Codex,
};

/// A hook's meaning: the pane's program is in this state.
struct StatusReport
{
    std::string state;   ///< idle | working | done | blocked | error.
    std::string kind;    ///< none | permission | question | auth; empty for none.
    std::string message; ///< The reason line; may be empty.
};

/// A hook's meaning: the program is gone, drop its record.
struct StatusClear
{
};

/// What one hook payload means: nothing (monostate), a report, or a clear.
using HookOutcome = std::variant<std::monostate, StatusReport, StatusClear>;

/// One agent's row.
struct AgentIntegration
{
    AgentId id;                     ///< The agent.
    std::string_view integrationId; ///< "claude-code", "codex": the status record's id.
    std::string_view label;         ///< What it is called to people.
    /// The hook events installed by `contour agent setup`, one matcher group each, and mapped by @ref map.
    std::span<std::string_view const> hookEvents;
    /// @param payload The decoded hook payload.
    /// @return What it means.
    HookOutcome (*map)(glz::generic const& payload);
};

/// @return Every integration, in AgentId order.
[[nodiscard]] std::span<AgentIntegration const> agentIntegrations() noexcept;

/// @param integrationId A record id such as "claude-code".
/// @return Its row, or null when there is none.
[[nodiscard]] AgentIntegration const* findIntegration(std::string_view integrationId) noexcept;

/// @param id An agent.
/// @return Its row.
[[nodiscard]] AgentIntegration const& integrationOf(AgentId id) noexcept;

} // namespace contour::cli
