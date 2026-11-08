// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/DaemonDriftFollower.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/WindowController.hpp>

#include <core/Assert.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include <algorithm>
#include <ranges>
#include <utility>

#include <vthost/RestoredCounts.hpp>
#include <vthost/client/ReleaseDrift.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

#ifdef CONTOUR_EXTENSIONS
    #include <vtextension/ExtensionConsent.hpp>
    #include <vthost/client/DaemonGrantReplies.hpp>
#endif

namespace contour::remote
{

DaemonLink::DaemonLink(Id id,
                       DaemonLinkConfig config,
                       IDaemonLinkHost& host,
                       session::TerminalSessionManager& sessions):
    _id(id),
    _host(host),
    _sessions(sessions),
    _hostLabel(std::move(config.hostLabel)),
    _locality(config.locality),
    _clientRelease(std::move(config.clientRelease)),
    _controller(std::make_unique<NativeController>(std::move(config.controller)))
{
    connectController();
}

DaemonLink::~DaemonLink()
{
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

bool DaemonLink::bindsPanes() const
{
    return anyController().bindsPanes();
}

bool DaemonLink::isFinished() const
{
    return _state == AttachState::Ended && !bindsPanes();
}

void DaemonLink::lastWindowReleased()
{
    requireGuiThread();
    if (isStopped())
        return;
    // stop(), less the controller's stop, which waits up to DefaultDetachBound for the daemon to see this
    // client detach: not inside the window close that got here, while the application lives on (the quick
    // terminal, a local window). Posted to the receiver, ahead of the destruction linkEnded() asks for, so
    // it runs first; a link destroyed before it stops its controller in the controller's destructor.
    _state = AttachState::Ended;
    _sessions.releaseBinding(*this);
    QMetaObject::invokeMethod(&_receiver, [this] { stop(); }, Qt::QueuedConnection);
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

void DaemonLink::connectController()
{
    requireGuiThread();
    // A window spawned to host a daemon workspace binds itself here (from its Main.qml, via
    // consumeAttachWindow) instead of creating a fresh first tab.
    // Removed by the manager as the link stops (releaseBinding()), and by awaitAttached() on giving up.
    _sessions.setAttachWindowBinder(*this, [this](window::WindowController* controller) {
        return controller != nullptr && bindPendingWindow(*controller) == WindowAdoption::Adopted;
    });
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
            // Not once a connect that failed has been given up on (see awaitAttached()).
            if (!isLive())
                return;
            // Before stop(), which closes every mirror pty — the order matters. A Reconnect from a footer
            // that ends here did not bring the daemon back: the panes it was for say so, with the
            // connection's own words. @see TerminalSessionManager::connectionClosed
            _sessions.connectionClosed(*this, _controller->failure());
            _controller->stop();
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
    // application waits for them through awaitAttached() just before the first window. The signals above are
    // connected first, so nothing the connection emits meanwhile is lost. A cold start's daemon start is part
    // of the connect, in the transport's ensureDaemon, on a worker thread of the transport's own.
    _controller->startConnecting();
}

std::expected<void, std::string> DaemonLink::awaitAttached(std::chrono::milliseconds budget)
{
    requireGuiThread();
    // Given up, not destroyed: see awaitConnectedOrAbandon(). Kept until the link ends instead.
    if (auto attached = awaitConnectedOrAbandon(_controller, _givenUp, budget, &_receiver); !attached)
    {
        // A window spawned from now on is no daemon workspace's: it opens a tab of its own.
        _sessions.clearAttachWindowBinder(*this);
        return attached;
    }
    _state = AttachState::Attached;
    return {};
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
    // Also once ended: lastWindowReleased() posts this stop, and the application's teardown may stop the
    // link before that event runs. Left running, the reactor would go on writing the mirrors of the sessions
    // the teardown deletes. Idempotent, as is a given-up controller's stop.
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
    auto const negotiated = controller.negotiated();
    auto const layout = negotiated.transform(vthost::client::layoutSurvivesRestart)
                            .value_or(vthost::client::LayoutAfterRestart::Lost);
    _shownDaemonRelease = controller.daemonRelease();
    auto notice = vthost::client::assessDrift(_clientRelease, _shownDaemonRelease, layout);
    auto const offer = window::driftOfferFor(
        notice.kind, _locality, negotiated ? negotiated->shared : vthost::proto::CapabilitySet {});
    // A daemon that is not ours to touch shows no badge: there is nothing the user could do about it.
    _drift.show(offer == window::DriftOffer::None ? vthost::client::DriftNotice {} : std::move(notice),
                offer);
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
    // Only a daemon this client can restart -- a local one -- is offered anything.
    if (_locality != window::DaemonLocality::Local)
        return;
    QObject::connect(&_drift, &window::DaemonDriftModel::upgradeRequested, &_receiver, [this] {
        if (!isStopped())
            _host.upgradeDaemon(*this);
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
    // to its remote session. Incremental — what is already shown is left untouched.
    reconcile();
    expireRestoreSummaryAfterLayout();
}

void DaemonLink::reconcile()
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

        // A new daemon workspace: spawn an OS window to host it. Its Main.qml claims the
        // staged id (consumeAttachWindow → bindPendingWindow), records the mapping
        // and reconciles — so it never creates a stray fresh tab. The QML load is
        // synchronous, so the stage is consumed before this returns. A window a move of this process asked
        // for opens where the pane was dropped.
        _pendingAttachWorkspace = daemonWorkspace;
        _host.openWindowFor(_sessions.stageRemoteMoveWindow(sessions));
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
    // Before anything is realized in it: what it makes is made through this link.
    _sessions.bindWindow(window.windowId(), *this);

    // A reconcile-spawned secondary OS window claims the daemon workspace staged for it.
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
    if (_attachBoot == session::AttachBoot::Pending)
    {
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
            if (isLive())
                _sessions.setHostConnection(*this, _controller->connectionState());
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
    reconcile();
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

std::optional<std::uint64_t> primaryDaemonWorkspaceToAdopt(
    std::vector<std::uint64_t> const& daemonWorkspaceIds)
{
    if (daemonWorkspaceIds.empty())
        return std::nullopt;
    return daemonWorkspaceIds.front(); // workspaceIds() is ascending, so front() is the primary workspace
}

} // namespace contour::remote
