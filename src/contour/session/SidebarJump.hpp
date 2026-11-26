// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A sidebar row's daemon, as a switch names it: the one step from the sidebar's DaemonRef to the
/// WindowSwitching service's SwitchTarget. Qt-free.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonSection.hpp>

#include <optional>
#include <string>
#include <utility>

namespace contour::session
{

/// Names a daemon and one of its workspaces as a switch does.
/// @param daemon The daemon, as the sidebar names it.
/// @param workspace Which of its workspaces.
/// @param focus The tab and pane within it the row names, for the window to activate once it shows it.
/// @param instance The run of the daemon the row was listed in, which the switch refuses once it ended.
/// @return A local daemon by its label (SwitchDaemonKind::Name), a saved host by its name (Host); none for a
///         `link:` daemon, which no switch can name, and for a reference that does not parse.
[[nodiscard]] inline std::optional<SwitchTarget> switchTargetOf(window::DaemonRef const& daemon,
                                                                WorkspaceTarget workspace,
                                                                LandingFocus focus,
                                                                window::DaemonInstance instance)
{
    auto const parsed = window::parseDaemonRef(daemon);
    if (!parsed)
        return std::nullopt;
    auto const kind = [&]() -> std::optional<vthost::proto::SwitchDaemonKind> {
        switch (parsed->first)
        {
            case window::DaemonRefKind::Local: return vthost::proto::SwitchDaemonKind::Name;
            case window::DaemonRefKind::Host: return vthost::proto::SwitchDaemonKind::Host;
            case window::DaemonRefKind::Link: return std::nullopt;
        }
        return std::nullopt;
    }();
    if (!kind)
        return std::nullopt;
    return SwitchTarget { .kind = *kind,
                          .daemon = std::string { parsed->second },
                          .workspace = std::move(workspace),
                          .focus = std::move(focus),
                          .instance = instance };
}

} // namespace contour::session
