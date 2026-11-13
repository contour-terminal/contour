// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a window switch goes, decided before anything is touched (spec §Behaviour, Switch steps 1 and 2):
/// which workspace of the target daemon, or whether to ask the user, focus another window or do nothing.
/// Pure, so every rule is a row of a table test.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/SwitcherRows.hpp>
#include <contour/window/SwitcherStatus.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vthost/WorkspaceVisibility.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::remote
{

/// A daemon as a switch names it: a local daemon by its socket label, or a saved host by its name.
struct DaemonAddress
{
    /// Label or Host; Same for a daemon no switch can name (`--connect-tcp`, a socket outside the label
    /// directory).
    vthost::proto::SwitchDaemonKind kind {};
    std::string name; ///< The label or the host's name; empty for Same.
    bool operator==(DaemonAddress const&) const = default;
};

/// The address of the local daemon a launch attaches to, named the way discovery names it: by the file name
/// of a socket in the label directory. A launch whose resolved control socket is anywhere else (`--socket`,
/// `$CONTOUR_MUX`) reaches a daemon no label names, so it is the one no switch can name (Same) -- never
/// "default", which would make an unrelated `default` daemon look like this window's.
/// @param controlPath The control socket the launch resolved.
/// @param labelDirectory The directory discovery lists the labelled daemons' sockets from.
/// @return The Label address of the socket's file name when it sits in @p labelDirectory, else Same.
[[nodiscard]] inline DaemonAddress launchDaemonAddress(std::filesystem::path const& controlPath,
                                                       std::filesystem::path const& labelDirectory)
{
    auto const normal = controlPath.lexically_normal();
    if (normal.parent_path() == labelDirectory.lexically_normal())
        return DaemonAddress { .kind = vthost::proto::SwitchDaemonKind::Label,
                               .name = normal.filename().string() };
    return DaemonAddress { .kind = vthost::proto::SwitchDaemonKind::Same, .name = {} };
}

/// @param target Where a switch goes.
/// @return The daemon it names.
[[nodiscard]] inline DaemonAddress daemonAddressOf(session::SwitchTarget const& target)
{
    return DaemonAddress { .kind = target.kind, .name = target.daemon };
}

/// @param daemon A daemon.
/// @param workspace Which of its workspaces; none to decide.
/// @return A switch to it.
[[nodiscard]] inline session::SwitchTarget switchTargetOf(DaemonAddress const& daemon,
                                                          session::WorkspaceTarget workspace = {})
{
    return session::SwitchTarget { .kind = daemon.kind,
                                   .daemon = daemon.name,
                                   .workspace = std::move(workspace) };
}

/// The workspace spec that asks for a new workspace on the target daemon (@see session::NewWorkspaceSpec).
inline constexpr auto NewWorkspaceSpec = session::NewWorkspaceSpec;

/// One workspace of the target daemon, as a switch decides on it.
struct WorkspaceRow
{
    uint64_t id = 0;             ///< The daemon's native id of it, which a connection follows it by.
    vtcontrol::ObjectRef ref {}; ///< Its control id, `workspace:<uuid>`.
    std::string label;           ///< Its label; empty for none.
    uint32_t position = 0;       ///< Its 1-based position among the daemon's workspaces.
    std::string name;            ///< How it is shown: @see workspaceDisplayName().
    vthost::WorkspaceVisibility visibility = vthost::WorkspaceVisibility::Hidden; ///< Shown or hidden.
    std::size_t tabs = 0;                                                         ///< How many tabs it holds.
    /// The window of this process that shows it; none when no window here does.
    std::optional<vtworkspace::WorkspaceId> shownByWindowHere;
    /// What it asks of the user, as another daemon's listing states it (window::switcherStatusOf()); idle for
    /// a row made from the window's own directory, whose status is the sidebar's (windowWorkspacesOf()).
    window::SwitcherStatus status {};
};

/// Raise and focus the window of this process that already shows the workspace.
struct SwitchFocus
{
    vtworkspace::WorkspaceId window; ///< The window to focus.
};

/// Ask the user which of the daemon's workspaces (the switcher popup, scoped to the daemon).
struct SwitchPick
{
};

/// Show this workspace.
struct SwitchTo
{
    uint64_t workspace = 0; ///< The daemon's native id; 0 for a new workspace.
};

/// The window shows the workspace already: nothing to do.
struct SwitchNoop
{
};

/// The target names no workspace of the daemon, or several.
struct SwitchUnresolved
{
    std::string reason; ///< Why, for the user.
};

/// What a switch does, once resolved.
using SwitchDecision = std::variant<SwitchFocus, SwitchPick, SwitchTo, SwitchNoop, SwitchUnresolved>;

/// Whether the target daemon is the one the asking window is on.
enum class SameDaemon : uint8_t
{
    No = 0,
    Yes,
};

/// Resolves a switch target to what the switch does (spec §Behaviour, steps 1-2): a named workspace is that
/// one (session::NewWorkspace a new one; one named by position or control id by that alone, never read as a
/// label); none named, a daemon holding several is the user's to pick, one holding
/// exactly one that one, and one holding none a new workspace. A resolved workspace this window shows is
/// nothing to do, and one another window here shows is that window to focus.
/// @param target Where to go; only its workspace is read.
/// @param daemonWorkspaces The target daemon's workspaces, in its order.
/// @param currentWorkspace The workspace the asking window shows on its daemon; none for none.
/// @param sameDaemon Whether the target daemon is the asking window's: only then is @p currentWorkspace one
///        of @p daemonWorkspaces.
/// @return What to do.
[[nodiscard]] SwitchDecision planSwitch(session::SwitchTarget const& target,
                                        std::span<WorkspaceRow const> daemonWorkspaces,
                                        std::optional<uint64_t> currentWorkspace,
                                        SameDaemon sameDaemon);

/// The window of this process showing a daemon workspace, by the daemon's native id of it; none for none.
using ShownHere = std::function<std::optional<vtworkspace::WorkspaceId>(uint64_t)>;

/// The rows a switch decides on, from what the daemon's `workspace.list` answered. A workspace whose native
/// id does not read as one (a daemon that predates it) is left out, as no connection could follow it; so is
/// one holding no tab, which has nothing for a window to show.
/// @param workspaces The daemon's workspaces, in its order.
/// @param shownHere Which window here shows each.
/// @return The rows, in the daemon's order.
[[nodiscard]] std::vector<WorkspaceRow> workspaceRowsOf(std::span<vtcontrol::WorkspaceInfo const> workspaces,
                                                        ShownHere const& shownHere);

/// The rows a switch on the window's own daemon decides on, from the directory its connection holds: no
/// listing of the daemon is asked for. A workspace holding no tab is left out, as workspaceRowsOf() leaves it
/// out. Each carries the control id the daemon stated for it, or, for one it stated none for, a reference
/// only this process uses (@see resolvesFromDirectory()).
/// @param directory Every workspace of the daemon, in its order.
/// @param shownHere Which window here shows each.
/// @return The rows, in the daemon's order.
[[nodiscard]] std::vector<WorkspaceRow> workspaceRowsOf(window::WorkspaceDirectory const& directory,
                                                        ShownHere const& shownHere);

/// The rows of a window that runs in this process: the workspaces it owns, none of them carrying a control
/// id (so each is named by an id made of its own, which only this process reads), and only the one the
/// window shows counted as shown.
/// @param directory What the window lists, as its directory source gives it; none lists nothing.
/// @param window The workspace the window shows.
/// @return The rows, in the directory's order.
[[nodiscard]] std::vector<WorkspaceRow> inProcessWorkspaceRowsOf(
    std::optional<window::WorkspaceDirectory> const& directory, vtworkspace::WorkspaceId window);

/// @param workspace What a switch names.
/// @return Whether it names the workspace by its control id, which only a listing, or a directory carrying
///         control ids, resolves.
[[nodiscard]] bool namesControlId(session::WorkspaceTarget const& workspace);

/// @param workspace What a switch on the window's own daemon names.
/// @param directory The directory the window's connection holds.
/// @return Whether @p directory resolves it without a listing of the daemon: always, unless it names a
///         control id (or the end of one) that no workspace of the directory holding a tab carries -- a
///         reconnect's empty directory, a workspace created before its batch settled, a daemon that states
///         no control ids.
[[nodiscard]] bool resolvesFromDirectory(session::WorkspaceTarget const& workspace,
                                         window::WorkspaceDirectory const& directory);

/// The switcher popup's rows of a daemon's workspaces (@see window::switcherRows).
/// @param rows The daemon's workspaces, as workspaceRowsOf() made them.
/// @param window The window the popup is over: the one whose workspace is marked as this window's.
/// @return One per row, in their order: its control id to choose it by, and where it is shown.
[[nodiscard]] std::vector<window::SwitcherWorkspace> switcherWorkspacesOf(std::span<WorkspaceRow const> rows,
                                                                          vtworkspace::WorkspaceId window);

/// The switcher popup's rows of the window's own daemon: switcherWorkspacesOf() of @p rows, each with the
/// status the window's sidebar shows for it (the workspace row of the same daemon id), so a workspace looks
/// the same in the sidebar and the switcher; idle for one the sidebar does not list yet.
/// @param rows The daemon's workspaces, as workspaceRowsOf() made them from the window's directory.
/// @param sidebarWorkspaces The window's sidebar's workspace rows (SidebarListing::workspaces).
/// @param window The window the popup is over.
/// @return One per row, in their order.
[[nodiscard]] std::vector<window::SwitcherWorkspace> windowWorkspacesOf(
    std::span<WorkspaceRow const> rows,
    std::span<window::SidebarRow const> sidebarWorkspaces,
    vtworkspace::WorkspaceId window);

/// How a workspace is named to the user: its label, else its active tab's title, else "workspace N".
/// @param label The workspace's label; empty for none.
/// @param activeTabTitle Its active tab's title; empty for none.
/// @param position Its 1-based position among the daemon's workspaces.
/// @return The name.
[[nodiscard]] std::string workspaceDisplayName(std::string_view label,
                                               std::string_view activeTabTitle,
                                               uint32_t position);

} // namespace contour::remote
