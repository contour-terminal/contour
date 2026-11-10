// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon and workspace switcher popup's list (spec §Switcher popup): the daemons grouped *This
/// computer* and *Saved hosts*, each expanding to its workspaces with their tab count and state, and
/// "+ new workspace on X" -- or, for a pick a switch asked for, the one daemon's workspaces alone. Pure and
/// Qt-free, so every grouping, filtering and naming rule is a row of a table test; the controller only
/// renders it.

#include <contour/session/WindowSwitching.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// Where a workspace is shown, as the window the popup is over sees it.
enum class SwitcherWorkspaceState : uint8_t
{
    Left = 0,       ///< No window shows it, and none switched away from it.
    Hidden,         ///< A window switched away from it, and none shows it since.
    ShownElsewhere, ///< Another window shows it: one here, or another client's.
    ThisWindow,     ///< The window the popup is over shows it.
};

/// @param state A workspace's state.
/// @return What its row says of it: `not shown`, `hidden`, `shown elsewhere`, `this window`.
[[nodiscard]] std::string_view describe(SwitcherWorkspaceState state) noexcept;

/// One workspace of a daemon, as the popup lists it.
struct SwitcherWorkspace
{
    /// What choosing it re-enters a switch with: its control id, which nothing reads again as text.
    vtcontrol::ObjectRef ref {};
    uint32_t position = 0; ///< Its 1-based position among the daemon's workspaces.
    std::string name;      ///< Its label, else its active tab's title, else "workspace N".
    std::size_t tabs = 0;  ///< How many tabs it holds.
    SwitcherWorkspaceState state = SwitcherWorkspaceState::Left; ///< Where it is shown.
};

/// Which group a daemon is listed under.
enum class SwitcherGroup : uint8_t
{
    ThisComputer = 0, ///< A local daemon, by its label.
    SavedHosts,       ///< A saved host.
    Pick,             ///< The one daemon a switch asked the user to pick a workspace of.
};

/// Whether a daemon's workspaces are listed under it.
enum class SwitcherExpansion : uint8_t
{
    Collapsed = 0,
    Expanded,
};

/// How far a daemon's workspaces are known.
enum class SwitcherListing : uint8_t
{
    NotListed = 0, ///< Not asked yet: for a saved host, not yet connected.
    Listing,       ///< Asked; the answer has not come.
    Listed,        ///< Answered.
    Failed,        ///< The daemon could not be listed.
};

/// How a saved host's link stands, as far as this session knows, while its workspaces are not listed.
enum class SwitcherHostLink : uint8_t
{
    NeverConnected = 0, ///< No window or listing has reached it since this application started.
    Disconnected,       ///< Reached before; nothing holds a connection to it now.
};

/// One daemon the popup lists, with what is known of its workspaces.
struct SwitcherDaemon
{
    session::SwitchTarget address; ///< The daemon (Label or Host, and its name; Same for this window's
                                   ///< unnameable one); its workspace spec is empty.
    std::optional<std::size_t> workspaceCount; ///< How many workspaces discovery counted; none when unknown.
    std::string tag;                           ///< `SSH` or `TLS` on a saved host; empty on a local daemon.
    SwitcherExpansion expansion = SwitcherExpansion::Collapsed; ///< Whether its workspaces are shown.
    SwitcherListing listing = SwitcherListing::NotListed;       ///< How far its workspaces are known.
    std::string failure;                       ///< Why it could not be listed; empty unless Failed.
    std::vector<SwitcherWorkspace> workspaces; ///< Its workspaces, in its order, once Listed.
    /// A saved host's link before its workspaces are listed; NeverConnected on a local daemon, which is
    /// running, not connected to.
    SwitcherHostLink link = SwitcherHostLink::NeverConnected;
};

/// Whether the popup lists every daemon or picks a workspace of one.
enum class SwitcherScope : uint8_t
{
    Everything = 0, ///< Every daemon, grouped; the ShowSwitcher action.
    OneDaemon,      ///< The one daemon a switch asked to pick a workspace of; only its workspaces.
};

/// What a row of the popup is.
enum class SwitcherRowKind : uint8_t
{
    Daemon = 0,   ///< A daemon: choosing it switches to it (and asks which workspace when it has several).
    Workspace,    ///< One of a daemon's workspaces.
    NewWorkspace, ///< "+ new workspace on X".
    NewDaemon,    ///< "New daemon…": asks for a label.
};

/// One row of the popup.
struct SwitcherRow
{
    SwitcherRowKind kind = SwitcherRowKind::Daemon;
    SwitcherGroup group = SwitcherGroup::ThisComputer; ///< What it is listed under.
    std::size_t daemon = 0; ///< Its daemon, an index into SwitcherInputs::daemons; 0 for NewDaemon.
    std::string title;      ///< `admin`, `1 "work"`, `+ new workspace on admin`.
    std::string detail;     ///< `running · 2 workspaces · this window`, `4 tabs · hidden`.
    std::string tag;        ///< `SSH` or `TLS` on a saved host's row.
    SwitcherExpansion expansion = SwitcherExpansion::Collapsed; ///< A daemon row's; Collapsed on others.
    session::SwitchTarget target; ///< Where choosing it goes; empty for NewDaemon.
};

/// What the popup's list is made of.
struct SwitcherInputs
{
    std::span<SwitcherDaemon const> daemons;         ///< Local daemons, then saved hosts, in display order.
    session::SwitchTarget windowDaemon;              ///< The daemon the window the popup is over is on.
    SwitcherScope scope = SwitcherScope::Everything; ///< Every daemon, or the one a pick is for.
};

/// What the popup shows, as it opens.
enum class SwitcherPrompt : uint8_t
{
    Browse = 0, ///< The daemons and their workspaces.
    NewDaemon,  ///< The label of a new daemon to start.
};

/// What opens the popup, over which window.
struct SwitcherOpening
{
    vtworkspace::WorkspaceId window;    ///< The window it is over: the one a choice switches.
    session::SwitchTarget windowDaemon; ///< The daemon that window is on.
    /// What Return does: replace the window's workspace, or (a pick an Open asked for) open a new window.
    /// Shift+Return always opens.
    session::SwitchMode mode = session::SwitchMode::Switch;
    SwitcherPrompt prompt = SwitcherPrompt::Browse; ///< What it shows first.
    /// For a pick a switch asked for: the one daemon, its workspaces listed. None for the whole switcher.
    std::optional<SwitcherDaemon> pick;
};

/// The popup's rows: every daemon in its group, its workspaces and "+ new workspace on X" under it when
/// expanded, and "New daemon…" closing *This computer*. A filter keeps a daemon matching it with what is
/// expanded under it, and any listed workspace matching it with its daemon, expanded or not. A pick
/// (SwitcherScope::OneDaemon) lists the one daemon's workspaces and the new-workspace row alone.
/// @param inputs What the list is made of.
/// @param filter What the user typed; every word must occur, ignoring case.
/// @return The rows, in display order.
[[nodiscard]] std::vector<SwitcherRow> switcherRows(SwitcherInputs const& inputs, std::string_view filter);

/// @param address A daemon.
/// @return How the popup names it: its label or host name, or `this daemon` for an unnameable one.
[[nodiscard]] std::string switcherDaemonName(session::SwitchTarget const& address);

/// @param rows The popup's rows.
/// @param keep A target to keep the selection on.
/// @return The row choosing @p keep goes to, else the first; 0 for none.
[[nodiscard]] std::size_t switcherSelection(std::span<SwitcherRow const> rows,
                                            std::optional<session::SwitchTarget> const& keep);

/// Where the selection starts when the popup opens: the first daemon row that is not the window's own, so
/// Return on an untouched popup moves the window somewhere (never to "New daemon…", which starts one). The
/// window's own daemon row when it is the only daemon, else the first row.
/// @param rows The popup's rows.
/// @param windowDaemon The daemon the window the popup is over is on.
/// @return The row to select; 0 for none.
[[nodiscard]] std::size_t switcherInitialSelection(std::span<SwitcherRow const> rows,
                                                   session::SwitchTarget const& windowDaemon);

} // namespace contour::window
