// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Switching a window in place -- or opening a new one -- onto another workspace of its daemon or onto
/// another daemon: resolve first, detach last, and never block the GUI thread.

#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/ObserverPool.hpp>
#include <contour/remote/SwitchHost.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/remote/SwitchingService.hpp>
#include <contour/remote/WorkspaceLister.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <core/Random.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vthost/Transport.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::remote
{

/// What the switcher popup is asked to show: the target daemon's workspaces, for the user to pick one of. The
/// pick re-enters session::WindowSwitching::request() with the same window, daemon and mode and the picked
/// workspace (its control id, or session::NewWorkspace); a cancel changes nothing.
struct SwitchPickRequest
{
    vtworkspace::WorkspaceId window;      ///< The window that asked.
    session::SwitchTarget target;         ///< Where it asked to go, its workspace still to decide.
    session::SwitchMode mode;             ///< Switch it, or open a new window.
    std::vector<WorkspaceRow> workspaces; ///< The daemon's workspaces, in its order.
};

/// Shows the switcher popup for a pick (@see SwitchPickRequest).
/// @return Nothing once shown; why it cannot be.
using SwitchPicker = std::function<std::expected<void, std::string>(SwitchPickRequest)>;

/// Switches windows between daemons and workspaces (spec §Behaviour, Switch and Open).
///
/// Resolve first: the target is resolved to one of the target daemon's workspaces (planSwitch()), or to the
/// user's pick, a window to focus, or nothing to do -- against the directory the window's own connection
/// holds for its own daemon, and against the listing of any other. Detach last: a switch on the window's own
/// daemon has the window's connection select the new workspace, which the window then displays in place; one
/// to another daemon connects a link of its own to it, and only once that connection shows the workspace is
/// the window rebound, which ends the old link. Anything failing before that leaves the window as it was and
/// is said in the window -- as does a target that does not show within the deadline (SwitchDeadline), which
/// leaves the window free for the next switch. A switch that must land by a time (session::SwitchLandBy) is
/// refused when that time passed before it selected or connected anything, and gives up at that time at the
/// latest. GUI thread only; nothing it does waits.
///
/// Through the presence (IObserverPool): a daemon the sidebar lists, connected, is resolved from the
/// directory its observer holds, and a switch to it takes that observer over rather than dialling; the daemon
/// the window leaves keeps the window's connection as its observer while no other window is on it. The user's
/// newer switch, a cancel and the window's close end a switch at any stage (supersessionOf()).
class WindowSwitcher final: public SwitchingService
{
  public:
    /// @param sessions The manager the windows live in. Borrowed, and not used here: it may still be under
    ///        construction, but must outlive every request.
    /// @param host The application. Borrowed; it must outlive the switcher.
    /// @param pool The presence's observers. Borrowed; it must outlive the switcher.
    /// @param lister Lists a daemon's workspaces.
    /// @param picker Shows the switcher popup for a pick.
    /// @param deadline How long a switch waits for its target to show, and what tells it the time is up.
    /// @param random Draws the creation token of a new workspace an adopted observer is promoted to.
    /// Borrowed;
    ///        it must outlive the switcher.
    WindowSwitcher(session::TerminalSessionManager& sessions,
                   ISwitchHost& host,
                   IObserverPool& pool,
                   std::unique_ptr<IWorkspaceLister> lister,
                   SwitchPicker picker,
                   SwitchDeadline deadline,
                   core::IRandom& random);
    ~WindowSwitcher() override;

    WindowSwitcher(WindowSwitcher const&) = delete;
    WindowSwitcher(WindowSwitcher&&) = delete;
    WindowSwitcher& operator=(WindowSwitcher const&) = delete;
    WindowSwitcher& operator=(WindowSwitcher&&) = delete;

    void request(vtworkspace::WorkspaceId window,
                 session::SwitchTarget target,
                 session::SwitchMode mode,
                 session::SwitchLandBy landBy,
                 session::SwitchCompletion done) override;

    /// Ends the switch @p window waits in, if any: whatever it started goes with the window, which a deadline
    /// no longer bounds while it waits for a tab.
    void windowClosed(vtworkspace::WorkspaceId window) override;

    /// Ends the switch of @p mode @p window waits in, if any, as the user cancelled it: whatever it started
    /// is undone, an observer it took over goes back to the presence, and the window shows what it showed.
    void cancel(vtworkspace::WorkspaceId window, session::SwitchMode mode) override;

    /// Reaches the daemon the way the switch would (a host not yet connected is connected to for this),
    /// and marks each row with the window here that shows it.
    void listWorkspaces(vtworkspace::WorkspaceId window,
                        session::SwitchTarget const& daemon,
                        DaemonWorkspacesListed done) override;

    /// Reaches the daemon the way the switch would, has it create the workspace, then switches to it.
    void createWorkspace(vtworkspace::WorkspaceId window,
                         session::SwitchTarget const& daemon,
                         std::string label,
                         session::SwitchMode mode,
                         session::SwitchCompletion done) override;

    [[nodiscard]] std::vector<WorkspaceRow> windowWorkspaceRows(vtworkspace::WorkspaceId window) override;

  private:
    /// Carries out a switch for a window that runs in this process: it shows one of the workspaces it owns,
    /// or makes a new one, named @p label when that is not empty. Nothing else is in reach of such a window:
    /// another daemon, or a new window, is refused.
    /// @param window The window.
    /// @param target What to show; of the same daemon.
    /// @param mode Replace what the window shows, or open a new window.
    /// @param label What to call a new workspace.
    /// @param done Told how it went.
    void switchInProcess(vtworkspace::WorkspaceId window,
                         session::SwitchTarget const& target,
                         session::SwitchMode mode,
                         std::string label,
                         session::SwitchCompletion const& done);

    /// @param window An in-process window.
    /// @return The rows of the workspaces it owns, from its own directory.
    [[nodiscard]] std::vector<WorkspaceRow> inProcessRows(vtworkspace::WorkspaceId window);

    /// The daemon a target names, as seen from the window's own link.
    struct Destination
    {
        SameDaemon sameDaemon = SameDaemon::No;              ///< Whether it is the window's own daemon.
        DaemonAddress address;                               ///< The daemon.
        std::expected<DaemonLinkConfig, std::string> config; ///< What a link to it is built with.
    };

    /// @return Where @p target goes from the window @p own serves.
    [[nodiscard]] Destination destinationOf(DaemonLink& own,
                                            session::SwitchTarget const& target,
                                            vtworkspace::WorkspaceId window);

    /// One request, from its resolution to its outcome.
    struct Request
    {
        vtworkspace::WorkspaceId window;
        session::SwitchTarget target;
        session::SwitchMode mode;
        session::SwitchLandBy landBy; ///< When it must have landed by, if it must.
        session::SwitchCompletion done;
        SameDaemon sameDaemon = SameDaemon::No;
        uint64_t serial = 0;                    ///< Which request of its window this is (@see _busy).
        DaemonAddress address;                  ///< The target daemon.
        std::optional<DaemonLinkConfig> config; ///< What a link to it is built with; none once failed.
    };

    /// @return The link @p window is bound to; null for a window none serves.
    [[nodiscard]] DaemonLink* linkOf(vtworkspace::WorkspaceId window);
    /// @return The window here showing @p daemonWorkspace of the daemon @p address names; none for none.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> shownHere(SameDaemon sameDaemon,
                                                                    DaemonAddress const& address,
                                                                    DaemonLink& own,
                                                                    uint64_t daemonWorkspace);
    /// Decides on @p request against the target daemon's listing.
    void resolveListed(Request request, std::vector<vtcontrol::WorkspaceInfo> const& workspaces);
    /// Decides on @p request against the target daemon's workspaces, @p rows (planSwitch()).
    void resolve(Request request, std::vector<WorkspaceRow> const& rows);
    /// Shows the workspace @p to names: over the window's own connection, or a link of its own.
    void land(Request request, SwitchTo const& to);
    /// What a switch's deadline did when it passed.
    enum class Expiry : uint8_t
    {
        GaveUp = 0, ///< The switch was undone and told it failed.
        Disarmed,   ///< The switch waits on: its target holds no tab, which is for the user to end.
    };

    /// Where a window's switch under way waits: one slot for a switch in place, one for an Open, so that
    /// neither kind ends the other.
    struct SwitchSlot
    {
        uint64_t window = 0;                                    ///< The window's workspace id.
        session::SwitchMode mode = session::SwitchMode::Switch; ///< Which kind of switch.
        auto operator<=>(SwitchSlot const&) const = default;
    };

    /// Every switch mode, each a slot of its own.
    static constexpr auto SwitchModes = std::array { session::SwitchMode::Switch, session::SwitchMode::Open };

    /// @return The slot a switch of @p mode of @p window waits in.
    [[nodiscard]] static constexpr SwitchSlot slotOf(vtworkspace::WorkspaceId window,
                                                     session::SwitchMode mode) noexcept
    {
        return SwitchSlot { .window = window.value, .mode = mode };
    }

    /// A window's switch under way: its request's serial, and how a newer request, a cancel or the window's
    /// close ends it (supersessionOf()).
    struct Busy
    {
        uint64_t serial = 0;
        std::function<Supersession(SupersedeCause)> supersede; ///< Set from the request's start on.
    };

    /// Takes a switch's request out of the callbacks that share it; empty once another took it.
    using TakeRequest = std::function<std::optional<Request>()>;
    /// Ends the switch of @p window over its own connection for @p cause, if supersessionOf() says so at the
    /// stage its link is at.
    /// @param take Takes the switch's request.
    [[nodiscard]] Supersession supersedeOwn(vtworkspace::WorkspaceId window,
                                            SupersedeCause cause,
                                            TakeRequest const& take);
    /// Ends the switch that the link @p madeAs started, for @p cause, under the same rule.
    /// @param take Takes the switch's request.
    [[nodiscard]] Supersession supersedeLinked(DaemonLinkId madeAs,
                                               SupersedeCause cause,
                                               TakeRequest const& take);

    /// Ends the switch under way in @p slot for @p cause, if it ends for it. Nothing while none is under way.
    void supersede(SwitchSlot slot, SupersedeCause cause);
    /// @param link The link a switch selected or connected through, if it is still there.
    /// @return The stage a switch through it is at: waiting for a tab, or connecting.
    [[nodiscard]] static SwitchStage stageOf(DaemonLink const* link);

    /// The presence's section of the daemon @p request goes to, if it lists it, connected.
    [[nodiscard]] std::optional<window::DaemonSection> connectedSectionOf(Request const& request) const;
    /// Builds the link of a switch to another daemon, or of an Open, around the presence's observer of it, if
    /// it has one -- which it has only while no window of this process is on the daemon.
    /// @param request The switch, or the Open.
    /// @param workspace The daemon's workspace to show; 0 for a new one.
    /// @param config What the link is built with, its arrival set.
    /// @return The link; null when the presence has no observer to hand over, or no area is known for the
    ///         window it shows the workspace in.
    [[nodiscard]] DaemonLink* adoptLink(Request const& request, uint64_t workspace, DaemonLinkConfig& config);
    /// @param daemon The daemon a link leaves.
    /// @param handOff The hand-over the connection's return ends; 0 for one the presence never marked.
    /// @return Where its connection goes as it leaves: back to the presence while the switcher lives.
    [[nodiscard]] GiveBack giveBackTo(window::DaemonRef daemon, HandOffId handOff) const;
    /// Shows the row's tab and pane in @p window (WindowController::land()), once it shows the workspace.
    void applyFocus(vtworkspace::WorkspaceId window, session::LandingFocus const& focus);

    /// Lets the switch @p serial in @p slot be ended by @p supersede (@see Busy).
    void onSupersede(SwitchSlot slot, uint64_t serial, std::function<Supersession(SupersedeCause)> supersede);
    /// Ends @p request as cancelled: logged, told to the caller, said nowhere in the window.
    void cancel(Request const& request, std::string const& reason);

    /// Gives the switch @p serial in @p slot up once the deadline passed -- the switch's own, or its landing
    /// time @p landBy when that comes first -- unless it ended before.
    /// @param expire Undoes what the switch started and ends it as failed, unless it waits for a tab in a
    ///        workspace that holds none (expiryOf()); then it says so.
    void armDeadline(SwitchSlot slot,
                     uint64_t serial,
                     session::SwitchLandBy landBy,
                     std::function<Expiry()> expire);

    /// @param wait What the switch waits for as its deadline passes.
    /// @param landBy The time a program that asked for the switch stops waiting, if it did.
    /// @return Disarmed for a switch that waits for a tab -- no deadline bounds that, only the connection
    ///         (DaemonLink::noteConnectionForSwitch()) -- unless a landing time bounds it, which a program
    ///         that stops waiting needs answered; GaveUp otherwise.
    [[nodiscard]] static Expiry expiryOf(SwitchWait wait, session::SwitchLandBy landBy) noexcept;
    /// The cross-daemon switch's last step: the new link's connection shows @p workspace.
    void handOver(Request const& request, DaemonLinkId to);
    /// Ends @p request with its outcome.
    void succeed(Request const& request, vthost::proto::SwitchOutcome outcome);
    /// Ends @p request as failed: said in its window, in Attach to Host's words, and to the caller.
    void fail(Request const& request, std::string const& reason);
    /// Ends @p request as declined by the user: said in its window without alarm, and to the caller. The
    /// window shows what it showed.
    void decline(Request const& request, std::string const& reason);

    /// Says in @p window why the workspace a switch to @p daemon was to land on could not be created, and
    /// tells @p done.
    void failCreate(vtworkspace::WorkspaceId window,
                    session::SwitchTarget const& daemon,
                    std::string const& reason,
                    session::SwitchCompletion const& done);

    session::TerminalSessionManager& _sessions;
    ISwitchHost& _host;
    IObserverPool& _pool;
    std::unique_ptr<IWorkspaceLister> _lister;
    SwitchPicker _picker;
    SwitchDeadline _deadline;
    core::IRandom& _random;
    /// The switches under way, by window and kind, each with its request's serial: a deadline for a request
    /// that ended finds another serial or none.
    std::map<SwitchSlot, Busy> _busy;
    uint64_t _nextSerial = 1; ///< The serial of the next request.
    /// Held by the switcher alone: a link's callback that outlives it finds it gone.
    std::shared_ptr<WindowSwitcher*> _alive;
};

} // namespace contour::remote
