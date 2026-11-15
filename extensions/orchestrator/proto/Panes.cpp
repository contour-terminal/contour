// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Panes.hpp>

#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/proto/PanesJson.hpp>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    using detail::JsonProgramStatus;
    using detail::JsonWorkspaceList;

    /// The input is a string_view: nothing terminates it, and a newer `contour` adds keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// @return The status the agent's record reports; std::nullopt without one, or for a state not known.
    [[nodiscard]] std::optional<AgentStatus> agentStatusOf(std::span<JsonProgramStatus const> records)
    {
        auto const record = std::ranges::find(records, AgentStatusRecord, &JsonProgramStatus::id);
        if (record == records.end())
            return std::nullopt;
        return agentStatusNamed(record->state);
    }
} // namespace

std::optional<std::string_view> enginePaneId(std::string_view controlId) noexcept
{
    if (!controlId.starts_with(ControlPanePrefix) || controlId.size() == ControlPanePrefix.size())
        return std::nullopt;
    return controlId.substr(ControlPanePrefix.size());
}

std::string controlPaneId(std::string_view enginePane)
{
    auto id = std::string(ControlPanePrefix);
    id += enginePane;
    return id;
}

std::expected<std::vector<PaneFact>, ParseError> parseWorkspaceList(std::string_view json,
                                                                    std::span<OwnedPane const> owned)
{
    auto const listing = orchestrator::json::readDocument<ReadOptions, JsonWorkspaceList>(json);
    if (!listing)
        return std::unexpected { listing.error() };

    auto facts = std::vector<PaneFact> {};
    for (auto const& workspace: listing->workspaces)
        for (auto const& tab: workspace.tabs)
            for (auto const& pane: tab.panes)
            {
                auto const id = enginePaneId(pane.id);
                if (!id)
                    continue;
                auto const record = std::ranges::find(owned, *id, &OwnedPane::pane);
                if (record == owned.end())
                    continue;
                facts.push_back(PaneFact { .pane = std::string(*id),
                                           .tag = record->tag,
                                           .status = agentStatusOf(pane.programStatus),
                                           .program = ProgramLiveness::Alive });
            }
    return facts;
}

} // namespace orchestrator::proto
