// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <orchestrator/core/Commands.hpp>

namespace orchestrator
{

/// The agent a worker runs: one kind per row of AgentRows.
enum class AgentKind : uint8_t
{
    Claude = 0, ///< Claude Code; spelled `claude`.
    Copilot,    ///< GitHub Copilot's CLI; spelled `copilot`.
};

/// Whether an agent reports its working, blocked and done states to the dashboard.
enum class StatusReporting : uint8_t
{
    Silent = 0, ///< It does not report them.
    Reported,   ///< It reports them.
};

/// Whether an agent reads Claude Code's state: its hooks, plugins, transcripts and stats.
enum class ClaudeState : uint8_t
{
    None = 0, ///< It reads none of it.
    Read,     ///< It reads it.
};

/// Whether an agent takes a permission profile, passed as `--settings`.
enum class SettingsProfile : uint8_t
{
    NotTaken = 0, ///< It takes none: its permission profile cannot be applied.
    Taken,        ///< It takes one.
};

/// What the orchestrator knows about one kind of agent. A new agent is a new row.
struct AgentRow
{
    AgentKind kind;             ///< The kind the row describes.
    std::string_view name;      ///< As `workers[].agent` and the stored run spell it.
    std::string_view boardPane; ///< Its pane template when a board is asked for.
    std::string_view plainPane; ///< Its pane template otherwise.
    StatusReporting status;     ///< Whether it reports its states.
    ClaudeState claudeState;    ///< Whether it reads Claude Code's state.
    SettingsProfile settings;   ///< Whether it takes a permission profile.
};

/// Every agent kind the orchestrator runs, one row each, in the order of AgentKind.
inline constexpr auto AgentRows = std::array {
    AgentRow { .kind = AgentKind::Claude,
               .name = "claude",
               .boardPane = AgentPaneTemplate,
               .plainPane = AgentPlainPaneTemplate,
               .status = StatusReporting::Reported,
               .claudeState = ClaudeState::Read,
               .settings = SettingsProfile::Taken },
    AgentRow { .kind = AgentKind::Copilot,
               .name = "copilot",
               .boardPane = CopilotPaneTemplate,
               .plainPane = CopilotPaneTemplate,
               .status = StatusReporting::Silent,
               .claudeState = ClaudeState::None,
               .settings = SettingsProfile::NotTaken },
};

/// @param kind An agent kind.
/// @return Its row in AgentRows.
[[nodiscard]] AgentRow const& agentRowOf(AgentKind kind) noexcept;

/// @param name An agent's name, as `workers[].agent` spells it.
/// @return The row of that agent, or nullptr if no agent has that name.
[[nodiscard]] AgentRow const* findAgent(std::string_view name) noexcept;

} // namespace orchestrator
