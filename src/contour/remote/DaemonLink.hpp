// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One daemon connection and the windows it serves.

#include <contour/askpass/AskpassPromptRouter.hpp>
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
#include <contour/window/WindowId.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtGui/QScreen>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
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
};

/// How a link that opens a window of its own (IDaemonLinkHost::startWindowLink) opens it.
struct WindowOpening
{
    /// The daemon's id of the tab the window activates once it shows the workspace -- the tab torn off
    /// another window --; none for the tab the daemon names.
    std::optional<uint64_t> activeTab = std::nullopt;
    /// Where the window opens; null for the default placement, and once the platform took the screen away
    /// while the link connected.
    QPointer<QScreen> screen = nullptr;
    /// The window New Window or a tear-off was asked from, told when the new window cannot open; none for
    /// nobody to tell.
    std::optional<window::WindowId> askedBy = std::nullopt;
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
    /// How a link opens a window of its own once its connection's selected workspace holds a tab (New
    /// Window, a tab torn off); none for the launch's link, which adopts the boot window, and for a link a
    /// switch started (@c arrival).
    std::optional<WindowOpening> opening = std::nullopt;
    /// The daemon as a window switch names it: its local label, or its saved host; Same for one no switch can
    /// name (`--connect-tcp`).
    DaemonAddress address {};
    /// What a link a window switch started does once its connection shows its workspace; none for every
    /// other link.
    std::optional<SwitchArrival> arrival = std::nullopt;
    /// The window whose ssh prompts this link's attach shows: the one that started it, or AnyWindow.
    askpass::AskpassRouteId askpassRoute = askpass::AnyWindow;
};

/// The launch's link: it adopts the boot window.
struct BootRole
{
};

/// A link that opens a window of its own (DaemonLinkConfig::opening).
struct OpeningRole
{
    WindowOpening opening; ///< How the window opens.
};

/// Why a link was started, which decides how it comes by its window: the launch's link adopts the boot
/// window, one opening a window opens it (OpeningRole), and one a window switch started is handed the window
/// that asked or opens one (SwitchArrival, whose landed callback is emptied once told).
using LinkRole = std::variant<BootRole, OpeningRole, SwitchArrival>;

/// One daemon connection and the one window it serves.
///
/// Owns the NativeController and the attach bookkeeping: the daemon workspace its window selects and the
/// local workspace that window displays it in, the boot window's adoption, the drift badge and the answers
/// to the daemon's extension questions. The connection selects one workspace, and the window displays that
/// one: every other workspace of the daemon is only an entry of the window's directory
/// (NativeController::directory). A selection the daemon moves -- asked for (selectWorkspace()), or because
/// the selected workspace ended -- is realized in the window in place. A further window on the daemon is a
/// link of its own (IDaemonLinkHost::startWindowLink). GUI thread only, apart from what the controller does
/// on its own reactor.
class DaemonLink final: public session::WindowBinding, public window::IWorkspaceDirectorySource
{
  public:
    /// Names a link among the application's.
    using Id = DaemonLinkId;

    /// Builds the link and its controller, and starts connecting: wires the controller's signals to the
    /// link's windows, installs the attach-window binder on the manager (removed again as the link stops or
    /// gives up), and starts the connect on the controller's reactor. Nothing of it can fail here; a connect
    /// that fails is told by pollAttached().
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

    /// @return The controller, until the attach was given up (pollAttached()); null after.
    [[nodiscard]] NativeController* liveController() noexcept { return _controller.get(); }

    /// @return Whether pollAttached() succeeded: the attach settled connected, was not given up, and the
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

    /// @return The controller as the factory of the link's sessions; the given-up one after pollAttached()
    ///         gave up, which creates nothing.
    [[nodiscard]] session::SessionFactory& sessions() noexcept override;

    /// @return The remote host the link is attached to; empty for a local daemon, `--connect-tcp`, until
    ///         pollAttached() succeeded, and once the link stopped.
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

    /// Its window closed: a selection under way fails, and the daemon keeps the workspace for whichever
    /// window selects it next. No-op once given up or stopped.
    /// @param window The closed window.
    void windowClosed(vtworkspace::WorkspaceId window) override;

    void reportFocus(vthost::proto::WindowFocus focus) override;
    /// @return Whether its window raises, as its daemon last said; Raise for a link that has given up, whose
    ///         window has no daemon's panes left to ring for.
    [[nodiscard]] vthost::proto::RaiseRole raiseRole() const noexcept override;
    /// @return How many tabs of the workspaces its window does not select wait for the user, as the
    ///         directory's program status has them.
    [[nodiscard]] unsigned tabsWaitingElsewhere() const override;
    /// @return Itself: its windows list its daemon's workspaces.
    [[nodiscard]] window::IWorkspaceDirectorySource* directorySource() noexcept override { return this; }
    /// @return Every workspace of its daemon, as its connection holds them; none while it holds no
    ///         connection.
    [[nodiscard]] window::WorkspaceDirectory directory() const override;
    /// Has its window show @p workspace of its daemon (selectWorkspace()); a refusal is logged, as the
    /// window has no one to tell.
    /// @param workspace The daemon's workspace id.
    void select(uint64_t workspace) override;
    /// @return Its daemon as every link to it names it: by the name a switch gives it, or, for one no switch
    ///         can name, by its transport.
    [[nodiscard]] std::optional<uint64_t> daemonKey() const override;
    /// Starts a link of its own to this link's daemon that selects what @p window displays and opens a window
    /// for it (IDaemonLinkHost::startWindowLink).
    [[nodiscard]] std::expected<void, std::string> openWindowLike(vtworkspace::WorkspaceId window,
                                                                  std::optional<vtworkspace::TabId> activeTab,
                                                                  QScreen* screen) override;

    /// @return The daemon as a window switch names it (DaemonLinkConfig::address).
    [[nodiscard]] DaemonAddress const& address() const noexcept { return _siblingTemplate.address; }

    /// @return The window whose prompts this link's attach shows (DaemonLinkConfig::askpassRoute).
    [[nodiscard]] askpass::AskpassRouteId askpassRoute() const noexcept
    {
        return _siblingTemplate.askpassRoute;
    }

    /// @return What a further link to this link's daemon is built with: this one's configuration, opening no
    ///         window, started by no switch and stating no working directory. Its intent is the caller's to
    ///         set.
    [[nodiscard]] DaemonLinkConfig const& siblingConfig() const noexcept { return _siblingTemplate; }

    /// @param window A local workspace of the manager.
    /// @return The daemon workspace this link's window displays in @p window; none for any other.
    [[nodiscard]] std::optional<uint64_t> workspaceOf(vtworkspace::WorkspaceId window) const;

    /// @param daemonWorkspace A daemon workspace.
    /// @return The local workspace this link's window displays it in; none when the window selects another.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> windowShowing(uint64_t daemonWorkspace) const;

    /// Has its window show another workspace of its daemon, in place: the connection selects @p workspace
    /// (NativeController::select), and once the daemon's layout batch states that selection and the
    /// workspace holds a tab, the window displays it in a local workspace of its own -- realized, then
    /// shown, then the panes of the one it displayed before detached, their sessions left running in the
    /// daemon. A connection lost, the window closed or the switch given up (cancelSwitch()) before that leave
    /// the window as it was.
    /// @param workspace The daemon workspace to show; 0 for a new one, which the daemon makes.
    /// @param landed Told once: the workspace the window shows now, or why it still shows the old one; empty
    ///        to be told nothing.
    /// @return Nothing once the selection was sent; why not otherwise (then @p landed is not told).
    [[nodiscard]] std::expected<void, std::string> selectWorkspace(uint64_t workspace,
                                                                   SwitchLanded landed = {});

    /// A switch selectWorkspace() started that is not wanted any more (it took too long): the connection
    /// selects again what the window displays, the window stays as it was, and the switch's landed callback
    /// is told @p reason. No-op when no switch of @p window is under way.
    /// @param window The window switching.
    /// @param reason Why it is given up.
    void cancelSwitch(vtworkspace::WorkspaceId window, std::string const& reason);

    /// Its window switches to another daemon: the window is the other link's to bind next (adoptWindow()),
    /// which ends this one. No-op once given up or stopped, and for a window that is not this link's.
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

    /// A link a switch started that is not wanted any more: it ends, and with its connection goes its
    /// selection.
    void abandon();

    /// @return What the link's drift badge shows.
    [[nodiscard]] window::DaemonDriftModel& drift() noexcept { return _drift; }

    /// @return What the drift badge of each window it serves shows: its own drift.
    [[nodiscard]] window::DaemonDriftModel* daemonDrift() noexcept override { return &_drift; }
    /// Upgrades the daemon in place when window::upgradeVerdictFor() allows it for this link's daemon.
    [[nodiscard]] std::expected<void, std::string> upgradeDaemonOnCommand() override;

    /// Looks at the connect the constructor began, with what remains of @p budget, and returns at once, so
    /// the event loop serves whatever the connect needs the GUI thread for (ssh's prompts). On failure the
    /// controller is given up (@see abandonConnect()), and with it the attach-window binder.
    /// @param budget How long the attach may take since the link was built.
    /// @return Nothing while it is still going; else nothing once attached, or why not.
    [[nodiscard]] std::optional<std::expected<void, std::string>> pollAttached(vthost::AttachBudget budget);

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

    /// Brings its window in line with the daemon: the selected workspace is adopted by the boot window, or
    /// opens the window of a link that opens one, or -- when the daemon moved the selection -- is realized in
    /// the window in place of the one it displayed; and the displayed workspace's tab/pane tree is
    /// reconciled. Idempotent; re-run on every layout change. No-op once given up or stopped.
    void reconcile();

    /// What every layout of the daemon brings, and the GUI's boot once: reconcile(), then a restore summary
    /// no window took is forgotten, once no window of the layout is still to be bound. No-op once given up or
    /// stopped.
    void followLayout();

    /// Called from a freshly spawned window's Main.qml (through the manager's attach-window binder): the
    /// window the link opened for itself, or the boot window, displays the workspace the link's connection
    /// selects, and is served by the link from then on (session::TerminalSessionManager::bindWindow). No-op
    /// once given up or stopped, and for a link that has its window.
    /// @param window The freshly created controller of the spawned OS window.
    /// @return Whether the link adopted the window.
    [[nodiscard]] WindowAdoption bindPendingWindow(window::WindowController& window);

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
    /// Whether a reconcile found the link's window to bring in line, or one still to come.
    enum class Placement : uint8_t
    {
        Pending = 0, ///< The window is still to come: a move landing in it is placed by a later reconcile.
        Placed,      ///< The window displays the selected workspace.
    };
    /// The link has no window yet: the boot window adopts the selected workspace, a link that opens a window
    /// opens it once that workspace holds a tab, and one a switch started waits to be handed one.
    /// @param selected The workspace the connection selects; 0 for none yet.
    /// @return Whether the window was placed by this reconcile.
    [[nodiscard]] Placement placeWindow(uint64_t selected);
    /// The daemon moved the selection to @p workspace: its window displays it, realized in a local workspace
    /// of its own, and the panes of the one it displayed before are detached. @see selectWorkspace
    void switchTo(uint64_t workspace);
    /// A pane this process dragged out of every window landed in a workspace of its own on the daemon, which
    /// no window selects: a window of a link of its own opens for it where it was dropped.
    void openWindowForMovedPanes();
    /// A link a switch started: tells the switch how its connection came to show its workspace, once.
    void reportArrival(std::expected<uint64_t, std::string> landed);
    /// @return Whether this is the launch's link, which adopts the boot window: it opens no window and was
    ///         started by no switch.
    [[nodiscard]] bool isBootLink() const noexcept { return std::holds_alternative<BootRole>(_role); }
    /// @return How a link that opens a window of its own opens it, for one opening a window or one an Open
    ///         switch started; null for every other link.
    [[nodiscard]] WindowOpening const* opening() const noexcept;
    /// @return What a link a switch started is to do; null for every other link.
    [[nodiscard]] SwitchArrival* arrival() noexcept { return std::get_if<SwitchArrival>(&_role); }
    /// @return Whether a link a switch started is still to tell the switch how it arrived.
    [[nodiscard]] bool arrivalDue() noexcept { return arrival() != nullptr && arrival()->landed; }
    /// @return The daemon workspace a link a switch started shows: the one its connection selects; none
    ///         before the daemon said.
    [[nodiscard]] std::optional<uint64_t> arrivalWorkspace() const;
    /// @return The workspace the window is to display: what the connection selects, unless a switch that was
    ///         given up still waits for its revert (@see cancelSwitch()).
    [[nodiscard]] uint64_t followedSelection();
    /// @return Whether the connection's last settled layout of @p daemonWorkspace holds a tab: only then has
    ///         a window switched to it something to show. One that holds none would empty the window, which
    ///         then closes as its last pane's end closes it.
    [[nodiscard]] bool holdsTab(uint64_t daemonWorkspace) const;
    /// Applies the daemon's layout of the workspace the window displays to the window.
    void applyShownLayout();
    /// A switch selectWorkspace() started failed before it was shown: the switch is told why. No-op without a
    /// switch under way.
    void failSwitch(std::string const& reason);
    /// Fails the switch under way once the daemon answered its own selection request (by number) with another
    /// workspace than the one asked for (NativeController::takeSelectAnswers): it has no such workspace, and
    /// the switch would otherwise wait out its timeout.
    void failRefusedSwitch();
    /// A link opening a window of its own whose window will not come -- its connection failed, or the window
    /// did not load -- tells the window it was asked from, once.
    /// @param reason Why, in words for the user.
    void noteOpeningFailed(std::string const& reason);
    /// Rings and notifies for what the panes of the workspaces the window does not select raised while it
    /// held the raise role (NativeController::takeRaised) -- a bell, a desktop notification, a command that
    /// finished -- as a pane out of sight would (IDaemonLinkHost::elsewhereNotices); each once.
    void raiseElsewhere();
    /// Rings the bell, and alerts, on the link's window, as @p bell says.
    void ringWindowBell(config::Bell const& bell);
    /// What a link a switch started, or a switch under way, makes of the connection's state: anything but
    /// connected or connecting before the workspace was shown fails it.
    void noteConnectionForSwitch();
    /// Stops, as the link no longer serves a window, and asks the host to destroy it (@see
    /// lastWindowReleased()).
    void end();
    void connectReconnectSignals();
    /// After a reconnect: the workspace the window displays is gone. The window displays what the
    /// connection selects instead, or, with nothing selected, closes.
    void closeLostWindows(std::vector<uint64_t> const& daemonWorkspaces);
    /// Records that the window displays @p daemonWorkspace in @p local, serves it from this link, and
    /// realizes the workspace into it at once: the one place the "a displayed workspace always implies a
    /// reconcile" invariant lives.
    /// @param daemonWorkspace The daemon workspace id.
    /// @param local The local workspace the window displays it in.
    void bindDaemonWorkspace(uint64_t daemonWorkspace, vtworkspace::WorkspaceId local);

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
    /// daemon start still under way (@see abandonConnect()). Null otherwise.
    std::unique_ptr<NativeController> _givenUp;

    /// Where the answers to the daemon's extensions are recorded, and so sent; made with the first question.
    /// Declared after the controller, which it sends through. Its deleter is chosen where it is made, so that
    /// this class is laid out the same without CONTOUR_EXTENSIONS, which never makes one.
    std::unique_ptr<vtextension::IGrantStore, void (*)(vtextension::IGrantStore*)> _grants { nullptr,
                                                                                             nullptr };

    /// The daemon workspace the window displays, and the local workspace it displays it in.
    struct Shown
    {
        uint64_t daemonWorkspace = 0;   ///< The daemon's id of it.
        vtworkspace::WorkspaceId local; ///< The local workspace realizing it.
        /// Whose active tab the next reconcile shows: the daemon's, until it was realized once.
        TabSelection selection = TabSelection::Seed;
    };
    std::optional<Shown> _shown;
    /// The window bound to this link that waits for the selected workspace's first layout: the boot window
    /// claimed before it came. Empty otherwise.
    std::optional<vtworkspace::WorkspaceId> _pendingWindow;
    /// Where a link that opens a window of its own stands with it.
    enum class OwnWindow : uint8_t
    {
        None = 0, ///< Not opened yet.
        Opening,  ///< Asked of the host; the window claims the workspace as it loads.
    };
    OwnWindow _ownWindow = OwnWindow::None;
    /// The layouts of the reconcile under way, one snapshot for the realization a selection change makes as
    /// for the window's pass. Null between reconciles.
    LayoutSnapshot const* _reconcilingLayouts = nullptr;
    /// What this link was started with, for the links it starts to its daemon.
    DaemonLinkConfig _siblingTemplate;
    /// Why the link was started (@see LinkRole); fixed at construction, its report state aside.
    LinkRole _role;
    /// A switch of the window to another workspace of its daemon, waiting for the daemon's selection to move
    /// there and the workspace to hold a tab (@see selectWorkspace()).
    struct PendingSwitch
    {
        uint64_t to = 0;      ///< The workspace it switches to; 0 for one the daemon makes.
        uint64_t request = 0; ///< The selection's request number, which the daemon's answer names.
        /// The workspaces the connection was pushed before the selection was asked for: a new one is the one
        /// not among them.
        std::set<uint64_t> before;
        SwitchLanded landed; ///< Told how it went.
    };
    std::optional<PendingSwitch> _switch;
    /// What a given-up switch selected again (cancelSwitch()): until the daemon states it, a batch still
    /// stating the given-up selection is not followed. Forgotten with the connection.
    std::optional<uint64_t> _revertTo;
    /// The move (session::TerminalSessionManager::PendingRemoteMove::request) a window of its own was opened
    /// for, so that a later reconcile opens no second one; 0 for none.
    uint64_t _movedWindowRequest = 0;
    /// Whether the window has displayed a daemon workspace yet.
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

} // namespace contour::remote
