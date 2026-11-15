// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Reconcile.hpp>

/// The prototype driver's one piece of logic beyond I/O glue: reading Contour's pane listing.
namespace orchestrator::proto
{

/// A pane the driver opened for a run: its record of the tab, since Contour's listing carries no tag.
struct OwnedPane
{
    std::string pane; ///< The pane's id, as the engine knows it (see enginePaneId()).
    std::string tag;  ///< The id of the run it was opened for.
};

/// The prefix of every pane id Contour's control API spells.
inline constexpr std::string_view ControlPanePrefix = "pane:";

/// The program-status record a run's agent reports under: the integration its hooks name.
inline constexpr std::string_view AgentStatusRecord = "claude-code";

/// The engine's spelling of a control-API pane id: the id without its `pane:` prefix, which a Name
/// (ArgumentKind::Name) cannot hold.
/// @param controlId Such as `pane:01a1df08-04b3-717f-b69a-d342bf66080e`.
/// @return The id after the prefix; std::nullopt when @p controlId has no prefix or nothing after it.
[[nodiscard]] std::optional<std::string_view> enginePaneId(std::string_view controlId) noexcept;

/// The control API's spelling of an engine pane id.
/// @param enginePane The id as the engine keeps it.
/// @return @p enginePane with its `pane:` prefix.
[[nodiscard]] std::string controlPaneId(std::string_view enginePane);

/// Reads a `workspace.list` reply into the facts the engine reconciles with.
///
/// Only the panes in @p owned are returned (PaneListing::OwnRecordsOnly): Contour's listing has no tag.
/// A listed pane is ProgramLiveness::Alive, because the agent is the pane's program and the pane closes
/// with it. Its status is the state of its AgentStatusRecord program-status record, if it has one.
/// @param json The reply, as `contour api call workspace.list` prints it.
/// @param owned The panes the driver opened, with the runs they were opened for.
/// @return The listed owned panes, in listing order; or why the reply could not be read, in which case
///         nothing may be fed to the engine.
[[nodiscard]] std::expected<std::vector<PaneFact>, ParseError> parseWorkspaceList(
    std::string_view json, std::span<OwnedPane const> owned);

} // namespace orchestrator::proto
