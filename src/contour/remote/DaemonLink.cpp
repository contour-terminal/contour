// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/remote/DaemonDriftFollower.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchGuard.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/TaskbarAttentionTally.hpp>
#include <contour/window/WindowController.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

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

#include <vthost/NotifyWire.hpp>
#include <vthost/RestoredCounts.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/ViewRouting.hpp>
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
        if (config.arrival)
            return std::move(*config.arrival);
        if (config.opening)
            return OpeningRole { .opening = *config.opening };
        return BootRole {};
    }

    /// @return @p layouts' layout of @p workspace alone: a window displays one workspace, so a pane that
    ///         layout no longer names went to another workspace, or ended, and leaves the window.
    [[nodiscard]] LayoutSnapshot onlyLayoutOf(LayoutSnapshot const& layouts, uint64_t workspace)
    {
        auto const found = layouts.find(workspace);
        return found != layouts.end() ? LayoutSnapshot { *found } : LayoutSnapshot {};
    }

    /// @return A command that finished in a pane this client does not mirror, as a finish notice reads one.
    [[nodiscard]] vtbackend::CommandBlockSummary summaryOf(vthost::proto::FinishedCommand const& finished)
    {
        auto summary = vtbackend::CommandBlockSummary {};
        summary.commandLine = vtbackend::CommandLineText { finished.commandLine };
        summary.exitCode = finished.exitCode;
        summary.duration = finished.durationMs.transform([](uint64_t ms) {
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds { ms });
        });
        return summary;
    }

    /// What every finish notice of a pane of another workspace is raised under: the pane's daemon session id
    /// follows, so a pane's later finish replaces its earlier one.
    constexpr auto FinishedElsewhereIdentifier = std::string_view { "contour-daemon-finished-{}" };

    /// Whether a window rings for what was raised.
    enum class RingBell : uint8_t
    {
        No = 0,
        Yes,
    };

    /// @param directory What the window lists.
    /// @param pane A daemon session.
    /// @return The name of the tab holding @p pane; empty for none.
    [[nodiscard]] std::string tabLabelOf(window::WorkspaceDirectory const& directory, uint64_t pane)
    {
        for (auto const& workspace: directory.workspaces)
            for (auto const& tab: workspace.tabs)
                if (std::ranges::contains(tab.panes, window::PaneKey { pane }, &window::DirectoryPane::key))
                    return tab.label;
        return {};
    }

    /// Notifies a command that finished in a pane of a workspace the window does not select, as a hidden
    /// pane's finish is decided.
    /// @param pane The pane's daemon session.
    /// @param finished How the command finished.
    /// @param tabName The name of the pane's tab.
    /// @param notices What the finish is decided and raised with.
    /// @return Whether the finish asks the window to ring too.
    [[nodiscard]] RingBell noticeFinishedElsewhere(uint64_t pane,
                                                   vthost::proto::FinishedCommand const& finished,
                                                   std::string const& tabName,
                                                   ElsewhereNotices const& notices)
    {
        auto const summary = summaryOf(finished);
        auto const ending =
            platform::finishNoticeKind(summary, notices.policy, platform::SessionVisibility::Hidden);
        if (!ending)
            return RingBell::No;
        auto const request = platform::finishNotificationText(
            summary, *ending, tabName, notices.homeDirectory, notices.policy.action);
        auto const effects = platform::effectsOf(request.action);
        if (effects.test(platform::FinishEffect::Notification))
        {
            auto notification = vtbackend::DesktopNotification {};
            notification.identifier = std::format(FinishedElsewhereIdentifier, pane);
            notification.title = request.title;
            notification.body = request.body;
            notification.sound = platform::finishNoticeSound(
                request.action, platform::bellAudibilityOf(notices.bell.sound, notices.bell.volume));
            notices.notifier.notify(notification);
        }
        return effects.test(platform::FinishEffect::Bell) ? RingBell::Yes : RingBell::No;
    }

    /// Raises a desktop notification of a pane of a workspace the window does not select, as a hidden pane's.
    /// @param notice What the pane notified.
    /// @param notifier Where it is raised.
    void notifyElsewhere(vthost::proto::PaneNotification const& notice, platform::Notifier& notifier)
    {
        // As a mirrored pane raises one (ScreenMirror::applyEvent): one that stated no occasion takes the OSC
        // 9 / OSC 777 rule.
        auto const occasion =
            vthost::displayOccasionOf(notice.occasion).value_or(platform::UnstatedNotificationOccasion);
        if (!platform::isNoticeWanted(platform::notifyWhenOf(occasion), platform::SessionVisibility::Hidden))
            return;
        auto notification = vtbackend::DesktopNotification {};
        notification.title = notice.title;
        notification.body = notice.body;
        notification.occasion = occasion;
        notification.focusOnActivation = false;
        notifier.notify(notification);
    }

    /// @return @p failure, or what a connection lost without a word says.
    [[nodiscard]] std::string failureOrLost(std::string failure)
    {
        return failure.empty() ? std::string { "the connection to the daemon was lost" } : std::move(failure);
    }

    /// Holds @p controller in the state of a layout being realized for its lifetime: what the window does to
    /// its panes meanwhile is the daemon's doing, or the window's own housekeeping, and none of it is
    /// reported back as the user's (NativeController::reportTabPresentation drops it as it drops a layout's).
    class DaemonOriginated
    {
      public:
        explicit DaemonOriginated(NativeController& controller): _controller { controller }
        {
            _controller.setRealizingLayout(true);
        }

        DaemonOriginated(DaemonOriginated const&) = delete;
        DaemonOriginated& operator=(DaemonOriginated const&) = delete;
        DaemonOriginated(DaemonOriginated&&) = delete;
        DaemonOriginated& operator=(DaemonOriginated&&) = delete;

        ~DaemonOriginated() { _controller.setRealizingLayout(false); }

      private:
        NativeController& _controller;
    };

    /// Detaches every pane of @p sessions still alive: its session runs on in its daemon. Each is activated
    /// and closed, as closeActivePane() closes the tab's active pane. None of that is the user's doing, so
    /// none of it is reported to the daemon, which keeps the tab and pane the user left active.
    void detachPanes(session::TerminalSessionManager& manager,
                     NativeController& controller,
                     std::span<QPointer<session::TerminalSession> const> sessions)
    {
        auto const daemonOriginated = DaemonOriginated { controller };
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
    _siblingTemplate(std::move(config)),
    _role(roleOf(_siblingTemplate))
{
    // The links this one starts to its daemon open no window of this one's, and no switch started them. Nor
    // do they open a tab in the directory a launch stated: that one is the launch's own link's alone.
    _siblingTemplate.opening.reset();
    _siblingTemplate.arrival.reset();
    _siblingTemplate.controller.workingDirectory.clear();
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
            // A switch whose link never showed its workspace leaves the window where it was.
            reportArrival(std::unexpected(failureOrLost(_controller->failure())));
            failSwitch(failureOrLost(_controller->failure()));
            // Before stop(), which closes every mirror pty — the order matters. A Reconnect from a footer
            // that ends here did not bring the daemon back: the panes it was for say so, with the
            // connection's own words. @see TerminalSessionManager::connectionClosed
            _sessions.connectionClosed(*this, _controller->failure());
            _controller->stop();
            // One that never got a window has nothing left to serve.
            if (!isBootLink() && _sessions.windowsBoundTo(*this).empty())
            {
                noteOpeningFailed(failureOrLost(_controller->failure()));
                end();
            }
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
    // The raise-holding window rings and notifies for what the panes of the workspaces it does not select
    // raised; and every window's taskbar count includes the tabs that wait there.
    QObject::connect(
        _controller.get(),
        &NativeController::directoryChanged,
        &_receiver,
        [this] {
            if (isStopped())
                return;
            raiseElsewhere();
            _sessions.refreshTaskbarAttention();
            _sessions.notifyWorkspaceDirectoryChanged(*this);
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
    auto polled = _controller->pollConnected(std::move(budget));
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
    // Reconcile the GUI against the daemon's authoritative workspace→tab→split tree whenever a layout
    // arrives or changes (GUI boot, or a workspace/tab/split authored on the daemon, here or by another
    // client): the window displays the workspace its connection selects, each pane bound to its remote
    // session. Incremental — what is already shown is left untouched.
    reconcile();
    expireRestoreSummaryAfterLayout();
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

WindowOpening const* DaemonLink::opening() const noexcept
{
    if (auto const* const own = std::get_if<OpeningRole>(&_role))
        return &own->opening;
    static auto const openSwitch = WindowOpening {};
    if (auto const* const switched = std::get_if<SwitchArrival>(&_role);
        switched != nullptr && switched->mode == session::SwitchMode::Open)
        return &openSwitch;
    return nullptr;
}

std::optional<uint64_t> DaemonLink::arrivalWorkspace() const
{
    if (!isLive())
        return std::nullopt;
    auto const selected = _controller->selectedWorkspace();
    return selected != 0 ? std::optional { selected } : std::nullopt;
}

uint64_t DaemonLink::followedSelection()
{
    auto const selected = _controller->selectedWorkspace();
    if (!_revertTo)
        return selected;
    // What the switch was given up for has ended since: the daemon selects another in its place, and no batch
    // will state it. The window follows what it selects rather than stay on a workspace that is gone.
    if (!holdsTab(*_revertTo))
    {
        _revertTo.reset();
        return selected;
    }
    // A batch still stating what a given-up switch selected is not followed: the window stays as it is
    // until the daemon states the selection asked back.
    if (selected != *_revertTo)
        return _shown ? _shown->daemonWorkspace : selected;
    _revertTo.reset();
    return selected;
}

bool DaemonLink::holdsTab(uint64_t daemonWorkspace) const
{
    // Bound, not copied: the reconcile's snapshot when one runs, else the controller's settled one.
    auto const settled = _reconcilingLayouts != nullptr ? LayoutSnapshot {} : _controller->settledLayouts();
    auto const& layouts = _reconcilingLayouts != nullptr ? *_reconcilingLayouts : settled;
    auto const found = layouts.find(daemonWorkspace);
    return found != layouts.end() && !found->second->tabs.empty();
}

std::optional<uint64_t> DaemonLink::workspaceOf(vtworkspace::WorkspaceId window) const
{
    if (!_shown || _shown->local != window)
        return std::nullopt;
    return _shown->daemonWorkspace;
}

std::optional<vtworkspace::WorkspaceId> DaemonLink::windowShowing(uint64_t daemonWorkspace) const
{
    if (!_shown || _shown->daemonWorkspace != daemonWorkspace)
        return std::nullopt;
    return _shown->local;
}

std::optional<uint64_t> DaemonLink::activePaneSession() const
{
    if (!_shown)
        return std::nullopt;
    auto const* const controller = _sessions.controllerDisplaying(_shown->local);
    auto* const session = controller != nullptr ? controller->activeSession() : nullptr;
    return session != nullptr ? _controller->sessionForPty(&session->pty()) : std::nullopt;
}

std::expected<void, std::string> DaemonLink::selectWorkspace(uint64_t workspace, SwitchLanded landed)
{
    requireGuiThread();
    if (!isLive())
        return std::unexpected(std::string { "the connection to the daemon is gone" });
    if (!_shown)
        return std::unexpected(std::string { "the window shows no workspace of its daemon yet" });
    if (workspace != 0 && workspace == _shown->daemonWorkspace && !_revertTo)
    {
        // Shown already: a switch away still under way is given up, as one that timed out is -- the shown
        // workspace is selected again, and a batch stating the other one is not followed.
        cancelSwitch(_shown->local, "another switch of this window replaced it");
        if (landed)
            landed(workspace);
        return {};
    }
    // Taken before the request goes out: the controller's reactor sends it at once, and the daemon's answer
    // may settle -- the workspace it made among the layouts -- before this thread reads them.
    auto before = std::ranges::to<std::set>(_controller->settledLayouts() | std::views::keys);
    // A new workspace starts where the pane that asked is, as a tab made from it does.
    auto const sent = _controller->select(workspace, workspace == 0 ? activePaneSession() : std::nullopt);
    if (!sent)
        return std::unexpected(sent.error());
    // Only the latest switch of the window lands.
    failSwitch("another switch of this window replaced it");
    _revertTo.reset();
    _switch = PendingSwitch {
        .to = workspace,
        .request = *sent,
        .before = std::move(before),
        .landed = std::move(landed),
    };
    return {};
}

void DaemonLink::failRefusedSwitch()
{
    // Every answer is taken, whether a switch waits for it or not, so none is read for a later switch.
    auto const answers = _controller->takeSelectAnswers();
    if (!_switch)
        return;
    if (auto const refused = switchRefusal(_switch->request, _switch->to, _switch->before, answers))
        failSwitch(std::string { *refused });
}

void DaemonLink::cancelSwitch(vtworkspace::WorkspaceId window, std::string const& reason)
{
    requireGuiThread();
    if (!_switch || !_shown || _shown->local != window)
        return;
    // The daemon may still move the selection there: it is asked back to what the window displays.
    if (isLive() && _controller->select(_shown->daemonWorkspace).has_value())
        _revertTo = _shown->daemonWorkspace;
    failSwitch(reason);
}

void DaemonLink::failSwitch(std::string const& reason)
{
    if (!_switch)
        return;
    auto pending = *std::exchange(_switch, std::nullopt);
    if (pending.landed)
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
    if (!isLive() || !_shown || _shown->local != window)
        return;
    failSwitch("the window switched to another daemon");
    _shown.reset();
}

std::expected<void, std::string> DaemonLink::canAdoptWindow(vtworkspace::WorkspaceId window) const
{
    auto const workspace = arrivalWorkspace();
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
    detachPanes(_sessions, *_controller, previous);
    return {};
}

void DaemonLink::abandon()
{
    requireGuiThread();
    if (auto* const switched = arrival())
        switched->landed = {};
    end();
}

void DaemonLink::windowClosed(vtworkspace::WorkspaceId window)
{
    requireGuiThread();
    if (!_shown || _shown->local != window)
        return;
    // The daemon keeps the workspace for whichever window selects it next.
    failSwitch("the window was closed");
    _shown.reset();
}

void DaemonLink::reportFocus(vthost::proto::WindowFocus focus)
{
    requireGuiThread();
    if (isLive())
        _controller->reportFocus(focus);
}

vthost::proto::RaiseRole DaemonLink::raiseRole() const noexcept
{
    return _controller != nullptr ? _controller->raiseRole() : vthost::proto::RaiseRole::Raise;
}

window::WorkspaceDirectory DaemonLink::directory() const
{
    return _controller != nullptr ? _controller->directory() : window::WorkspaceDirectory {};
}

window::SelectResult DaemonLink::select(uint64_t workspace)
{
    auto const local = _shown ? std::optional { _shown->local } : std::nullopt;
    auto const selected =
        selectWorkspace(workspace, [this, local](std::expected<uint64_t, std::string> const& outcome) {
            if (outcome || !local)
                return;
            if (auto* const controller = _sessions.controllerOwning(*local))
                controller->cancelPendingJump();
        });
    if (!selected)
    {
        errorLog()("Cannot show workspace {}: {}", workspace, selected.error());
        return window::SelectResult::Refused;
    }
    return window::SelectResult::Requested;
}

unsigned DaemonLink::tabsWaitingElsewhere() const
{
    if (!isLive())
        return 0;
    auto const directory = _controller->directory();
    return window::tabsWaitingIn(directory, directory.selected);
}

std::optional<uint64_t> DaemonLink::daemonKey() const
{
    // As onOneDaemon() tells two links of one daemon: by its name, or by the transport they share.
    auto const& daemon = address();
    if (daemon.kind != vthost::proto::SwitchDaemonKind::Same)
        return std::hash<std::string> {}(std::format("{}:{}", std::to_underlying(daemon.kind), daemon.name));
    return std::hash<vthost::ITransportFactory const*> {}(_siblingTemplate.controller.transport.get());
}

std::expected<void, std::string> DaemonLink::openSiblingWindow(vtworkspace::WorkspaceId window,
                                                               session::SiblingShows shows,
                                                               std::optional<vtworkspace::TabId> activeTab,
                                                               QScreen* screen)
{
    requireGuiThread();
    if (!isLive())
        return std::unexpected(std::string { "this window shows no workspace of its daemon" });
    auto config = _siblingTemplate;
    switch (shows)
    {
        case session::SiblingShows::SameWorkspace:
            if (!_shown || _shown->local != window)
                return std::unexpected(std::string { "this window shows no workspace of its daemon" });
            config.controller.intent =
                vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Select,
                                              .workspace = _shown->daemonWorkspace };
            break;
        case session::SiblingShows::DaemonsChoice:
            // The daemon decides, as for a launch: a workspace no window shows, else a new one. It needs
            // nothing the asking window shows, and no tab to activate.
            config.controller.intent =
                vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Default,
                                              .workspace = 0 };
            activeTab = std::nullopt;
            break;
    }
    config.controller.resumeFallback = NativeController::ResumeFallback::Nothing;
    auto const* const asking = _sessions.controllerDisplaying(window);
    config.opening = WindowOpening {
        .activeTab =
            activeTab.and_then([this](vtworkspace::TabId tab) { return _controller->pairedWireTab(tab); }),
        .screen = screen,
        .askedBy = asking != nullptr ? std::optional { asking->windowId() } : std::nullopt,
    };
    _host.startWindowLink(std::move(config));
    return {};
}

void DaemonLink::reconcile()
{
    requireGuiThread();
    if (!isLive())
        return;

    // Taken before the settled layouts are read, so they are at least as new as the push of each move named.
    auto const landedMoves = _controller->takeLandedMoves();
    _sessions.noteRemoteMovesLanded(landedMoves);

    // One snapshot of the layouts for the whole pass: as of the last push's end, as applyRemoteLayout
    // realizes them, so that what the next push's first frame adds is not realized half.
    auto const layouts = _controller->settledLayouts();
    _reconcilingLayouts = &layouts;
    auto const selected = followedSelection();
    auto placement = Placement::Placed;
    if (!_shown)
        placement = placeWindow(selected);
    else if (selected != 0 && selected != _shown->daemonWorkspace && holdsTab(selected))
        switchTo(selected);
    else
    {
        applyShownLayout();
        // A switch to what the window shows already (asked while a revert was under way) has landed.
        if (_switch && _shown && _switch->to == _shown->daemonWorkspace && selected == _switch->to)
            if (auto pending = *std::exchange(_switch, std::nullopt); pending.landed)
                pending.landed(pending.to);
    }
    failRefusedSwitch();
    _reconcilingLayouts = nullptr;
    openWindowForMovedPanes();
    // A tab closed since it was paired names nothing: its pairing goes, whatever the daemon hands out next.
    forgetClosedTabPairings(_sessions, *_controller);
    // The window is reconciled: what a landing move moved is in place, to raise, focus and announce.
    _sessions.completeRemoteMoves(placement == Placement::Placed
                                      ? session::TerminalSessionManager::ReconcileReach::Whole
                                      : session::TerminalSessionManager::ReconcileReach::Partial);
}

DaemonLink::Placement DaemonLink::placeWindow(uint64_t selected)
{
    if (selected == 0 || _controller->settledLayout(selected) == nullptr)
        return Placement::Pending;
    // The boot window, claimed before the first layout came.
    if (auto const window = std::exchange(_pendingWindow, std::nullopt);
        window && _sessions.model().workspace(*window) != nullptr)
    {
        bindDaemonWorkspace(selected, *window);
        return Placement::Placed;
    }
    if (isBootLink())
    {
        if (_attachBoot != session::AttachBoot::Pending)
            return Placement::Pending;
        // The boot window, created before the first layout arrived, adopts the selected workspace; one that
        // is not there yet binds it as it comes (bindPendingWindow()). Never the quick terminal, which is no
        // daemon workspace, nor a window another link serves.
        if (auto const* const boot = _sessions.applicationController();
            boot != nullptr && _sessions.isInProcessWindow(boot->displayedWorkspace()))
        {
            bindDaemonWorkspace(selected, boot->displayedWorkspace());
            return Placement::Placed;
        }
        return Placement::Pending;
    }
    // A link that opens a window of its own opens it once the workspace has something to show; it claims the
    // selection as it loads (bindPendingWindow()). One a switch started is handed its window (adoptWindow()).
    auto const* const own = opening();
    if (own == nullptr || !holdsTab(selected) || _ownWindow == OwnWindow::Opening)
        return Placement::Pending;
    _ownWindow = OwnWindow::Opening;
    installAttachWindowBinder();
    _host.openWindowFor(own->screen.data());
    // The load is synchronous: a window that did not load, or did not take the workspace, never will, and
    // the link has nothing to serve.
    if (!_shown)
    {
        noteOpeningFailed("its window did not open");
        reportArrival(std::unexpected(std::string { "its window did not open" }));
        end();
        return Placement::Pending;
    }
    return Placement::Placed;
}

void DaemonLink::switchTo(uint64_t workspace)
{
    auto* const controller = _sessions.controllerDisplaying(_shown->local);
    if (controller == nullptr)
        return;
    auto const before = _shown->local;
    // Watched: the window realizes the new workspace first, so it never empties, and the old panes go after,
    // their sessions left running in the daemon.
    auto previous = std::vector<QPointer<session::TerminalSession>> {};
    for (auto* const session: _sessions.sessionsOfWindow(before))
        previous.emplace_back(session);
    auto const fresh = _sessions.createWorkspaceIn(controller->windowId());
    bindDaemonWorkspace(workspace, fresh);
    if (!_shown || _shown->local != fresh)
        return; // the window went while it was realized
    controller->displayWorkspace(fresh);
    detachPanes(_sessions, *_controller, previous);
    controller->dropWorkspaceIfEmpty(before);
    if (_switch && (_switch->to == workspace || (_switch->to == 0 && !_switch->before.contains(workspace))))
    {
        auto pending = *std::exchange(_switch, std::nullopt);
        if (pending.landed)
            pending.landed(workspace);
    }
}

void DaemonLink::applyShownLayout()
{
    if (!_shown)
        return;
    auto const local = _shown->local;
    auto const settled = _reconcilingLayouts != nullptr ? LayoutSnapshot {} : _controller->settledLayouts();
    auto const only = onlyLayoutOf(_reconcilingLayouts != nullptr ? *_reconcilingLayouts : settled,
                                   _shown->daemonWorkspace);
    if (only.empty())
        return;
    auto const seeding = _shown->selection == TabSelection::Seed && !only.begin()->second->tabs.empty();
    applyRemoteLayout(
        _sessions, local, *_controller, only, _shown->daemonWorkspace, std::nullopt, _shown->selection);
    // Its last pane closed with the layout, the window may have gone meanwhile.
    if (!_shown || _shown->local != local)
        return;
    if (seeding)
    {
        _shown->selection = TabSelection::Keep;
        // A window a tab was torn off to activates that tab, once.
        if (auto* const own = std::get_if<OpeningRole>(&_role); own != nullptr && own->opening.activeTab)
            if (auto const tab =
                    _controller->tabPairedWith(*std::exchange(own->opening.activeTab, std::nullopt)))
                _sessions.model().activateTab(local, *tab);
    }
    _sessions.deliverRestoreSummaryTo(local);
    // A jump that selected this workspace shows its pane now that the layout is here.
    if (auto* const controller = _sessions.controllerDisplaying(local))
        controller->completePendingJump();
}

void DaemonLink::openWindowForMovedPanes()
{
    auto const* const pending = _sessions.livePendingRemoteMove();
    if (pending == nullptr || pending->binding != this || pending->landing != window::MoveLanding::NewWindow
        || pending->phase != session::TerminalSessionManager::PendingRemoteMove::Phase::Landing
        || pending->request == _movedWindowRequest)
        return;
    // The workspace the daemon made for what was dropped outside every window: the one holding it all, and
    // not the one this window displays.
    auto const layouts = _controller->settledLayouts();
    auto const holdsAll = [&](auto const& entry) {
        auto sessions = std::vector<uint64_t> {};
        for (auto const& tab: entry.second->tabs)
            window::forEachLeaf(tab.root, [&sessions](vthost::proto::WirePane const& leaf) {
                sessions.push_back(leaf.session);
            });
        return std::ranges::all_of(
            pending->sessions, [&](uint64_t session) { return std::ranges::contains(sessions, session); });
    };
    auto const made = std::ranges::find_if(layouts, [&](auto const& entry) {
        return (!_shown || entry.first != _shown->daemonWorkspace) && holdsAll(entry);
    });
    if (made == layouts.end())
        return;
    auto config = _siblingTemplate;
    config.controller.intent = vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Select,
                                                             .workspace = made->first };
    config.controller.resumeFallback = NativeController::ResumeFallback::Nothing;
    config.opening = WindowOpening { .activeTab = std::nullopt,
                                     .screen = _sessions.stageRemoteMoveWindow(pending->sessions),
                                     .askedBy = std::nullopt };
    // Once: the new window's reconcile completes the move, raising and announcing it there.
    _movedWindowRequest = pending->request;
    _host.startWindowLink(std::move(config));
}

WindowAdoption DaemonLink::bindPendingWindow(window::WindowController& window)
{
    requireGuiThread();
    if (!isLive() || _shown || _pendingWindow)
        return WindowAdoption::Declined;
    auto const local = window.displayedWorkspace();

    // The window this link opened for itself claims the workspace its connection selects.
    if (_ownWindow == OwnWindow::Opening)
    {
        bindDaemonWorkspace(followedSelection(), local);
        return WindowAdoption::Adopted;
    }

    // The boot (first) OS window in attach mode ADOPTS the selected workspace instead of authoring a fresh
    // tab. Claiming it here makes Main.qml skip win.createNewTab(), which in attach mode would author a
    // spurious extra tab on the daemon. Bound now when the daemon already selected one; otherwise still
    // claimed, and the reconcile binds it once the first layout arrives.
    if (_attachBoot == session::AttachBoot::Pending && isBootLink())
    {
        // Before anything is realized in it: what it makes is made through this link.
        _sessions.bindWindow(local, *this);
        if (auto const selected = followedSelection();
            selected != 0 && _controller->settledLayout(selected) != nullptr)
            bindDaemonWorkspace(selected, local);
        else
            _pendingWindow = local;
        return WindowAdoption::Adopted;
    }

    return WindowAdoption::Declined;
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
            // What a given-up switch selected again is the connection's own: the one sought now resumes it.
            if (_controller->connectionState() != vthost::client::ConnectionState::Connected)
                _revertTo.reset();
            // A switch fails when the connection goes before the workspace it waits for is shown.
            if (_switch || arrivalDue())
            {
                noteConnectionForSwitch();
                if (!isLive())
                    return;
            }
            // A link opening a window of its own awaits nobody's pollAttached(): it settles as it connects,
            // or learns the daemon has no such workspace (NativeController's failed connect, which stays up).
            // So does a link a switch started.
            if (isBootLink() || _state != AttachState::Connecting)
                return;
            if (auto const failure = _controller->failure(); !failure.empty())
            {
                noteOpeningFailed(failure);
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
    if (!isLive() || !_shown || !std::ranges::contains(daemonWorkspaces, _shown->daemonWorkspace))
        return;
    // The workspace the connection selects instead -- the daemon's choice, or a restarted daemon's own, which
    // may even carry the lost one's id -- is displayed in its place: the window stays.
    if (auto const selected = _controller->selectedWorkspace(); selected != 0 && holdsTab(selected))
    {
        switchTo(selected);
        return;
    }
    // Nothing selected: the window has nothing left to display. Before the close, which closes their ptys:
    // their shells did not exit, the connection to them went. @see TerminalSession::noteTransportLost
    for (auto* session: _sessions.sessionsOfWindow(_shown->local))
        session->noteTransportLost();
    if (auto* const controller = _sessions.controllerDisplaying(_shown->local))
        controller->closeWindow();
}

void DaemonLink::bindDaemonWorkspace(std::uint64_t daemonWorkspace, vtworkspace::WorkspaceId local)
{
    requireGuiThread();
    _attachBoot = session::AttachBoot::Adopted;
    _shown = Shown { .daemonWorkspace = daemonWorkspace, .local = local, .selection = TabSelection::Seed };
    // The window is open: whatever becomes of the link now, the window it was asked from has nothing to hear.
    if (auto* const own = std::get_if<OpeningRole>(&_role))
        own->opening.askedBy.reset();
    _sessions.bindWindow(local, *this);
    applyShownLayout();
    // The daemon names the window that raises by the focus each one reports: a window focused as it comes
    // says so at once.
    if (_shown && _sessions.focusedWindow() == local)
        _controller->reportFocus(vthost::proto::WindowFocus::Focused);
}

void DaemonLink::noteOpeningFailed(std::string const& reason)
{
    auto* const own = std::get_if<OpeningRole>(&_role);
    if (own == nullptr || _shown)
        return;
    auto const askedBy = std::exchange(own->opening.askedBy, std::nullopt);
    auto* const asking = askedBy ? _sessions.controllerOf(*askedBy) : nullptr;
    if (asking == nullptr)
        return;
    asking->notices().post(window::windowLinkFailedNotice(reason));
}

void DaemonLink::raiseElsewhere()
{
    if (!isLive())
        return;
    // Judged as each arrived (NativeController::takeRaised), so each is raised here once, by the window that
    // held the role then, whatever the role is now.
    auto const raised = _controller->takeRaised();
    if (raised.empty())
        return;
    auto const notices = _host.elsewhereNotices();
    auto const directory = _controller->directory();
    auto ring = RingBell::No;
    // Out of every window's sight by definition: decided as a hidden pane's are.
    for (auto const& summary: raised)
    {
        if (summary.bell == vthost::proto::PaneBell::Rang)
            ring = RingBell::Yes;
        for (auto const& notice: summary.notifications)
            notifyElsewhere(notice, notices.notifier);
        if (summary.finished
            && noticeFinishedElsewhere(
                   summary.session, *summary.finished, tabLabelOf(directory, summary.session), notices)
                   == RingBell::Yes)
            ring = RingBell::Yes;
    }
    // Once per turn, however many rang, as a pane coalesces its own bells.
    if (ring == RingBell::Yes)
        ringWindowBell(notices.bell);
}

void DaemonLink::ringWindowBell(config::Bell const& bell)
{
    auto* const controller = _shown ? _sessions.controllerDisplaying(_shown->local) : nullptr;
    if (controller == nullptr)
        return;
    // The window rings, as it does for a finish in one of its own background tabs: the pane has no
    // SessionChrome here to ring it.
    controller->requestBell(QString::fromStdString(platform::bellSourceOf(bell.sound)),
                            static_cast<double>(bell.volume));
    if (bell.alert)
        controller->requestAlert();
}

void DaemonLink::expireRestoreSummaryAfterLayout()
{
    requireGuiThread();
    if (!isLive())
        return;
    auto const layout = _controller->workspaceIds().empty() ? session::LayoutWorkspaces::None
                                                            : session::LayoutWorkspaces::Some;
    auto const spawn = _ownWindow == OwnWindow::Opening && !_shown ? session::AttachSpawn::Spawning
                                                                   : session::AttachSpawn::Idle;
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
    ///         can name (`--connect-tcp`), by the transport they share -- every further link to a daemon
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

} // namespace contour::remote
