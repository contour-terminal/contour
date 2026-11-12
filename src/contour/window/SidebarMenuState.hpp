// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/SidebarMenus.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <core/Utils.hpp>

#include <cstdint>
#include <ranges>
#include <vector>

namespace contour::window
{

/// The workspaces a tab of @p directory's selected workspace can move to: every other one.
/// @param directory What the window lists of its daemon's workspaces.
/// @return One choice per workspace but the selected one, in list order; each is titled with the
///         workspace's title (@see workspaceTitleOf).
[[nodiscard]] inline std::vector<command::WorkspaceChoice> moveTargetsOf(WorkspaceDirectory const& directory)
{
    auto choices = std::vector<command::WorkspaceChoice> {};
    for (auto const [index, workspace]: core::views::enumerate(directory.workspaces))
    {
        if (directory.selected == workspace.id)
            continue;
        choices.push_back(command::WorkspaceChoice {
            .position = static_cast<std::uint32_t>(index + 1),
            .title = workspaceTitleOf(workspace, static_cast<std::size_t>(index)) });
    }
    return choices;
}

} // namespace contour::window
