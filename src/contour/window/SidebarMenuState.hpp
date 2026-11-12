// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/SidebarMenus.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <core/Utils.hpp>

#include <cstdint>
#include <format>
#include <ranges>
#include <vector>

namespace contour::window
{

/// The workspaces a tab of @p directory's selected workspace can move to: every other one.
/// @param directory What the window lists of its daemon's workspaces.
/// @return One choice per workspace but the selected one, in list order; each is titled with the
///         workspace's label, or "Workspace N" (its 1-based position) when it has none.
[[nodiscard]] inline std::vector<command::WorkspaceChoice> moveTargetsOf(WorkspaceDirectory const& directory)
{
    auto choices = std::vector<command::WorkspaceChoice> {};
    for (auto const [index, workspace]: core::views::enumerate(directory.workspaces))
    {
        if (directory.selected == workspace.id)
            continue;
        auto const position = static_cast<std::uint32_t>(index + 1);
        choices.push_back(command::WorkspaceChoice {
            .position = position,
            .title = workspace.label.empty() ? std::format("Workspace {}", position) : workspace.label });
    }
    return choices;
}

} // namespace contour::window
