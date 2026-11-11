// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/DaemonDriftFollower.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchGuard.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/WindowController.hpp>

#include <core/Assert.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <utility>

#include <vthost/RestoredCounts.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/SwitchClient.hpp>
#include <vthost/client/ReleaseDrift.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

#ifdef CONTOUR_EXTENSIONS
    #include <vtextension/ExtensionConsent.hpp>
    #include <vthost/client/DaemonGrantReplies.hpp>
#endif

namespace contour::remote
{

namespace
{
    /// @return Why a link built with @p config is started; its switch's callback is moved out of @p config.
    [[nodiscard]] LinkRole roleOf(DaemonLinkConfig& config)
    {
        if (config.handOverFrom)
            return HandOverRole { .from = *config.handOverFrom, .report = HandOverRole::Report::Due };
        if (config.arrival)
            return std::move(*config.arrival);
        return BootRole {};
    }

    /// What a window says when a further workspace could not get a connection of its own and opens on its
    /// connection instead; {} is why.
    constexpr auto WindowLinkFailedText =
        std::string_view { "A workspace opened on this window's connection: {}" };

    /// @return @p failure, or what a connection lost without a word says.
    [[nodiscard]] std::string failureOrLost(std::string failure)
    {
        return failure.empty() ? std::string { "the connection to the daemon was lost" } : std::move(failure);
    }

    /// Detaches every pane of @p sessions still alive: its session runs on in its daemon. Each is activated
    /// and closed, as closeActivePane() closes the tab's active pane.
    void detachPanes(session::TerminalSessionManager& manager,
                     std::span<QPointer<session::TerminalSession> const> sessions)
    {
        for (auto const& session: sessions)
        {
            if (!session)
                continue;
            auto const [window, tab, leaf] = manager.model().findSessionLeaf(session->modelSessionId());
            if (leaf == nullptr)
                continue;
            manager.model().setActivePane(tab->id(), leaf->id());
            manager.closeActivePane(session.data(), session::SessionEnd::Detach);
        }
    }

    /// A daemon's release judged against the client's, with what both offered at the handshake.
    struct AssessedDaemon
    {
        vthost::client::DriftNotice notice;        ///< How the daemon's release stands against the client's.
        vthost::proto::CapabilitySet capabilities; ///< What the daemon and this client both offered.
    };

    /// @param controller The live connection to the daemon.
    /// @param clientRelease This client's release.
    /// @return The drift of the daemon's release from @p clientRelease and the negotiated capabilities.
    [[nodiscard]] AssessedDaemon assessDaemon(NativeController const& controller,
                                              std::string const& clientRelease)
    {
        auto const negotiated = controller.negotiated();
        auto const layout = negotiated.transform(vthost::client::layoutSurvivesRestart)
                                .value_or(vthost::client::LayoutAfterRestart::Lost);
        return AssessedDaemon {
            .notice = vthost::client::assessDrift(clientRelease, controller.daemonRelease(), layout),
            .capabilities = negotiated ? negotiated->shared : vthost::proto::CapabilitySet {},
        };
    }

} // namespace

DaemonLink::DaemonLink(Id id,
                       DaemonLinkConfig config,
                       IDaemonLinkHost& host,
                       session::TerminalSessionManager& sessions):
    _id(id),
    _host(host),
    _sessions(sessions),
    _hostLabel(config.hostLabel),
    _locality(config.locality),
    _upkeep(config.upkeep),
    _clientRelease(config.clientRelease),
    _controller(std::make_unique<NativeController>(config.controller)),
    _handOverTemplate(std::move(config)),
    _role(roleOf(_handOverTemplate))
{
    // The links this one hands workspaces to take them from this one, and no switch started them. Nor do they
    // open a tab in the directory a launch stated: that one is the launch's own link's alone.
    _handOverTemplate.handOverFrom.reset();
    _handOverTemplate.arrival.reset();
    _handOverTemplate.controller.workingDirectory.clear();
    connectController();
}

DaemonLink::~DaemonLink()
{
    // Before the controller's destructor waits for a stop end() began, which tells this link when done.
    _postTarget->withdraw();
    if (!isStopped())
        _sessions.releaseBinding(*this);
}

void DaemonLink::requireGuiThread() const
{
    // No application in a unit test that never makes one: there is no other thread to have strayed onto.
    if (auto const* const app = QCoreApplication::instance())
        Require(QThread::currentThread() == app->thread());
}

session::SessionFactory& DaemonLink::sessions() noexcept
{
    return anyController();
}

std::string_view DaemonLink::hostLabel() const noexcept
{
    return _state == AttachState::Attached ? std::string_view { _hostLabel } : std::string_view {};
}

std::string_view DaemonLink::chromeLabel() const noexcept
{
    if (_state != AttachState::Attached)
        return {};
    if (!_hostLabel.empty())
        return _hostLabel;
    // A local daemon says its label, unless it is the one a plain launch uses.
    auto const& daemon = address();
    if (daemon.kind != vthost::proto::SwitchDaemonKind::Label || daemon.name == vthost::DefaultSocketLabel)
        return {};
    return daemon.name;
}

std::optional<session::SwitchTarget> DaemonLink::daemonTarget() const
{
    return switchTargetOf(address());
}

NativeController::ResumeFallback DaemonLink::resumeFallback() const
{
    return anyController().resumeFallback();
}

void DaemonLink::resumeAsLaunch()
{
    requireGuiThread();
    if (isLive())
        _controller->resumeAsLaunch();
}

bool DaemonLink::bindsPanes() const
{
    return anyController().bindsPanes();
}

bool DaemonLink::isFinished() const
{
    return _state == AttachState::Ended && !anyController().isWindingDown() && !bindsPanes();
}

void DaemonLink::lastWindowReleased()
{
    requireGuiThread();
    end();
}

void DaemonLink::end()
{
    if (isStopped())
        return;
    // stop(), with the controller's stop off the GUI thread: it waits up to DefaultDetachBound for the daemon
    // to see this client detach, which a daemon that stopped reading makes it wait in full, and the
    // application lives on meanwhile (the quick terminal, a local window). The link is not finished until
    // the controller has wound down, so it outlives the stop, and is told when that is.
    _state = AttachState::Ended;
    _sessions.releaseBinding(*this);
    anyController().stopInBackground([target = _postTarget, this] {
        target->post([this] {
            if (isFinished())
                _host.linkEnded();
        });
    });
    // From now on only: the panes of its closed windows let its controller go one by one, and the last one
    // has the link destroyed.
    QObject::connect(
        &anyController(),
        &NativeController::paneUnbound,
        &_receiver,
        [this] {
            if (isFinished())
                _host.linkEnded();
        },
        Qt::QueuedConnection);
    _host.linkEnded();
}

void DaemonLink::installAttachWindowBinder()
{
    // A window spawned to host a daemon workspace binds itself here (from its Main.qml, via
    // consumeAttachWindow) instead of creating a fresh first tab.
    // Removed by the manager as the link stops (releaseBinding()), and by pollAttached() on giving up.
    _sessions.setAttachWindowBinder(*this, [this](window::WindowController* controller) {
        return controller != nullptr && bindPendingWindow(*controller) == WindowAdoption::Adopted;
    });
}

void DaemonLink::connectController()
{
    requireGuiThread();
    // The launch's link, for the boot window; one taking a workspace over, or opening one for a switch,
    // installs it as it opens its window.
    if (isBootLink())
        installAttachWindowBinder();
    QObject::connect(
        _controller.get(),
        &NativeController::layoutChanged,
        &_receiver,
        [this] {
            if (isStopped())
                return;
            // First: a Reconnect from a footer has the panes lost with the connection take their sessions
            // back before the layout is reconciled, which would otherwise find no live pane for them and
            // realize them again beside the ended ones.
            _sessions.rebindDisconnected(*this);
            followLayout();
        },
        Qt::QueuedConnection);
    // The daemon's restore is told once, ahead of the layout it restored: the manager holds it until a
    // window of that layout is realized or reconciled, and says it there.
    QObject::connect(
        _controller.get(),
        &NativeController::workspaceRestored,
        &_receiver,
        [this](quint32 workspaces, quint32 tabs) {
            if (isStopped())
                return;
            _sessions.noteWorkspaceRestored(
                *this, vthost::RestoredCounts { .workspaces = workspaces, .tabs = tabs });
        },
        Qt::QueuedConnection);
    connectReconnectSignals();
    // A dying connection ends every mirror session (stop() closes the ptys), closing the tabs through the
    // shell-exit teardown.
    QObject::connect(
        _controller.get(),
        &NativeController::connectionClosed,
        &_receiver,
        [this] {
            // Not once a connect that failed has been given up on (see pollAttached()).
            if (!isLive())
                return;
            // A link taking a workspace over that never showed it leaves it where it was; so does a switch.
            reportHandOver(HandOverOutcome::Failed, _controller->failure());
            reportArrival(std::unexpected(failureOrLost(_controller->failure())));
            failSwitch(failureOrLost(_controller->failure()));
            // Before stop(), which closes every mirror pty — the order matters. A Reconnect from a footer
            // that ends here did not bring the daemon back: the panes it was for say so, with the
            // connection's own words. @see TerminalSessionManager::connectionClosed
            _sessions.connectionClosed(*this, _controller->failure());
            _controller->stop();
            // One that never got a window has nothing left to serve.
            if (!isBootLink() && _sessions.windowsBoundTo(*this).empty())
                end();
        },
        Qt::QueuedConnection);
    // A drop the daemon refused did nothing there, and nothing local follows: say so.
    QObject::connect(
        _controller.get(),
        &NativeController::tabMergeRefused,
        &_receiver,
        [this] {
            if (isStopped())
                return;
            _sessions.postNoticeToWindowsOf(
                *this,
                window::Notice { .kind = window::NoticeKind::DropRefused,
                                 .text = std::string { window::TabMergeRefusedText } });
        },
        Qt::QueuedConnection);
    QObject::connect(
        _controller.get(),
        &NativeController::moveRefused,
        &_receiver,
        [this](int reason, int modelReason, quint64 request) {
            if (isStopped())
                return;
            _sessions.remoteMoveRefused(request);
            // A drop onto itself, or one that would leave everything where it is, has nothing to tell.
            if (auto const text = window::moveRefusedText(reason, modelReason); !text.empty())
                _sessions.postNoticeToWindowsOf(
                    *this,
                    window::Notice { .kind = window::NoticeKind::DropRefused, .text = std::string { text } });
        },
        Qt::QueuedConnection);
    QObject::connect(
        _controller.get(),
        &NativeController::switchRequested,
        &_receiver,
        [this] {
            if (!isStopped())
                answerSwitchRequests();
        },
        Qt::QueuedConnection);
#ifdef CONTOUR_EXTENSIONS
    QObject::connect(
        _controller.get(),
        &NativeController::extensionPermissionRequested,
        &_receiver,
        [this] {
            if (!isStopped())
                askHostedExtensions();
        },
        Qt::QueuedConnection);
#endif
    // The connect, hello, first spawn and first snapshot run on the reactor while the GUI boots; the
    // application waits for them through pollAttached() just before the first window. The signals above are
    // connected first, so nothing the connection emits meanwhile is lost. A cold start's daemon start is part
    // of the connect, in the transport's ensureDaemon, on a worker thread of the transport's own.
    _controller->startConnecting();
}

std::optional<std::expected<void, std::string>> DaemonLink::pollAttached(vthost::AttachBudget budget)
{
    requireGuiThread();
    if (_controller == nullptr)
        return std::unexpected(std::string { "the attach was given up" });
    auto polled = _controller->pollConnected(budget);
    if (!polled)
        return std::nullopt;
    if (!*polled)
    {
        abandonConnect(_controller, _givenUp, &_receiver);
        // A window spawned from now on is no daemon workspace's: it opens a tab of its own.
        _sessions.clearAttachWindowBinder(*this);
        return polled;
    }
    _state = AttachState::Attached;
    return polled;
}

void DaemonLink::stop()
{
    requireGuiThread();
    if (!isStopped())
    {
        _state = AttachState::Stopped;
        // The attach-window binder goes with the windows.
        _sessions.releaseBinding(*this);
    }
    // Also once ended: lastWindowReleased() only began this stop, in the background, and the application's
    // teardown may stop the link before it ended. Left running, the reactor would go on writing the mirrors
    // of the sessions the teardown deletes; this waits for it. Idempotent, as is a given-up controller's
    // stop.
    anyController().stop();
}

void DaemonLink::onConfigurationReloaded()
{
    requireGuiThread();
    if (isLive())
        _controller->restateSessionSettings();
}

void DaemonLink::stopForDaemonRestart()
{
    requireGuiThread();
    // Ended here, rather than lost when the daemon stops: a lost connection is sought again, and a
    // reconnect would start a daemon of its own while the restart waits for none to accept.
    // Before stop(), which closes every mirror pty -- @see TerminalSession::noteTransportLost
    if (!isLive())
        return;
    _sessions.noteTransportLost(*this);
    _controller->stop();
}

void DaemonLink::showDaemonDrift()
{
    auto const& controller = *_controller;
    _shownDaemonRelease = controller.daemonRelease();
    auto assessed = assessDaemon(controller, _clientRelease);
    // Only the daemon this client launched on is its to restart or upgrade (window::DaemonUpkeep).
    auto const offer = window::isThisClientsDaemon(_locality, _upkeep)
                           ? window::driftOfferFor(assessed.notice.kind, _locality, assessed.capabilities)
                           : window::DriftOffer::None;
    // A daemon that is not ours to touch shows no badge: there is nothing the user could do about it.
    _drift.show(offer == window::DriftOffer::None ? vthost::client::DriftNotice {}
                                                  : std::move(assessed.notice),
                offer);
}

std::expected<void, std::string> DaemonLink::upgradeDaemonOnCommand()
{
    requireGuiThread();
    auto daemon = std::optional<window::ConnectedDaemon> {};
    // A controller that is still connecting, reconnecting or held by an upgrade knows nothing current of
    // its daemon: only a connected one is judged.
    if (isLive() && _controller->connectionState() == vthost::client::ConnectionState::Connected)
    {
        auto const assessed = assessDaemon(*_controller, _clientRelease);
        daemon = window::ConnectedDaemon { .drift = assessed.notice.kind,
                                           .daemonCapabilities = assessed.capabilities };
    }
    if (auto const verdict = window::upgradeVerdictFor(_locality, _upkeep, daemon);
        verdict != window::UpgradeVerdict::Upgrade)
        return std::unexpected(std::string { window::describe(verdict) });
    // The model's upgradeRequested() reaches the application (offerDaemonRestartIfDrifted()).
    if (_drift.requestCommandedUpgrade() == window::CommandedUpgradeStart::AlreadyUnderWay)
        return std::unexpected(std::string { window::UpgradeAlreadyUnderWay });
    return {};
}

void DaemonLink::offerDaemonRestartIfDrifted()
{
    requireGuiThread();
    if (!isLive())
        return;
    showDaemonDrift();
    // An upgrade closes this client's connection and holds it for the daemon's return; the badge says so
    // meanwhile, and shows what the new daemon runs once the connection is back. Whichever client asked, and
    // whether or not the daemon is ours to upgrade: its windows wait all the same.
    auto& controller = *_controller;
    followDaemonDrift(controller, _drift, [this, &controller](window::DriftReassessment reassessment) {
        if (reassessment == window::DriftReassessment::Reassess
            || controller.daemonRelease() != _shownDaemonRelease)
            showDaemonDrift();
    });
    // Only a daemon this client can restart -- the local one it launched on -- is offered anything.
    if (!window::isThisClientsDaemon(_locality, _upkeep))
        return;
    // The badge shows a failure only next to a drift; an upgrade asked for by command may have none.
    QObject::connect(
        &_drift, &window::DaemonDriftModel::upgradeFailed, &_receiver, [this](QString const& reason) {
            if (_drift.isVisible() || isStopped())
                return;
            _sessions.postNoticeToWindowsOf(
                *this,
                window::upgradeFailedNotice(reason.isEmpty() ? "unknown error" : reason.toStdString()));
        });
    QObject::connect(&_drift, &window::DaemonDriftModel::upgradeRequested, &_receiver, [this] {
        if (isStopped() || _host.upgradeDaemon(*this) == UpgradeStart::Started)
            return;
        // Another restart or upgrade of the application's daemon runs, asked of another window's link: this
        // request starts nothing, and the model stands idle again.
        _drift.noteUpgradeNotStarted();
        _sessions.postNoticeToWindowsOf(*this, window::upgradeFailedNotice(window::UpgradeAlreadyUnderWay));
    });
    QObject::connect(&_drift, &window::DaemonDriftModel::restartRequested, &_receiver, [this] {
        if (!isStopped())
            _host.restartDaemon(*this);
    });
}

void DaemonLink::followLayout()
{
    // Reconcile the GUI against the daemon's authoritative workspace→tab→split tree: whenever a layout
    // arrives or changes (GUI boot, or a workspace/tab/split authored on the daemon, here or by another
    // client), map each daemon workspace onto an OS window and realize any tab not yet shown, each pane bound
    // to its remote session. Incremental — what is already shown is left untouched. First, a workspace a
    // given-up switch asked for is let go of, before a window is opened for it.
    hideAbandonedWorkspace();
    reconcile();
    expireRestoreSummaryAfterLayout();
    // A link taking a workspace over has it shown once its connection has the layout: the link it came from
    // may let it go now, and the workspace was followed by one of the two throughout.
    if (auto const* const handOver = handOverRole();
        handOver && isLive() && _controller->settledLayout(handOver->from.workspace) != nullptr)
        reportHandOver(HandOverOutcome::Settled, {});
    // A window switching to another workspace of this daemon is brought to it once its layout holds a tab.
    if (_switch && isLive())
    {
        auto const layouts = _controller->settledLayouts();
        auto const keys = layouts | std::views::keys;
        if (auto const target = std::ranges::find_if(
                keys, [this](uint64_t id) { return isSwitchTarget(id) && holdsTab(id); });
            target != keys.end())
            finishSwitch(*target);
    }
    // Last: a link a switch started tells it once its connection shows the workspace, and may be handed the
    // window that asked right then. Not before the workspace holds a tab: the window would be emptied.
    if (arrivalDue() && isLive())
        if (auto const workspace = arrivalWorkspace(); workspace && holdsTab(*workspace))
            reportArrival(*workspace);
}

void DaemonLink::reportArrival(std::expected<uint64_t, std::string> landed)
{
    if (!arrivalDue())
        return;
    // Emptied first: what it is told may end or rebind this link.
    std::exchange(arrival()->landed, SwitchLanded {})(std::move(landed));
}

std::optional<uint64_t> DaemonLink::arrivalWorkspace() const
{
    auto const& intent = _handOverTemplate.controller.intent;
    if (intent.kind == vthost::proto::AttachIntentKind::Workspaces && !intent.workspaces.empty())
        return intent.workspaces.front();
    if (!isLive())
        return std::nullopt;
    // A new workspace's id is what the daemon pushes: the only one the connection follows.
    auto const layouts = _controller->settledLayouts();
    if (layouts.empty())
        return std::nullopt;
    return layouts.begin()->first;
}

bool DaemonLink::isSwitchTarget(uint64_t daemonWorkspace) const
{
    if (!_switch)
        return false;
    if (_switch->to != 0)
        return daemonWorkspace == _switch->to;
    // One the daemon made: pushed since the follow, and shown by no window yet.
    return !_switch->before.contains(daemonWorkspace) && !_attachWindowMap.contains(daemonWorkspace);
}

bool DaemonLink::holdsTab(uint64_t daemonWorkspace) const
{
    auto const layout = _controller->settledLayout(daemonWorkspace);
    return layout != nullptr && !layout->tabs.empty();
}

std::optional<uint64_t> DaemonLink::workspaceOf(vtworkspace::WorkspaceId window) const
{
    auto const mapped =
        std::ranges::find(_attachWindowMap, window, [](auto const& entry) { return entry.second; });
    if (mapped == _attachWindowMap.end())
        return std::nullopt;
    return mapped->first;
}

std::optional<vtworkspace::WorkspaceId> DaemonLink::windowShowing(uint64_t daemonWorkspace) const
{
    auto const mapped = _attachWindowMap.find(daemonWorkspace);
    if (mapped == _attachWindowMap.end())
        return std::nullopt;
    return mapped->second;
}

std::expected<void, std::string> DaemonLink::switchWindow(vtworkspace::WorkspaceId window,
                                                          uint64_t workspace,
                                                          vthost::proto::WorkspaceRelease undo,
                                                          SwitchLanded landed)
{
    requireGuiThread();
    if (!isLive())
        return std::unexpected(std::string { "the window's connection to its daemon is gone" });
    if (_switch)
        return std::unexpected(std::string { "a switch of this window is under way" });
    if (!workspaceOf(window))
        return std::unexpected(std::string { "the window shows no workspace of its daemon" });
    auto before = std::ranges::to<std::set>(_controller->settledLayouts() | std::views::keys);
    if (auto followed = _controller->follow(workspace); !followed)
        return followed;
    _switch = PendingSwitch { .window = window,
                              .to = workspace,
                              .undo = undo,
                              .before = std::move(before),
                              .landed = std::move(landed) };
    return {};
}

void DaemonLink::finishSwitch(uint64_t workspace)
{
    auto pending = *std::exchange(_switch, std::nullopt);
    // Detach last: the old workspace stays in its daemon as hidden, and only now.
    if (auto const from = workspaceOf(pending.window))
    {
        if (auto released = _controller->release(*from, vthost::proto::WorkspaceRelease::SwitchedAway);
            !released)
            errorLog()("switch: workspace {} stays followed: {}", *from, released.error());
        _attachWindowMap.erase(*from);
    }
    // The old workspace's layout is gone with its release: the reconcile realizes the new one and closes
    // (detaches) the panes no layout names any more, after the new ones exist.
    bindDaemonWorkspace(workspace, pending.window);
    pending.landed(workspace);
}

void DaemonLink::cancelSwitch(vtworkspace::WorkspaceId window, std::string const& reason)
{
    requireGuiThread();
    if (!_switch || _switch->window != window)
        return;
    if (_switch->to == 0)
        _abandonedNewWorkspace = _switch->before;
    failSwitch(reason);
}

void DaemonLink::hideAbandonedWorkspace()
{
    if (!_abandonedNewWorkspace || !isLive())
        return;
    auto const layouts = _controller->settledLayouts();
    auto const keys = layouts | std::views::keys;
    // The one the daemon made: pushed since the follow, and shown by no window (as isSwitchTarget() has it).
    auto const made = std::ranges::find_if(keys, [this](uint64_t id) {
        return !_abandonedNewWorkspace->contains(id) && !_attachWindowMap.contains(id)
               && _pendingAttachWorkspace != id;
    });
    if (made == keys.end())
        return;
    _abandonedNewWorkspace.reset();
    if (auto released = _controller->release(*made, vthost::proto::WorkspaceRelease::SwitchedAway); !released)
        attachLog()(
            "switch: workspace {} a given-up switch made stays followed: {}", *made, released.error());
}

void DaemonLink::failSwitch(std::string const& reason)
{
    if (!_switch)
        return;
    auto pending = *std::exchange(_switch, std::nullopt);
    // Undone, the target left as it was: a reconnect does not resume it, and the daemon is told when the
    // connection is up.
    if (pending.to != 0 && _controller)
        if (auto released = _controller->release(pending.to, pending.undo); !released)
            attachLog()("switch: the follow of workspace {} is not undone: {}", pending.to, released.error());
    pending.landed(std::unexpected(reason));
}

void DaemonLink::noteConnectionForSwitch()
{
    using vthost::client::ConnectionState;
    auto const state = _controller->connectionState();
    if (state == ConnectionState::Connected || state == ConnectionState::Connecting)
        return;
    failSwitch(failureOrLost(_controller->failure()));
    if (!arrivalDue())
        return;
    // A link a switch started whose connection went before it showed the workspace: the window that asked
    // stays as it was, and nothing here needs the link any more.
    reportArrival(std::unexpected(failureOrLost(_controller->failure())));
    _controller->stop();
    if (_sessions.windowsBoundTo(*this).empty())
        end();
}

void DaemonLink::switchedAway(vtworkspace::WorkspaceId window)
{
    requireGuiThread();
    if (!isLive())
        return;
    auto const from = workspaceOf(window);
    if (!from)
        return;
    if (auto released = _controller->release(*from, vthost::proto::WorkspaceRelease::SwitchedAway); !released)
        errorLog()("switch: workspace {} stays followed: {}", *from, released.error());
    _attachWindowMap.erase(*from);
}

std::expected<void, std::string> DaemonLink::canAdoptWindow(vtworkspace::WorkspaceId window) const
{
    auto const workspace = isLive() ? arrivalWorkspace() : std::nullopt;
    if (!workspace || _controller->settledLayout(*workspace) == nullptr)
        return std::unexpected(std::string { "the connection to the daemon shows no workspace yet" });
    if (!holdsTab(*workspace))
        return std::unexpected(std::string { "the daemon's workspace holds no tab" });
    if (_sessions.model().workspace(window) == nullptr)
        return std::unexpected(std::string { "the window is gone" });
    return {};
}

std::expected<void, std::string> DaemonLink::adoptWindow(vtworkspace::WorkspaceId window)
{
    requireGuiThread();
    if (auto adoptable = canAdoptWindow(window); !adoptable)
        return adoptable;
    auto const workspace = arrivalWorkspace();
    // Watched: the new tabs come first, so the window never empties, and the old panes go after.
    auto previous = std::vector<QPointer<session::TerminalSession>> {};
    for (auto* const session: _sessions.sessionsOfWindow(window))
        previous.emplace_back(session);
    bindDaemonWorkspace(*workspace, window);
    detachPanes(_sessions, previous);
    return {};
}

void DaemonLink::abandon()
{
    requireGuiThread();
    auto const undo = arrival() != nullptr ? arrival()->undo : vthost::proto::WorkspaceRelease::Closed;
    if (auto* const switched = arrival())
        switched->landed = {};
    if (auto const workspace = arrivalWorkspace(); workspace && isLive())
        std::ignore = _controller->release(*workspace, undo);
    end();
}

void DaemonLink::reportHandOver(HandOverOutcome outcome, std::string const& reason)
{
    auto* const handOver = handOverRole();
    if (handOver == nullptr || handOver->report == HandOverRole::Report::Told)
        return;
    handOver->report = HandOverRole::Report::Told;
    _host.handOverSettled(handOver->from, outcome, reason);
}

void DaemonLink::completeHandOver(uint64_t workspace, HandOverOutcome outcome, std::string const& reason)
{
    requireGuiThread();
    auto const found = _handOvers.find(workspace);
    if (!isLive() || found == _handOvers.end())
        return;
    switch (outcome)
    {
        case HandOverOutcome::Settled:
            _handOvers.erase(found);
            // Closed, not SwitchedAway: no window switched away from it; another shows it now. Should the
            // release not reach the daemon, the connection ending later leaves the workspace left all the
            // same.
            if (auto released = _controller->release(workspace, vthost::proto::WorkspaceRelease::Closed);
                !released)
                errorLog()("attach: workspace {} stays followed by the link it was handed from: {}",
                           workspace,
                           released.error());
            return;
        case HandOverOutcome::Failed:
            found->second = HandOverState::Failed;
            _sessions.postNoticeToWindowsOf(
                *this,
                window::Notice { .kind = window::NoticeKind::WindowLinkFailed,
                                 .text = std::format(WindowLinkFailedText, reason) });
            // The window it gets is this link's (the multi-window path).
            reconcile();
            return;
    }
}

void DaemonLink::windowClosed(vtworkspace::WorkspaceId window)
{
    requireGuiThread();
    if (!isLive())
        return;
    // Given up as a timed-out one is: a new workspace the daemon makes for it later is hidden, not shown in
    // a window of its own.
    cancelSwitch(window, "the window was closed");
    auto const mapped =
        std::ranges::find(_attachWindowMap, window, [](auto const& entry) { return entry.second; });
    if (mapped == _attachWindowMap.end())
        return;
    // Emptied by a move, the workspace may be filled again, and then gets a window of its own: kept.
    auto const layout = _controller->settledLayout(mapped->first);
    if (layout == nullptr || layout->tabs.empty())
        return;
    // The mapping goes with the closed window in the next reconcile; the workspace is not pushed any more.
    if (auto released = _controller->release(mapped->first, vthost::proto::WorkspaceRelease::Closed);
        !released)
        attachLog()("attach: the daemon is not told window {} closed: {}", window.value, released.error());
}

void DaemonLink::spawnWindowFor(uint64_t daemonWorkspace, std::vector<uint64_t> const& sessions)
{
    // Its Main.qml claims the staged id (consumeAttachWindow → bindPendingWindow), records the mapping and
    // reconciles -- so it never creates a stray fresh tab. The QML load is synchronous, so the stage is
    // consumed before this returns. A window a move of this process asked for opens where the pane was
    // dropped.
    _pendingAttachWorkspace = daemonWorkspace;
    installAttachWindowBinder();
    _host.openWindowFor(_sessions.stageRemoteMoveWindow(sessions));
}

void DaemonLink::placeFurtherWorkspace(uint64_t daemonWorkspace,
                                       std::vector<uint64_t> const& sessions,
                                       std::size_t& replacementsDue)
{
    // What comes instead of a window the daemon lost opens at once, in this link: the lost one closes next,
    // and were it the process's last, a window still to come would be too late.
    if (replacementsDue > 0)
    {
        --replacementsDue;
        spawnWindowFor(daemonWorkspace, sessions);
        return;
    }
    auto const negotiated = _controller->negotiated();
    auto const canHandOver =
        negotiated && negotiated->shared.contains(vthost::proto::Capability::WorkspaceFollow);
    auto const found = _handOvers.find(daemonWorkspace);
    if (!canHandOver || (found != _handOvers.end() && found->second == HandOverState::Failed))
    {
        spawnWindowFor(daemonWorkspace, sessions);
        return;
    }
    if (found != _handOvers.end())
        return; // the link it is handed to is still connecting
    _handOvers.emplace(daemonWorkspace, HandOverState::Pending);
    auto config = _handOverTemplate;
    config.controller.intent =
        vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Workspaces,
                                      .workspaces = { daemonWorkspace } };
    config.controller.resumeFallback = NativeController::ResumeFallback::Nothing;
    config.handOverFrom = HandOver { .from = _id, .workspace = daemonWorkspace };
    _host.startHandOverLink(std::move(config));
}

void DaemonLink::reconcile()
{
    reconcileReplacing(0);
}

void DaemonLink::reconcileReplacing(std::size_t replacementsDue)
{
    requireGuiThread();
    if (!isLive())
        return;

    // Taken before the settled layouts are read, so they are at least as new as the push of each move named.
    auto const landedMoves = _controller->takeLandedMoves();
    _sessions.noteRemoteMovesLanded(landedMoves);

    // Whether every window was placed: one whose OS window is still spawning is skipped, and a move landing
    // in it is placed by the reconcile that gets to it.
    auto reach = session::TerminalSessionManager::ReconcileReach::Whole;

    // One snapshot of the layouts for every window: a move between two windows is then seen in both, however
    // the push that made it is read meanwhile. As of the last push's end, as applyRemoteLayout realizes
    // them: a window the next push's first frame adds would otherwise get an OS window that stays blank
    // until that push ends.
    auto const layouts = _controller->settledLayouts();
    _reconcilingLayouts = &layouts;

    // Daemon workspace ids come ascending, so the primary (lowest-id) workspace is handled
    // first — it maps to the boot window; the rest each get their own OS window. One already shown is brought
    // up to date; this places the others.
    auto const openWindow = [&](ReconciledWorkspace const& workspace) {
        auto const& [daemonWorkspace, tabs, sessions] = workspace;
        if (_pendingAttachWorkspace == daemonWorkspace)
        {
            reach = session::TerminalSessionManager::ReconcileReach::Partial;
            return; // an OS window is already spawning for this daemon workspace
        }

        // The workspace a window switches to is that window's, once its first layout is here.
        if (isSwitchTarget(daemonWorkspace))
        {
            reach = session::TerminalSessionManager::ReconcileReach::Partial;
            return;
        }

        if (auto const* const handOver = handOverRole();
            _attachBoot == session::AttachBoot::Pending && handOver)
        {
            // A link taking a workspace over opens its own window for it, and places nothing else before.
            if (daemonWorkspace == handOver->from.workspace)
                spawnWindowFor(daemonWorkspace, sessions);
            reach = session::TerminalSessionManager::ReconcileReach::Partial;
            return;
        }

        if (auto const* const switched = arrival(); _attachBoot == session::AttachBoot::Pending && switched)
        {
            // A link a switch started: one opening a workspace opens its own window for it; one switching a
            // window waits to be handed that window. Neither places anything else before.
            if (switched->mode == session::SwitchMode::Open && daemonWorkspace == arrivalWorkspace())
                spawnWindowFor(daemonWorkspace, sessions);
            reach = session::TerminalSessionManager::ReconcileReach::Partial;
            return;
        }

        if (_attachBoot == session::AttachBoot::Pending)
        {
            // The primary daemon workspace adopts the boot window (already created before
            // the first layout arrived). A missing focused window (mid-boot) stages
            // the daemon workspace so bindPendingWindow can bind it once the boot
            // window becomes ready, instead of hoping for another layout push.
            // Never the quick terminal, which is no daemon workspace: an ordinary window adopts it.
            if (auto const* const boot = _sessions.applicationController())
                bindDaemonWorkspace(daemonWorkspace, boot->windowId());
            else if (!_pendingAttachWorkspace)
                _pendingAttachWorkspace = daemonWorkspace;
            reach = session::TerminalSessionManager::ReconcileReach::Partial;
            return;
        }

        // A daemon workspace holding no tab is nothing to show yet -- one a reconnect found emptied, under a
        // client that follows every window -- and gets its OS window once it holds one.
        if (tabs == DaemonWorkspaceTabs::None)
            return;

        // A further daemon workspace: a window of its own, on a link of its own.
        placeFurtherWorkspace(daemonWorkspace, sessions, replacementsDue);
    };
    reconcileAttachWindows(_sessions, *_controller, layouts, _attachWindowMap, openWindow);
    _reconcilingLayouts = nullptr;
    // A tab closed since it was paired names nothing: its pairing goes, whatever the daemon hands out next.
    forgetClosedTabPairings(_sessions, *_controller);
    // Every window is reconciled: what a landing move moved is in place, to raise, focus and announce.
    _sessions.completeRemoteMoves(reach);
}

WindowAdoption DaemonLink::bindPendingWindow(window::WindowController& window)
{
    requireGuiThread();
    if (!isLive())
        return WindowAdoption::Declined;

    // A reconcile-spawned OS window claims the daemon workspace staged for it.
    if (_pendingAttachWorkspace)
    {
        auto const daemonWorkspace = *_pendingAttachWorkspace;
        _pendingAttachWorkspace.reset();
        bindDaemonWorkspace(daemonWorkspace, window.windowId());
        return WindowAdoption::Adopted;
    }

    // The boot (first) OS window in attach mode ADOPTS the daemon's primary workspace instead of
    // authoring a fresh tab. Claiming it here makes Main.qml skip win.createNewTab(), which in
    // attach mode would author a spurious extra tab on the daemon (and create no local first tab).
    // Bind the primary workspace now if the daemon already reported one; otherwise still claim the boot
    // window and let reconcile() bind it once the first layout arrives.
    if (_attachBoot == session::AttachBoot::Pending && isBootLink())
    {
        // Before anything is realized in it: what it makes is made through this link.
        _sessions.bindWindow(window.windowId(), *this);
        // As of the last push's end, as every reconcile reads them: the next push's first frame may name a
        // window whose layout is not complete yet.
        auto const settled = std::ranges::to<std::vector>(_controller->settledLayouts() | std::views::keys);
        if (auto const adopt = primaryDaemonWorkspaceToAdopt(settled))
            bindDaemonWorkspace(*adopt, window.windowId());
        return WindowAdoption::Adopted;
    }

    return WindowAdoption::Declined;
}

session::RemoteRouting DaemonLink::requestBootWorkspace(session::RemoteSpawn const& spawn)
{
    requireGuiThread();
    return isLive() && _attachBoot == session::AttachBoot::Pending
                   && _controller->requestRemoteWorkspace(spawn)
               ? session::RemoteRouting::Routed
               : session::RemoteRouting::Local;
}

void DaemonLink::connectReconnectSignals()
{
    // Every pane of the link's windows says it is reconnecting while the connection is sought again, rather
    // than showing a grid that merely stopped.
    QObject::connect(
        _controller.get(),
        &NativeController::connectionStateChanged,
        &_receiver,
        [this] {
            if (!isLive())
                return;
            _sessions.setHostConnection(*this, _controller->connectionState());
            // The workspace a given-up switch asked for is not resumed by the connection sought now (no
            // layout of it arrived, or it would have been let go of already): whatever that connection is
            // pushed later is some other one, and must not be taken for it.
            if (_controller->connectionState() != vthost::client::ConnectionState::Connected)
                _abandonedNewWorkspace.reset();
            // A switch fails when the connection goes before the workspace it waits for is shown.
            if (_switch || arrivalDue())
            {
                noteConnectionForSwitch();
                if (!isLive())
                    return;
            }
            // A link taking a workspace over awaits nobody's pollAttached(): it settles as it connects, or
            // learns the daemon has no such workspace (NativeController's failed connect, which stays up). So
            // does a link a switch started.
            if (isBootLink() || _state != AttachState::Connecting)
                return;
            if (auto const failure = _controller->failure(); !failure.empty())
            {
                reportHandOver(HandOverOutcome::Failed, failure);
                reportArrival(std::unexpected(failure));
                _controller->stop();
                end();
                return;
            }
            if (_controller->connectionState() == vthost::client::ConnectionState::Connected)
            {
                _state = AttachState::Attached;
                offerDaemonRestartIfDrifted();
            }
        },
        Qt::QueuedConnection);
    QObject::connect(
        _controller.get(),
        &NativeController::workspacesLost,
        &_receiver,
        [this](std::vector<std::uint64_t> const& daemonWorkspaces) {
            if (!isStopped())
                closeLostWindows(daemonWorkspaces);
        },
        Qt::QueuedConnection);
}

void DaemonLink::closeLostWindows(std::vector<std::uint64_t> const& daemonWorkspaces)
{
    if (!isLive())
        return;
    auto doomed = std::vector<vtworkspace::WorkspaceId> {};
    for (auto const daemonWorkspace: daemonWorkspaces)
    {
        auto const mapped = _attachWindowMap.find(daemonWorkspace);
        if (mapped == _attachWindowMap.end())
            continue;
        // Before the close, which closes their ptys: their shells did not exit, the connection to them went.
        // @see TerminalSession::noteTransportLost
        for (auto* session: _sessions.sessionsOfWindow(mapped->second))
            session->noteTransportLost();
        doomed.push_back(mapped->second);
        _attachWindowMap.erase(mapped);
    }
    // The windows that came instead first: closing the last stale window before would end the application.
    reconcileReplacing(doomed.size());
    for (auto const window: doomed)
        if (auto* const controller = _sessions.controllerFor(window))
            controller->closeWindow();
}

void DaemonLink::bindDaemonWorkspace(std::uint64_t daemonWorkspace, vtworkspace::WorkspaceId local)
{
    requireGuiThread();
    _attachBoot = session::AttachBoot::Adopted;
    _attachWindowMap.emplace(daemonWorkspace, local);
    _sessions.bindWindow(local, *this);
    if (_reconcilingLayouts != nullptr)
        applyRemoteLayout(_sessions, local, *_controller, *_reconcilingLayouts, daemonWorkspace);
    else
        applyRemoteLayout(_sessions, local, *_controller, daemonWorkspace);
    _sessions.deliverRestoreSummaryTo(local);
}

void DaemonLink::expireRestoreSummaryAfterLayout()
{
    requireGuiThread();
    if (!isLive())
        return;
    auto const layout = _controller->workspaceIds().empty() ? session::LayoutWorkspaces::None
                                                            : session::LayoutWorkspaces::Some;
    auto const spawn = _pendingAttachWorkspace ? session::AttachSpawn::Spawning : session::AttachSpawn::Idle;
    if (session::restoreSummaryFateAfterLayout(layout, spawn, _attachBoot)
        == session::RestoreSummaryFate::Keep)
        return;
    _sessions.expireRestoreSummary(*this);
}

void DaemonLink::askHostedExtensions()
{
#ifdef CONTOUR_EXTENSIONS
    if (!isLive())
        return;
    if (!_grants)
    {
        auto replies = std::make_unique<vthost::client::DaemonGrantReplies>(
            [this](std::string_view hostId, std::string_view digest, vthost::GrantState answer) {
                if (_controller)
                    _controller->answerPermission(std::string { hostId }, std::string { digest }, answer);
            });
        _grants = { replies.release(), [](vtextension::IGrantStore* made) {
                       std::default_delete<vtextension::IGrantStore> {}(made);
                   } };
    }
    for (auto& [pty, request]: _controller->takePermissionRequests())
    {
        auto* session = _sessions.sessionForPty(pty);
        if (!session)
            continue;
        if (auto consent = vthost::client::consentRequestOf(request))
            session->askForHostedExtension(std::move(*consent), *_grants);
        else
            // A newer daemon's permission this client cannot phrase: asking without it would have the
            // user grant what they were not shown. Asked again on the next attach.
            errorLog()("Extension {} asks for a control permission this version of Contour cannot show; "
                       "not asking.",
                       request.hostId);
    }
#endif
}

void DaemonLink::answerSwitchRequests()
{
    if (!isLive())
        return;
    for (auto& request: _controller->takeSwitchRequests())
    {
        // Answered on the connection that asked, while its controller lives: a switch to another daemon ends
        // this link once the window is rebound, and the reply is posted ahead of that stop.
        auto reply = [controller = QPointer<NativeController> { _controller.get() },
                      id = request.request](vthost::proto::SwitchOutcome outcome, std::string detail) {
            if (controller)
                controller->replySwitch(vthost::proto::SwitchReply {
                    .request = id, .outcome = outcome, .detail = std::move(detail) });
        };
        auto const window = windowShowing(request.workspace);
        if (!window)
        {
            reply(vthost::proto::SwitchOutcome::Failed,
                  std::format("no window here shows workspace {}", request.workspace));
            continue;
        }
        runGuardedSwitch(
            GuardedSwitch { .window = *window,
                            .source = address(),
                            .target = session::switchTargetOf(std::move(request.target)),
                            .answerWithin = switchAnswerWindowOf(request.answerWithinMs) },
            _host.switchGrants(),
            [this](vtworkspace::WorkspaceId asking,
                   session::SwitchAsk ask) -> std::expected<SwitchWithdraw, std::string> {
                auto* const session = _sessions.actingSessionOfWindow(asking);
                if (session == nullptr)
                    return std::unexpected(std::string { "the window has no pane to ask in" });
                auto const asked = session->askToSwitchWindow(std::move(ask));
                return SwitchWithdraw { [session = QPointer<session::TerminalSession> { session }, asked] {
                    if (session)
                        session->withdrawSwitchAsk(asked);
                } };
            },
            _sessions.windowSwitching(),
            _host.switchDeadline(),
            std::move(reply));
    }
}

namespace
{
    /// @return Whether @p a and @p b reach one daemon: by the name a switch gives it, or, for one no switch
    ///         can name (`--connect-tcp`), by the transport they share -- every link a hand-over starts
    ///         copies its template's. Not by the run each connection met, which differs between the links of
    ///         one daemon while they reconnect to a restarted one.
    [[nodiscard]] bool onOneDaemon(DaemonLink& a, DaemonLink& b)
    {
        if (a.address().kind != vthost::proto::SwitchDaemonKind::Same)
            return a.address() == b.address();
        return b.address().kind == vthost::proto::SwitchDaemonKind::Same
               && a.siblingConfig().controller.transport == b.siblingConfig().controller.transport;
    }
} // namespace

void passOnLaunchResume(std::span<DaemonLink* const> links)
{
    using Fallback = NativeController::ResumeFallback;
    auto const live = std::ranges::to<std::vector>(links | std::views::filter(&DaemonLink::isLive));
    for (auto* const link: live)
    {
        // The first live link of its daemon decides for all of them.
        auto const sameDaemon = [link](DaemonLink* other) {
            return other == link || onOneDaemon(*link, *other);
        };
        if (*std::ranges::find_if(live, sameDaemon) != link)
            continue;
        if (std::ranges::none_of(live | std::views::filter(sameDaemon),
                                 [](DaemonLink* each) { return each->resumeFallback() == Fallback::Launch; }))
            link->resumeAsLaunch();
    }
}

std::optional<std::uint64_t> primaryDaemonWorkspaceToAdopt(
    std::vector<std::uint64_t> const& daemonWorkspaceIds)
{
    if (daemonWorkspaceIds.empty())
        return std::nullopt;
    return daemonWorkspaceIds.front(); // workspaceIds() is ascending, so front() is the primary workspace
}

} // namespace contour::remote
