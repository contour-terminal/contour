// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One daemon connection and the windows it serves.

#include <contour/platform/QtInvoke.hpp>
#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/session/RestoreSummaryPolicy.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/WindowBinding.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonDriftModel.hpp>

#include <QtCore/QObject>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace vtextension
{
class IGrantStore;
}

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::window
{
class WindowController;
}

namespace contour::remote
{

/// Whether a freshly spawned window was taken by a link to show one of its daemon workspaces.
enum class WindowAdoption : uint8_t
{
    Declined = 0, ///< Not the link's: the window creates its own first tab.
    Adopted,      ///< The link fills it from a daemon workspace: it must create no tab of its own.
};

/// Told once, on the GUI thread, how a switch's connection came to show its workspace: the daemon's id of the
/// workspace it shows now, or why it does not.
using SwitchLanded = std::function<void(std::expected<uint64_t, std::string>)>;

/// A link a window switch started (WindowSwitcher), and what it does once its connection shows the workspace
/// its intent names (a new one for proto::AttachIntentKind::NewWorkspace).
struct SwitchArrival
{
    /// Switch: it opens no window, and is handed the window that asked (DaemonLink::adoptWindow()); Open: it
    /// opens a window of its own for the workspace.
    session::SwitchMode mode = session::SwitchMode::Switch;
    SwitchLanded landed; ///< Told once: the workspace is shown (Open) or ready to be (Switch), or it failed.
    /// How the workspace is let go of when the switch is abandoned: as it was before (@see undoReleaseFor()).
    vthost::proto::WorkspaceRelease undo = vthost::proto::WorkspaceRelease::Closed;
};

/// Everything a DaemonLink is built with.
struct DaemonLinkConfig
{
    /// What its controller is built with.
    NativeController::Config controller;
    /// The remote host, as `contour client --host` named it; empty for a local daemon and `--connect-tcp`.
    std::string hostLabel;
    /// Whether the daemon is this machine's.
    window::DaemonLocality locality = window::DaemonLocality::Remote;
    /// Whether this client restarts and upgrades it from the drift badge: the launch's daemon only, which is
    /// local; a local daemon a switch reached is offered nothing.
    window::DaemonUpkeep upkeep = window::DaemonUpkeep::None;
    /// This client's release, which the daemon's is judged against for the drift badge.
    std::string clientRelease;
    /// The link this one takes its workspace over from, which @c controller's intent names; none for the
    /// launch's link, which adopts the boot window. A link taking one over opens its own window once its
    /// connection shows the workspace, and reports the outcome (IDaemonLinkHost::handOverSettled).
    std::optional<HandOver> handOverFrom = std::nullopt;
    /// The daemon as a window switch names it: its local label, or its saved host; Same for one no switch can
    /// name (`--connect-tcp`).
    DaemonAddress address {};
    /// What a link a window switch started does once its connection shows its workspace; none for every
    /// other link.
    std::optional<SwitchArrival> arrival = std::nullopt;
};

/// The launch's link: it adopts the boot window, and takes no workspace over.
struct BootRole
{
};

/// A link that takes a further workspace over from another link (DaemonLinkConfig::handOverFrom).
struct HandOverRole
{
    HandOver from; ///< The link it takes the workspace over from, and the workspace.
    /// Whether how it went has been told (@see IDaemonLinkHost::handOverSettled).
    enum class Report : uint8_t
    {
        Due = 0,
        Told,
    };
    Report report = Report::Due;
};

/// Why a link was started, which decides what it does with the windows it gets: the launch's link, one taking
/// a workspace over, or one a window switch started (SwitchArrival, whose landed callback is emptied once
/// told).
using LinkRole = std::variant<BootRole, HandOverRole, SwitchArrival>;

/// One daemon connection and the windows it serves.
///
/// Owns the NativeController and the attach bookkeeping: which daemon workspace its OS window shows, the
/// boot window's adoption, the window being spawned, the drift badge and the answers to the daemon's
/// extension questions. One link serves one window: a further workspace its connection is pushed -- a launch
/// brings back several, a new window asks the daemon for one -- is handed to a link of its own
/// (IDaemonLinkHost::startHandOverLink), and let go here only once that link's connection shows it, so the
/// workspace is followed throughout. Only a daemon that cannot let a connection stop showing a workspace
/// (Capability::WorkspaceFollow), or a hand-over that failed, has one link serve several windows. GUI thread
/// only, apart from what the controller does on its own reactor.
class DaemonLink final: public session::WindowBinding
{
  public:
    /// Names a link among the application's.
    using Id = DaemonLinkId;

    /// Builds the link and its controller, and starts connecting: wires the controller's signals to the
    /// link's windows, installs the attach-window binder on the manager (removed again as the link stops or
    /// gives up), and starts the connect on the controller's reactor. Nothing of it can fail here; a connect
    /// that fails is told by awaitAttached().
    /// @param id The link's name among the application's.
    /// @param config What it connects with.
    /// @param host The application. Borrowed; it must outlive the link.
    /// @param sessions The manager the link's windows live in. Borrowed; the link must not touch it once it
    ///        is gone, so stop() the link before the manager is destroyed.
    DaemonLink(Id id,
               DaemonLinkConfig config,
               IDaemonLinkHost& host,
               session::TerminalSessionManager& sessions);
    /// Releases the windows it serves (session::TerminalSessionManager::releaseBinding) unless it stopped
    /// already: a manager outliving the link must not keep naming it. The manager must still exist then.
    ~DaemonLink() override;

    DaemonLink(DaemonLink const&) = delete;
    DaemonLink& operator=(DaemonLink const&) = delete;
    DaemonLink(DaemonLink&&) = delete;
    DaemonLink& operator=(DaemonLink&&) = delete;

    /// @return The link's name among the application's.
    [[nodiscard]] Id id() const noexcept { return _id; }

    /// @return The controller, until the attach was given up (awaitAttached()); null after.
    [[nodiscard]] NativeController* liveController() noexcept { return _controller.get(); }

    /// @return Whether awaitAttached() succeeded: the attach settled connected, was not given up, and the
    ///         link has not stopped since.
    [[nodiscard]] bool isAttached() const noexcept { return _state == AttachState::Attached; }

    /// @return Whether the link has a live controller to act on: not given up, and not stopped.
    [[nodiscard]] bool isLive() const noexcept { return _controller != nullptr && !isStopped(); }

    /// @return What its reconnect follows when none of the workspaces it resumes survived -- after a restart
    ///         of its daemon, whose ids name other workspaces then.
    [[nodiscard]] NativeController::ResumeFallback resumeFallback() const;

    /// Makes this link the one of its daemon's whose reconnect follows what a launch would when none of its
    /// workspaces survived (NativeController::resumeAsLaunch). No-op once given up or stopped.
    void resumeAsLaunch();

    /// @return Whether the link stopped because its last window went (@see lastWindowReleased), its
    ///         controller has wound down and binds no pane any more: nothing needs it, and it may be
    ///         destroyed.
    [[nodiscard]] bool isFinished() const;

    /// @return The controller as the factory of the link's sessions; the given-up one after awaitAttached()
    ///         gave up, which creates nothing.
    [[nodiscard]] session::SessionFactory& sessions() noexcept override;

    /// @return The remote host the link is attached to; empty for a local daemon, `--connect-tcp`, until
    ///         awaitAttached() succeeded, and once the link stopped.
    [[nodiscard]] std::string_view hostLabel() const noexcept override;
    [[nodiscard]] std::string_view chromeLabel() const noexcept override;
    [[nodiscard]] std::optional<session::SwitchTarget> daemonTarget() const override;
    [[nodiscard]] window::DaemonLocality daemonLocality() const noexcept override { return _locality; }

    /// The last window the link served went (closed, or bound elsewhere): nothing is left for the connection
    /// to show. The link stops -- its controller from the event loop, not inside the window close that may
    /// have called this, as its detach can take up to DefaultDetachBound -- and tells the host it ended
    /// (IDaemonLinkHost::linkEnded) -- again as each
    /// pane of its closed windows lets its controller go. Its daemon workspaces' mappings are kept meanwhile,
    /// so that a window its user closed does not open again. No-op once stopped.
    void lastWindowReleased() override;

    /// One of its windows closed: the daemon workspace it showed is released
    /// (proto::WorkspaceRelease::Closed), and stays left for a later launch to bring back. A workspace a move
    /// emptied is kept followed, as the daemon may fill it again. No-op once given up or stopped, and against
    /// a daemon without Capability::WorkspaceFollow.
    /// @param window The closed window.
    void windowClosed(vtworkspace::WorkspaceId window) override;

    /// A link started for one of this link's workspaces reports how the hand-over went: settled, the
    /// workspace is released here (Closed: the other link shows it now); failed, it stays here and gets an OS
    /// window of this link's, with a notice saying why. No-op once stopped.
    /// @param workspace The daemon workspace handed over.
    /// @param outcome How it went.
    /// @param reason Why it failed; empty when it settled.
    void completeHandOver(uint64_t workspace, HandOverOutcome outcome, std::string const& reason);

    /// @return The daemon as a window switch names it (DaemonLinkConfig::address).
    [[nodiscard]] DaemonAddress const& address() const noexcept { return _handOverTemplate.address; }

    /// @return What a further link to this link's daemon is built with: this one's configuration, taking over
    ///         nothing, started by no switch and stating no working directory. Its intent is the caller's to
    ///         set.
    [[nodiscard]] DaemonLinkConfig const& siblingConfig() const noexcept { return _handOverTemplate; }

    /// @param window A window of the manager.
    /// @return The daemon workspace @p window shows through this link; none for a window it does not serve.
    [[nodiscard]] std::optional<uint64_t> workspaceOf(vtworkspace::WorkspaceId window) const;

    /// @param daemonWorkspace A daemon workspace.
    /// @return The window that shows it through this link; none when no window does.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> windowShowing(uint64_t daemonWorkspace) const;

    /// Switches @p window to another workspace of this link's daemon over the window's own connection,
    /// resolve first and detach last: the connection follows @p workspace as well (NativeController::follow),
    /// and only once its first layout is here is the window's workspace released
    /// (proto::WorkspaceRelease::SwitchedAway) and the window brought to the new one. A connection lost, or
    /// the window closed, before that releases the followed workspace again (@p undo), and the window stays
    /// as it was.
    /// @param window A window this link serves.
    /// @param workspace The daemon workspace to show; 0 for a new one.
    /// @param undo How @p workspace is released when the switch does not land, so it is as it was before.
    /// @param landed Told once: the workspace the window shows now, or why it still shows the old one.
    /// @return Nothing once the follow was asked; why not otherwise (then @p landed is not told).
    [[nodiscard]] std::expected<void, std::string> switchWindow(vtworkspace::WorkspaceId window,
                                                                uint64_t workspace,
                                                                vthost::proto::WorkspaceRelease undo,
                                                                SwitchLanded landed);

    /// A switch of @p window that switchWindow() started and that is not wanted any more (it took too long):
    /// undone as a lost connection undoes it -- the followed workspace is released as it was before
    /// (switchWindow()'s undo), the window stays as it was, and the switch's landed callback is
    /// told @p reason. A workspace the daemon was asked to make and pushes later is hidden, and no window
    /// shows it. No-op when no switch of @p window is under way.
    /// @param window The window switching.
    /// @param reason Why it is given up.
    void cancelSwitch(vtworkspace::WorkspaceId window, std::string const& reason);

    /// The window that showed one of this link's workspaces switches to another daemon: the workspace is
    /// released (proto::WorkspaceRelease::SwitchedAway), and stays in its daemon as hidden. The window is the
    /// other link's to bind next (adoptWindow()), which ends this one when it was its last. No-op once given
    /// up or stopped, and for a window that shows nothing of this link's.
    /// @param window The window switching away.
    void switchedAway(vtworkspace::WorkspaceId window);

    /// A link a switch started (SwitchArrival, mode Switch) whose connection shows its workspace takes the
    /// window that asked: the window is bound here and brought to the workspace, and every pane it showed
    /// before is detached (session::SessionEnd::Detach), its session left running in its daemon.
    /// @param window The window to show the workspace in.
    /// @return Nothing once the window shows it; why not when the connection shows no workspace yet, or one
    ///         holding no tab, or the window is gone (then nothing changed).
    [[nodiscard]] std::expected<void, std::string> adoptWindow(vtworkspace::WorkspaceId window);

    /// @param window The window to show the workspace in.
    /// @return Whether adoptWindow() would take @p window now: nothing when it would, else why not.
    [[nodiscard]] std::expected<void, std::string> canAdoptWindow(vtworkspace::WorkspaceId window) const;

    /// A link a switch started that is not wanted any more: the workspace its connection shows is released
    /// as it was before (SwitchArrival::undo), and the link ends.
    void abandon();

    /// Asks the daemon for a new workspace, whose layout push then opens the window showing it -- while the
    /// link still boots only: its first layout is pending, so a window opened meanwhile would be claimed as
    /// one more boot window and stay empty. A link serving windows is asked through them instead
    /// (session::TerminalSessionManager::requestRemoteWorkspace).
    /// @param spawn What the new workspace's first session runs.
    /// @return Routed when the daemon took the request; Local when the caller is to open a window itself.
    [[nodiscard]] session::RemoteRouting requestBootWorkspace(session::RemoteSpawn const& spawn);

    /// @return What the link's drift badge shows.
    [[nodiscard]] window::DaemonDriftModel& drift() noexcept { return _drift; }

    /// @return What the drift badge of each window it serves shows: its own drift.
    [[nodiscard]] window::DaemonDriftModel* daemonDrift() noexcept override { return &_drift; }
    /// Upgrades the daemon in place when window::upgradeVerdictFor() allows it for this link's daemon.
    [[nodiscard]] std::expected<void, std::string> upgradeDaemonOnCommand() override;

    /// Waits for the connect the constructor began, with what remains of @p budget; gives the controller up
    /// when it does not connect in time (@see awaitConnectedOrAbandon()), and with it the attach-window
    /// binder.
    /// @param budget How long the connect may take since the link was built.
    /// @return Nothing once attached; why not otherwise.
    [[nodiscard]] std::expected<void, std::string> awaitAttached(std::chrono::milliseconds budget);

    /// Shows the drift badge when the daemon runs another release than this client, and wires its actions to
    /// the host's upgrade and restart, for a local daemon. Follows the daemon through an upgrade: the badge
    /// reads "Upgrading daemon…" while the connection is held, and shows the drift anew once it is back.
    /// No-op once given up or stopped.
    void offerDaemonRestartIfDrifted();

    /// Ends the connection ahead of a restart of its daemon: every pane of its windows notes the transport
    /// lost, then the controller stops, so that a reconnect does not start a daemon of its own. No-op once
    /// given up or stopped.
    void stopForDaemonRestart();

    /// Stops the controller, and serves no window any more: what they make from then on is local
    /// (@see session::TerminalSessionManager::releaseBinding). A signal of the controller still queued is
    /// ignored from then on. Idempotent; stops the controller of a link that ended too, whose stop
    /// lastWindowReleased() only posted.
    void stop();

    /// Has the controller state the reloaded configuration's session settings, to the daemon and in the hello
    /// of every later connection (NativeController::restateSessionSettings). No-op once given up or stopped.
    void onConfigurationReloaded();

    /// Brings the local OS windows in line with the daemon's workspaces: the primary daemon workspace maps
    /// onto the boot window, every other one holding a tab gets an OS window of its own (openWindowFor()),
    /// and each window's tab/pane tree is reconciled. Idempotent; re-run on every layout change. No-op once
    /// given up or stopped.
    void reconcile();

    /// What every layout of the daemon brings, and the GUI's boot once: reconcile(), then a restore summary
    /// no window took is forgotten, once no window of the layout is still to be bound. No-op once given up or
    /// stopped.
    void followLayout();

    /// Called from a freshly spawned window's Main.qml (through the manager's attach-window binder): binds it
    /// to the daemon workspace staged for it, or, while the boot window is still to adopt one, to the
    /// primary daemon workspace. Every window it is called for is served by the link from then on
    /// (session::TerminalSessionManager::bindWindow), adopted or not: one link serves every window of the
    /// process it attached. No-op once given up or stopped.
    /// @param window The freshly created controller of the spawned OS window.
    /// @return Whether the link adopted the window.
    [[nodiscard]] WindowAdoption bindPendingWindow(window::WindowController& window);

    /// Records the daemon-workspace → OS-window mapping and reconciles that daemon workspace into it at once:
    /// the one place the "a mapping always implies a reconcile" invariant lives.
    /// @param daemonWorkspace The daemon workspace id.
    /// @param local The local workspace of the OS window that mirrors it.
    void bindDaemonWorkspace(uint64_t daemonWorkspace, vtworkspace::WorkspaceId local);

  private:
    /// Forgets a restore summary no window took, once a layout has been reconciled and no window of it is
    /// still to be bound: a toast about a restore must not surface in a window opened later.
    void expireRestoreSummaryAfterLayout();

    /// Whether the attach is still to settle, succeeded, or the link stopped. One given up stays Connecting:
    /// that it was given up is that its live controller is gone (@see isLive()).
    enum class AttachState : uint8_t
    {
        Connecting = 0,
        Attached,
        Stopped, ///< stop() ran.
        Ended,   ///< Stopped, because the last window it served went; asks the host to be destroyed.
    };

    /// @return Whether the link stopped, ended or not: what its controller still signals is ignored.
    [[nodiscard]] bool isStopped() const noexcept
    {
        return _state == AttachState::Stopped || _state == AttachState::Ended;
    }

    /// @return The live controller, else the given-up one: the one the link's ptys were made by.
    [[nodiscard]] NativeController& anyController() const noexcept
    {
        return _controller ? *_controller : *_givenUp;
    }

    /// @return Whether the live or the given-up controller still binds a pane (NativeController::bindsPanes).
    [[nodiscard]] bool bindsPanes() const;

    /// The constructor's wiring and connect start (@see DaemonLink()).
    void connectController();
    /// Makes this link the one a window spawned now asks (TerminalSessionManager::setAttachWindowBinder).
    void installAttachWindowBinder();
    /// Opens an OS window for @p daemonWorkspace, which claims it as it loads (bindPendingWindow()).
    /// @param daemonWorkspace The daemon workspace to show.
    /// @param sessions The daemon sessions it shows: a window a move of this process asked for opens where
    ///        the pane was dropped.
    void spawnWindowFor(uint64_t daemonWorkspace, std::vector<uint64_t> const& sessions);
    /// Places a further daemon workspace: hands it to a link of its own, or, where that is not to be,
    /// opens a window of this link's for it.
    /// @param daemonWorkspace The daemon workspace no window of this link shows.
    /// @param sessions What it shows; @see spawnWindowFor().
    /// @param replacementsDue How many further workspaces, while windows the daemon lost are closed, are
    /// still
    ///        to open in this link in their place rather than on a link of their own (@see
    ///        closeLostWindows()); one is counted off for each opened so.
    void placeFurtherWorkspace(uint64_t daemonWorkspace,
                               std::vector<uint64_t> const& sessions,
                               std::size_t& replacementsDue);
    /// reconcile(), where @p replacementsDue windows the daemon lost are to be replaced in this link
    /// (@see placeFurtherWorkspace()).
    void reconcileReplacing(std::size_t replacementsDue);
    /// A link taking a workspace over: tells the host how it went, once.
    void reportHandOver(HandOverOutcome outcome, std::string const& reason);
    /// A link a switch started: tells the switch how its connection came to show its workspace, once.
    void reportArrival(std::expected<uint64_t, std::string> landed);
    /// @return Whether this is the launch's link, which adopts the boot window: it takes no workspace over
    ///         and was started by no switch.
    [[nodiscard]] bool isBootLink() const noexcept { return std::holds_alternative<BootRole>(_role); }
    /// @return What a link taking a workspace over took over; null for every other link.
    [[nodiscard]] HandOverRole* handOverRole() noexcept { return std::get_if<HandOverRole>(&_role); }
    /// @return What a link a switch started is to do; null for every other link.
    [[nodiscard]] SwitchArrival* arrival() noexcept { return std::get_if<SwitchArrival>(&_role); }
    /// @return Whether a link a switch started is still to tell the switch how it arrived.
    [[nodiscard]] bool arrivalDue() noexcept { return arrival() != nullptr && arrival()->landed; }
    /// @return The daemon workspace a link a switch started shows: the one its intent names, else the first
    ///         its connection was pushed; none before that.
    [[nodiscard]] std::optional<uint64_t> arrivalWorkspace() const;
    /// @return Whether @p daemonWorkspace is the one a switch of a window to another workspace of this
    ///         link's daemon waits for (switchWindow()).
    [[nodiscard]] bool isSwitchTarget(uint64_t daemonWorkspace) const;
    /// @return Whether the connection's last settled layout of @p daemonWorkspace holds a tab: only then has
    ///         a window switched to it something to show. One that holds none would empty the window, which
    ///         then closes as its last pane's end closes it.
    [[nodiscard]] bool holdsTab(uint64_t daemonWorkspace) const;
    /// The workspace a given-up switch asked the daemon to make arrived late: it is released as
    /// proto::WorkspaceRelease::SwitchedAway, so it stays in the daemon as hidden -- the native protocol
    /// closes no workspace, and a left one would come back on the next launch. @see cancelSwitch
    void hideAbandonedWorkspace();
    /// The workspace a switch of a window waits for is shown: the window is brought to it. @see switchWindow
    void finishSwitch(uint64_t workspace);
    /// A switch of a window to another workspace failed before it was shown: the follow is undone, and the
    /// switch is told why. No-op without a switch under way.
    void failSwitch(std::string const& reason);
    /// What a link a switch started, or a switch under way, makes of the connection's state: anything but
    /// connected or connecting before the workspace was shown fails it.
    void noteConnectionForSwitch();
    /// Stops, as the link no longer serves a window, and asks the host to destroy it (@see
    /// lastWindowReleased()).
    void end();
    void connectReconnectSignals();
    void closeLostWindows(std::vector<uint64_t> const& daemonWorkspaces);

    void showDaemonDrift();
    void askHostedExtensions();
    /// Answers the switch requests the daemon sent: each behind the guard (runGuardedSwitch()), in the window
    /// showing its workspace, or Failed when no window of this link shows it.
    void answerSwitchRequests();
    void requireGuiThread() const;

    Id _id;
    IDaemonLinkHost& _host;
    session::TerminalSessionManager& _sessions;
    std::string _hostLabel;
    window::DaemonDriftModel _drift; ///< Declared before the controllers: destroyed after them.
    window::DaemonLocality _locality;
    window::DaemonUpkeep _upkeep;
    std::string _clientRelease;
    AttachState _state = AttachState::Connecting;

    /// The live controller; null once given up. Remote-backed sessions hold ptys that unregister from it.
    std::unique_ptr<NativeController> _controller;
    /// The controller given up on, stopped: kept rather than destroyed, because its destruction waits for a
    /// daemon start still under way (@see awaitConnectedOrAbandon()). Null otherwise.
    std::unique_ptr<NativeController> _givenUp;

    /// Where the answers to the daemon's extensions are recorded, and so sent; made with the first question.
    /// Declared after the controller, which it sends through. Its deleter is chosen where it is made, so that
    /// this class is laid out the same without CONTOUR_EXTENSIONS, which never makes one.
    std::unique_ptr<vtextension::IGrantStore, void (*)(vtextension::IGrantStore*)> _grants { nullptr,
                                                                                             nullptr };

    /// Each daemon workspace mapped to the OS window that mirrors it.
    AttachWindowMap _attachWindowMap;
    /// The daemon workspace whose OS window is being spawned right now: reconcile() stages it just before
    /// openWindowFor(), and that window claims it through bindPendingWindow(). Single-slot: the QML load is
    /// synchronous, so a stage is always consumed before the next is made.
    std::optional<uint64_t> _pendingAttachWorkspace;
    /// The layouts of the reconcile under way, one snapshot for every window of it -- one whose OS window
    /// opens meanwhile included -- so that all of them are brought to the same push. Null between reconciles.
    LayoutSnapshot const* _reconcilingLayouts = nullptr;
    /// Where a further workspace's hand-over to a link of its own stands; one handed over is gone from here.
    enum class HandOverState : uint8_t
    {
        Pending = 0, ///< The other link is connecting; this link still follows the workspace.
        Failed,      ///< It did not connect: the workspace stays here, in a window of this link's.
    };

    /// What this link was started with, for the links it hands workspaces to.
    DaemonLinkConfig _handOverTemplate;
    /// Why the link was started (@see LinkRole); fixed at construction, its report state aside.
    LinkRole _role;
    /// A switch of one of this link's windows to another workspace of its daemon, waiting for that
    /// workspace's first layout (@see switchWindow()).
    struct PendingSwitch
    {
        vtworkspace::WorkspaceId window; ///< The window switching.
        uint64_t to = 0;                 ///< The workspace it switches to; 0 for one the daemon makes.
        /// How @c to is released when the switch does not land (@see undoReleaseFor()).
        vthost::proto::WorkspaceRelease undo = vthost::proto::WorkspaceRelease::Closed;
        /// The workspaces the connection was pushed before: a new one is the one not among them.
        std::set<uint64_t> before;
        SwitchLanded landed; ///< Told how it went.
    };
    std::optional<PendingSwitch> _switch;
    /// What the connection was pushed before a switch to a workspace the daemon makes (PendingSwitch::to 0)
    /// that cancelSwitch() gave up: the daemon may still make it, and this connection claims it. Once its
    /// layout arrives it is hidden (hideAbandonedWorkspace()), not shown in a window of its own. Forgotten
    /// when the connection is lost: the one sought next does not resume a workspace whose layout never came.
    std::optional<std::set<uint64_t>> _abandonedNewWorkspace;
    /// The further workspaces this link hands over, each with where its hand-over stands.
    std::map<uint64_t, HandOverState> _handOvers;
    /// Whether the boot window has adopted the daemon's primary workspace yet. Not "the map is empty": after
    /// a reconnect lost every window, the windows that came instead are new OS windows.
    session::AttachBoot _attachBoot = session::AttachBoot::Pending;
    /// The daemon release the drift badge was last assessed for, to notice a daemon that came back changed.
    std::string _shownDaemonRelease;

    /// The receiver of every controller connection the link makes; declared last but for its post target,
    /// so that it is destroyed first and no queued signal reaches a link being torn down.
    QObject _receiver;
    /// What the controller's background stop (@see end) tells the link through, from its own thread;
    /// withdrawn as the link is destroyed, before the controller.
    std::shared_ptr<platform::PostTarget> _postTarget = std::make_shared<platform::PostTarget>(_receiver);
};

/// Sees to it that every daemon the application is linked to has one live link whose reconnect follows what a
/// launch would when none of its workspaces survived (NativeController::ResumeFallback::Launch): the launch's
/// own link, until it ends; then, of that daemon's live links, the one made first. Only that link takes the
/// workspaces a daemon restarted with a restore brings back -- every other link's ids name nothing of the new
/// run (proto::AttachIntent::daemonInstance) -- so they are shown again, and the application keeps a window
/// rather than closing its last. Links share a daemon by the name a switch gave it, or, for `--connect-tcp`,
/// by their shared transport. Called whenever a link starts or ends.
/// @param links The application's links, in the order they were made.
void passOnLaunchResume(std::span<DaemonLink* const> links);

/// The daemon workspace the boot (first) OS window should adopt when attaching, rather than authoring
/// a fresh tab: in attach mode the boot window mirrors the daemon's primary (lowest-id) workspace. Pure so
/// the boot-adoption rule is unit-testable without the QML boot flow.
/// @param daemonWorkspaceIds The daemon's known workspace ids, ascending (workspaceIds() ordering).
/// @return The daemon workspace to adopt, or nullopt if the daemon has not reported a workspace yet.
[[nodiscard]] std::optional<std::uint64_t> primaryDaemonWorkspaceToAdopt(
    std::vector<std::uint64_t> const& daemonWorkspaceIds);

} // namespace contour::remote
