// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a window switch goes, decided before anything is touched (spec §Behaviour, Switch steps 1 and 2):
/// which workspace of the target daemon, or whether to ask the user, focus another window or do nothing.
/// Pure, so every rule is a row of a table test.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SwitcherRows.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <cstddef>
#include <cstdint>
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
    /// Label or Host; Same for a daemon no switch can name (`--connect-tcp`).
    vthost::proto::SwitchDaemonKind kind {};
    std::string name; ///< The label or the host's name; empty for Same.
    bool operator==(DaemonAddress const&) const = default;
};

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
/// out; each carries a reference only this process uses, as a directory carries no control ids (@see
/// namesControlId()).
/// @param directory Every workspace of the daemon, in its order.
/// @param shownHere Which window here shows each.
/// @return The rows, in the daemon's order.
[[nodiscard]] std::vector<WorkspaceRow> workspaceRowsOf(window::WorkspaceDirectory const& directory,
                                                        ShownHere const& shownHere);

/// @param workspace What a switch names.
/// @return Whether it names the workspace by its control id, which only the daemon's listing resolves.
[[nodiscard]] bool namesControlId(session::WorkspaceTarget const& workspace);

/// The switcher popup's rows of a daemon's workspaces (@see window::switcherRows).
/// @param rows The daemon's workspaces, as workspaceRowsOf() made them.
/// @param window The window the popup is over: the one whose workspace is marked as this window's.
/// @return One per row, in their order: its control id to choose it by, and where it is shown.
[[nodiscard]] std::vector<window::SwitcherWorkspace> switcherWorkspacesOf(std::span<WorkspaceRow const> rows,
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
