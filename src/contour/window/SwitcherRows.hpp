// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon and workspace switcher's list, as palette levels (the redesign's boards 1-5): the top level
/// lists the window's daemon's workspaces, the other daemons and the actions; typing lists the matching
/// workspaces of every listed local daemon, best first; a daemon row drills into that daemon's level; the New
/// Daemon prompt is a level of its own; and the workspace chip opens it scoped to the window's daemon. Every
/// row carries the status the sidebar shows (SwitcherStatus). Pure and Qt-free, so every grouping, filtering
/// and naming rule is a row of a table test; the controller only renders it.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SwitcherStatus.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
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
    Hidden = 0,     ///< No window shows it.
    ShownElsewhere, ///< Another window shows it: one here, or another client's.
    ThisWindow,     ///< The window the popup is over shows it.
};

/// @param state A workspace's state.
/// @return What its row says of it: `hidden`, `shown elsewhere`, `this window`.
[[nodiscard]] std::string_view describe(SwitcherWorkspaceState state) noexcept;

/// One workspace of a daemon, as the popup lists it.
struct SwitcherWorkspace
{
    /// What choosing it re-enters a switch with: its control id, which nothing reads again as text.
    vtcontrol::ObjectRef ref {};
    uint32_t position = 0; ///< Its 1-based position among the daemon's workspaces.
    std::string name {};   ///< Its label, else its active tab's title, else "workspace N".
    std::size_t tabs = 0;  ///< How many tabs it holds.
    SwitcherWorkspaceState state = SwitcherWorkspaceState::Hidden; ///< Where it is shown.
    SwitcherStatus status {}; ///< What it asks of the user, its git text, reason and time.
    uint64_t id = 0; ///< The daemon's native id of it, which the sidebar's rows are keyed by; 0 unknown.
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
    Connected,          ///< A window of this application is on it now.
};

/// @param host A saved host's name.
/// @param reached The saved hosts a link was started to, or a listing reached, since the application began.
/// @param shownHere The saved hosts some window of this application is on now.
/// @return How the host's link stands: Connected while a window is on it, else Disconnected once it was
///         reached, else NeverConnected.
[[nodiscard]] inline SwitcherHostLink switcherHostLinkOf(std::string const& host,
                                                         std::set<std::string> const& reached,
                                                         std::span<std::string const> shownHere)
{
    if (std::ranges::find(shownHere, host) != shownHere.end())
        return SwitcherHostLink::Connected;
    return reached.contains(host) ? SwitcherHostLink::Disconnected : SwitcherHostLink::NeverConnected;
}

/// One daemon the popup lists, with what is known of its workspaces.
struct SwitcherDaemon
{
    session::SwitchTarget address {}; ///< The daemon (Label or Host, and its name; Same for this window's
                                      ///< unnameable one); its workspace spec is empty.
    std::optional<std::size_t>
        workspaceCount {}; ///< How many workspaces discovery counted; none when unknown.
    std::string tag {};    ///< `SSH` or `TLS` on a saved host; empty on a local daemon.
    SwitcherListing listing = SwitcherListing::NotListed; ///< How far its workspaces are known.
    std::string failure {};                       ///< Why it could not be listed; empty unless Failed.
    std::vector<SwitcherWorkspace> workspaces {}; ///< Its workspaces, in its order, once Listed.
    /// A saved host's link before its workspaces are listed; NeverConnected on a local daemon, which is
    /// running, not connected to.
    SwitcherHostLink link = SwitcherHostLink::NeverConnected;
};

/// Which level of the popup is shown.
enum class SwitcherLevel : uint8_t
{
    Top = 0,   ///< The window's daemon, the other daemons and the actions; or, scoped, one daemon.
    Daemon,    ///< One daemon's workspaces, drilled into from its row.
    NewDaemon, ///< The label of a new daemon to start.
};

/// Where the popup opened from, which decides where it hangs and what its chip scope offers.
enum class SwitcherOrigin : uint8_t
{
    Centred = 0, ///< ShowSwitcher, NewDaemon or a switch's pick: centred like the palette.
    Chip,        ///< The workspace chip: hanging below it.
};

/// Whether the top level lists every daemon or one daemon's workspaces.
enum class SwitcherScope : uint8_t
{
    Everything = 0, ///< Every daemon, grouped; the ShowSwitcher action, or the chip's dropdown widened.
    OneDaemon,      ///< One daemon's workspaces alone: the chip's dropdown, or the daemon a switch asked to
                    ///< pick a workspace of.
    WindowOnly,     ///< The window has no daemon (an in-process window): its own workspaces alone, and no
                    ///< other daemon is looked for or offered.
};

/// @param scope A top level's scope.
/// @return Whether it lists the workspaces of a single daemon (or of the window alone), not every daemon.
[[nodiscard]] constexpr bool listsOneDaemon(SwitcherScope scope) noexcept
{
    return scope != SwitcherScope::Everything;
}

/// @param scope A top level's scope.
/// @return Whether a pick in it can open a workspace in a new window. A window without a daemon has no daemon
///         to open one of in another window, so the Shift+Return and Open in New Window choices are not
///         offered.
[[nodiscard]] constexpr bool offersNewWindow(SwitcherScope scope) noexcept
{
    return scope != SwitcherScope::WindowOnly;
}

/// Whether looking for other local daemons is still under way.
enum class SwitcherDiscovery : uint8_t
{
    Settled = 0, ///< Discovery answered (or is not needed).
    Running,     ///< Discovery has not answered yet: Other daemons shows a looking row.
};

/// Which group a row is listed under.
enum class SwitcherGroup : uint8_t
{
    None = 0,         ///< No group: the New Daemon level's action.
    WindowDaemon,     ///< The window's daemon's workspaces.
    OtherDaemons,     ///< The other daemons, local ones first, then saved hosts.
    Matches,          ///< What typing found: the matching workspaces of every listed local daemon.
    DaemonWorkspaces, ///< The workspaces of a daemon drilled into, or a pick's.
    Actions,          ///< New Workspace, New Daemon…, All daemons….
};

/// What a row of the popup is.
enum class SwitcherRowKind : uint8_t
{
    Workspace = 0, ///< One of a daemon's workspaces: choosing it switches to it.
    Daemon,        ///< Another daemon: choosing it drills into its level.
    NewWorkspace,  ///< `New Workspace`, or `New Workspace “query”`: creates one and switches to it.
    NewDaemon,     ///< `New Daemon…`: drills into the label prompt.
    AllDaemons,    ///< `All daemons…`: widens the chip's dropdown to every daemon.
    Discovering,   ///< `Looking for other daemons…`, while discovery runs.
    Connecting,    ///< `Connecting to host…`, while a daemon drilled into is being listed.
    LabelAction,   ///< The New Daemon level's one action: start the daemon, or switch to the running one.
    Failure,       ///< Why the daemon drilled into could not be listed; choosing it does nothing.
};

/// What the footer's key hints are about.
enum class SwitcherHintSubject : uint8_t
{
    None = 0,        ///< No row: Escape alone.
    Workspace,       ///< A workspace row.
    Daemon,          ///< A daemon row.
    NewWorkspace,    ///< New Workspace.
    NewDaemon,       ///< New Daemon….
    AllDaemons,      ///< All daemons….
    CreateDaemon,    ///< The New Daemon level's action for an available label.
    SwitchToRunning, ///< The New Daemon level's action for a label that runs already.
};

/// Whether a row of a kind leads into another level (→, and Return).
enum class DrillsIn : uint8_t
{
    No = 0,
    Yes,
};

/// Whether Shift+Return on a row of a kind opens a new window.
enum class OpensInNewWindow : uint8_t
{
    No = 0,
    Yes,
};

/// What a row of a kind is to assistive technology: the first words of its description.
enum class SwitcherRowRole : uint8_t
{
    Workspace = 0, ///< `workspace`.
    Daemon,        ///< `daemon`.
    Action,        ///< `action`.
    Progress,      ///< `in progress`.
    Error,         ///< `error`.
};

/// Whether a row of a kind stands for an answer still awaited: it turns the Working ring.
enum class AwaitsAnswer : uint8_t
{
    No = 0,
    Yes,
};

/// Whether a row of a kind wears the trailing "open in a new window" button while selected or hovered. Only
/// a workspace does (board 6): Shift+Return on a daemon or an action opens a window too, without one.
enum class NewWindowButton : uint8_t
{
    No = 0,
    Yes,
};

/// What a row of one kind does with the keys, and what its footer hints are about.
struct SwitcherRowKindRule
{
    SwitcherRowKind kind = SwitcherRowKind::Workspace;     ///< The kind.
    DrillsIn drillsIn = DrillsIn::No;                      ///< Whether it leads into a level.
    OpensInNewWindow newWindow = OpensInNewWindow::No;     ///< Whether Shift+Return opens a window.
    SwitcherHintSubject hints = SwitcherHintSubject::None; ///< What its footer hints are about.
    SwitcherRowRole role = SwitcherRowRole::Workspace;     ///< What it is to assistive technology.
    /// Whether it waits for an answer. The LabelAction row waits only while its start runs, which the
    /// controller knows: No here.
    AwaitsAnswer awaits = AwaitsAnswer::No;
    NewWindowButton button = NewWindowButton::No; ///< Whether it wears the new-window button.
};

/// One row per SwitcherRowKind, in enumerator order: every per-kind fact the keys, the footer and the
/// delegate read, in one place.
inline constexpr auto SwitcherRowKindRules = std::array {
    SwitcherRowKindRule { .kind = SwitcherRowKind::Workspace,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::Yes,
                          .hints = SwitcherHintSubject::Workspace,
                          .role = SwitcherRowRole::Workspace,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::Yes },
    SwitcherRowKindRule { .kind = SwitcherRowKind::Daemon,
                          .drillsIn = DrillsIn::Yes,
                          .newWindow = OpensInNewWindow::Yes,
                          .hints = SwitcherHintSubject::Daemon,
                          .role = SwitcherRowRole::Daemon,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::NewWorkspace,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::Yes,
                          .hints = SwitcherHintSubject::NewWorkspace,
                          .role = SwitcherRowRole::Action,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::NewDaemon,
                          .drillsIn = DrillsIn::Yes,
                          .newWindow = OpensInNewWindow::No,
                          .hints = SwitcherHintSubject::NewDaemon,
                          .role = SwitcherRowRole::Action,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::AllDaemons,
                          .drillsIn = DrillsIn::Yes,
                          .newWindow = OpensInNewWindow::No,
                          .hints = SwitcherHintSubject::AllDaemons,
                          .role = SwitcherRowRole::Action,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::Discovering,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::No,
                          .hints = SwitcherHintSubject::None,
                          .role = SwitcherRowRole::Progress,
                          .awaits = AwaitsAnswer::Yes,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::Connecting,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::No,
                          .hints = SwitcherHintSubject::None,
                          .role = SwitcherRowRole::Progress,
                          .awaits = AwaitsAnswer::Yes,
                          .button = NewWindowButton::No },
    // Its verdict decides its hints (SwitcherLabelState::subject).
    SwitcherRowKindRule { .kind = SwitcherRowKind::LabelAction,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::Yes,
                          .hints = SwitcherHintSubject::None,
                          .role = SwitcherRowRole::Action,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
    SwitcherRowKindRule { .kind = SwitcherRowKind::Failure,
                          .drillsIn = DrillsIn::No,
                          .newWindow = OpensInNewWindow::No,
                          .hints = SwitcherHintSubject::None,
                          .role = SwitcherRowRole::Error,
                          .awaits = AwaitsAnswer::No,
                          .button = NewWindowButton::No },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SwitcherRowKindRules.size()),
                                  [](std::size_t index) {
                                      return SwitcherRowKindRules[index].kind
                                             == static_cast<SwitcherRowKind>(index);
                                  }),
              "SwitcherRowKindRules lists every SwitcherRowKind once, in enumerator order");

/// @param kind A row kind.
/// @return Its rule.
[[nodiscard]] constexpr SwitcherRowKindRule const& switcherRowKindRule(SwitcherRowKind kind) noexcept
{
    return SwitcherRowKindRules[static_cast<std::size_t>(kind)];
}

/// The pill at the end of a row.
enum class SwitcherPill : uint8_t
{
    None = 0,    ///< No pill.
    Current,     ///< `Current`: the workspace this window shows.
    OtherWindow, ///< `Other window`: a workspace another window shows.
    Daemon,      ///< The daemon a matching workspace is on.
    Transport,   ///< `SSH` or `TLS` on a saved host.
};

/// One row of the popup.
struct SwitcherRow
{
    SwitcherRowKind kind = SwitcherRowKind::Workspace;
    SwitcherGroup group = SwitcherGroup::None; ///< What it is listed under.
    std::string groupTitle {};                 ///< The group's header, on the group's first row only.
    /// Its daemon, an index into SwitcherInputs::daemons; the window's daemon for a row of no daemon's.
    std::size_t daemon = 0;
    std::string title {};             ///< `api-gateway`, `home`, `New Workspace “api”`.
    std::vector<int> titleMatches {}; ///< The UTF-8 byte offsets of the title the filter matched.
    SwitcherStatus status {};         ///< A workspace's status, a daemon's rolled up; idle on other rows.
    SwitcherPill pill = SwitcherPill::None; ///< The pill at its end.
    std::string pillText {};                ///< What the pill says.
    std::size_t tabs = 0;                   ///< A workspace's tab count; 0 on other rows.
    /// Where a workspace is shown; Hidden on other rows.
    SwitcherWorkspaceState state = SwitcherWorkspaceState::Hidden;
    session::SwitchTarget target {}; ///< Where choosing it goes; empty for a row that only navigates.
};

/// What the popup's list is made of.
struct SwitcherInputs
{
    std::span<SwitcherDaemon const> daemons {}; ///< Local daemons, then saved hosts, in display order.
    std::size_t windowDaemon =
        0; ///< The daemon the window the popup is over is on, an index into @c daemons.
    SwitcherDiscovery discovery = SwitcherDiscovery::Settled; ///< Whether other daemons are still looked for.
    /// The chord bound to ShowSwitcher, as the platform spells it: the `All daemons…` row's detail, since
    /// that row widens the dropdown to what the chord opens. Empty while nothing binds it.
    std::string_view showSwitcherShortcut {};
};

/// Where in the popup the user is, and what they typed.
struct SwitcherView
{
    SwitcherLevel level = SwitcherLevel::Top; ///< The level shown.
    /// The daemon of the Daemon level, or of the OneDaemon scope, an index into SwitcherInputs::daemons.
    std::size_t daemon = 0;
    SwitcherOrigin origin = SwitcherOrigin::Centred; ///< Where the popup opened from.
    SwitcherScope scope = SwitcherScope::Everything; ///< Every daemon, or one, at the top level.
    std::string_view filter {};                      ///< What the user typed; the New Daemon level's label.
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
    /// Every daemon, or the window's own alone, listed afresh as it opens: the workspace chip's dropdown asks
    /// which workspace of this window's daemon to show. A pick is always one daemon, whatever this says.
    SwitcherScope scope = SwitcherScope::Everything;
    /// The window's own daemon's workspaces, with the status its sidebar shows (remote::windowWorkspacesOf):
    /// that daemon is not listed over the control API, and the window pushes fresh ones while it is open.
    std::vector<SwitcherWorkspace> windowWorkspaces {};
    /// Where it opened from: centred, or hanging below the workspace chip.
    SwitcherOrigin origin = SwitcherOrigin::Centred;
};

/// What a typed label for a new daemon amounts to.
enum class LabelVerdict : uint8_t
{
    Empty = 0, ///< Nothing typed yet.
    Available, ///< A valid label no running daemon has: a daemon can start under it.
    Running,   ///< A daemon of that label runs already: Return switches to it.
    Invalid,   ///< Not a label a daemon can have (vthost::validateSocketLabel).
};

/// @param label What the user typed.
/// @param runningLabels The labels of the local daemons running now.
/// @return What it amounts to.
[[nodiscard]] LabelVerdict switcherLabelVerdict(std::string_view label,
                                                std::span<std::string const> runningLabels);

/// @param inputs What the list is made of.
/// @return The labels of its local daemons.
[[nodiscard]] std::vector<std::string> switcherRunningLabels(SwitcherInputs const& inputs);

/// @param label A label to act on.
/// @param verdict Its verdict.
/// @return The New Daemon level's action row title: start it (Available), switch to it (Running); empty
///         otherwise.
[[nodiscard]] std::string switcherLabelActionTitle(std::string_view label, LabelVerdict verdict);

/// The popup's rows for where the user is.
///
/// - Top, every daemon, nothing typed: the window's daemon's workspaces under "Workspaces on X", the other
///   daemons under "Other daemons" (a looking row while discovery runs), then New Workspace and New Daemon….
/// - Top, every daemon, typed: the matching workspaces of every listed local daemon (and the window's) under
///   "Workspaces", best first, ties by daemon then position, each with its daemon's pill; the daemons whose
///   name matches under "Other daemons"; then `New Workspace “query”`. A saved host's workspaces are never
///   matched: it is matched by its name alone.
/// - Top, one daemon (the chip's dropdown, a pick), or the Daemon level: that daemon's workspaces (those
///   matching, best first, when typed), kept while a listing refreshes them; a connecting row while it is
///   listed for the first time; a failure row saying why it could not be listed; then its New Workspace, and
///   from the chip All daemons….
/// - NewDaemon: the one action a valid label offers, else nothing.
/// @param inputs What the list is made of.
/// @param view Where the user is and what they typed.
/// @return The rows, in display order.
[[nodiscard]] std::vector<SwitcherRow> switcherRows(SwitcherInputs const& inputs, SwitcherView const& view);

/// @param daemon A daemon.
/// @return Its listed workspaces' status, rolled up (rollUpStatus); idle when none is listed.
[[nodiscard]] SwitcherStatus daemonStatusOf(SwitcherDaemon const& daemon);

/// @param address A daemon.
/// @return How the popup names it: its label or host name, or `this daemon` for an unnameable one.
[[nodiscard]] std::string switcherDaemonName(session::SwitchTarget const& address);

/// What tells one row from every other across a rebuild: its kind and its target. Every row bound to a daemon
/// names that daemon in its target (a workspace, a daemon, a New Workspace, a label action); the rows without
/// a target (New Daemon…, All daemons…, the looking, connecting and failure rows) are one of a kind in a
/// view, so their kind alone tells them apart. A daemon's index is not part of it: discovery reorders the
/// daemons.
struct SwitcherRowIdentity
{
    SwitcherRowKind kind = SwitcherRowKind::Workspace; ///< What the row is.
    session::SwitchTarget target {};                   ///< Where choosing it goes.

    bool operator==(SwitcherRowIdentity const&) const = default;
};

/// @param row A row.
/// @return What tells it apart across a rebuild.
[[nodiscard]] SwitcherRowIdentity switcherRowIdentity(SwitcherRow const& row);

/// @param rows The popup's rows.
/// @param keep The row to keep the selection on.
/// @return The row of the identity @p keep, else the first; 0 for none.
[[nodiscard]] std::size_t switcherSelection(std::span<SwitcherRow const> rows,
                                            std::optional<SwitcherRowIdentity> const& keep);

/// Where the selection starts when the popup opens: the first workspace row this window does not show, so
/// Return on an untouched popup moves the window somewhere; failing that the first row.
/// @param rows The popup's rows.
/// @return The row to select; 0 for none.
[[nodiscard]] std::size_t switcherInitialSelection(std::span<SwitcherRow const> rows);

} // namespace contour::window
