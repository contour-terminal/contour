// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The sections a window's sidebar lists: what the presence lists of every daemon, with the window's own
/// daemon's entry taken from the window itself, whose link is the live one. Qt-free and free of `remote/`.

#include <contour/window/DaemonSection.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace contour::window
{

/// The daemon a window is on, as its sidebar lists it: the section the window's own link feeds.
struct OwnDaemon
{
    DaemonRef daemon;                                      ///< Which daemon.
    DaemonInstance instance;                               ///< Which run of it.
    std::string name;                                      ///< What its row calls it.
    DaemonLocality locality = DaemonLocality::Remote;      ///< Whether it runs on this machine.
    DaemonLinkState state = DaemonLinkState::NotConnected; ///< Where the window's link stands.
};

/// The window's sections.
/// @param listed The sections the presence lists, in the order the sidebar shows them.
/// @param own The window's own section, from its own link.
/// @param displayedWorkspace The workspace the window displays, if any.
/// @return @p listed in its order, the entry for @p own's daemon replaced by @p own (appended when the
///         presence does not list it yet), the window's displayed workspace taken out of its shownElsewhere.
[[nodiscard]] inline std::vector<DaemonSection> windowSections(std::vector<DaemonSection> listed,
                                                               DaemonSection own,
                                                               std::optional<uint64_t> displayedWorkspace)
{
    if (displayedWorkspace)
        own.shownElsewhere.erase(*displayedWorkspace);
    auto const at = std::ranges::find(listed, own.daemon, &DaemonSection::daemon);
    if (at != listed.end())
        *at = std::move(own);
    else
        listed.push_back(std::move(own));
    return listed;
}

/// The window's sections, its own built from what its own link says of its daemon.
/// @param listed The sections the presence lists, in the order the sidebar shows them.
/// @param own The window's own daemon.
/// @param directory Its workspaces, from the window's own link.
/// @param displayedWorkspace The workspace the window displays, if any.
/// @return As the overload above, the own section keeping what the presence lists of the workspaces other
///         windows show.
[[nodiscard]] inline std::vector<DaemonSection> windowSections(std::vector<DaemonSection> listed,
                                                               OwnDaemon const& own,
                                                               WorkspaceDirectory directory,
                                                               std::optional<uint64_t> displayedWorkspace)
{
    auto section = DaemonSection { .daemon = own.daemon,
                                   .instance = own.instance,
                                   .name = own.name,
                                   .locality = own.locality,
                                   .state = own.state,
                                   .reason = {},
                                   .directory = std::move(directory),
                                   .shownElsewhere = {},
                                   .feed = SectionFeed::WindowLink };
    if (auto const at = std::ranges::find(listed, own.daemon, &DaemonSection::daemon); at != listed.end())
        section.shownElsewhere = at->shownElsewhere;
    return windowSections(std::move(listed), std::move(section), displayedWorkspace);
}

/// Why a jump from a sidebar row is not made.
enum class JumpRefusal : uint8_t
{
    /// The row names a run of its daemon that has ended: a restarted daemon may reuse its workspace ids for
    /// other workspaces, so the row is one rebuild stale and names nothing (spec §3).
    StaleRun = 1,
};

/// The control id a jump names its workspace by, read from what the sidebar lists of the daemon.
/// @param instance The run of the daemon the sidebar lists.
/// @param directory Its workspaces.
/// @param target What the row names.
/// @return The workspace's control id; all zero for one the directory does not list, or whose daemon stated
///         none, which leaves the choice to the daemon; StaleRun when @p target names another run.
[[nodiscard]] inline std::expected<std::array<uint8_t, 16>, JumpRefusal> jumpControlIdOf(
    DaemonInstance instance, WorkspaceDirectory const& directory, SidebarTarget const& target)
{
    if (target.instance != instance)
        return std::unexpected(JumpRefusal::StaleRun);
    auto const* const workspace = findWorkspace(directory, target.workspace);
    return workspace != nullptr ? workspace->controlId : std::array<uint8_t, 16> {};
}

} // namespace contour::window
