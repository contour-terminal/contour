// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a remote::WindowSwitcher needs from the application; light, so that the composition root can
/// implement it without including the switcher.

#include <expected>
#include <string>
#include <vector>

namespace contour::remote
{

class DaemonLink;
struct DaemonLinkConfig;
struct DaemonAddress;

/// What a WindowSwitcher needs from the application: its links, and new ones made to a daemon it names.
class ISwitchHost
{
  public:
    ISwitchHost() = default;
    ISwitchHost(ISwitchHost const&) = delete;
    ISwitchHost(ISwitchHost&&) = delete;
    ISwitchHost& operator=(ISwitchHost const&) = delete;
    ISwitchHost& operator=(ISwitchHost&&) = delete;
    virtual ~ISwitchHost() = default;

    /// @return The application's links, the ended ones not yet destroyed among them.
    [[nodiscard]] virtual std::vector<DaemonLink*> switchableLinks() = 0;

    /// Resolves a daemon to what a link to it is built with, as Attach to Host and the Connect picker resolve
    /// it: a local label to its socket, a saved host to its transport. Its intent is the caller's to set.
    /// @param daemon The daemon.
    /// @param window The window that asks for the switch: where the login prompts of the new link are shown.
    /// @return The configuration, or why the daemon cannot be reached as configured.
    [[nodiscard]] virtual std::expected<DaemonLinkConfig, std::string> linkConfigFor(
        DaemonAddress const& daemon, vtworkspace::WorkspaceId window) = 0;

    /// Makes a link and starts its connect (ContourGuiApp::startDaemonLink).
    /// @param config What it is built with.
    /// @return The link, owned by the application.
    virtual DaemonLink& startSwitchLink(DaemonLinkConfig config) = 0;
};

} // namespace contour::remote
