// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/DaemonPresence.hpp>
#include <contour/remote/ElsewhereRaise.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/WindowWire.hpp>
#include <contour/window/TaskbarAttentionTally.hpp>

#include <core/Assert.hpp>

#include <QtCore/QObject>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

namespace contour::remote
{

namespace
{
    using vthost::client::ConnectionState;
    using window::DaemonLinkState;

    /// @return Whether a connection in @p state has given up and dials nothing more by itself.
    [[nodiscard]] bool hasGivenUp(ConnectionState state) noexcept
    {
        return state == ConnectionState::Failed || state == ConnectionState::Refused;
    }

    /// @return The tab @p tab of @p directory; null for none.
    [[nodiscard]] window::DirectoryTab const* tabOf(window::WorkspaceDirectory const& directory,
                                                    window::TabRef tab)
    {
        auto const workspace =
            std::ranges::find(directory.workspaces, tab.workspace, &window::DirectoryWorkspace::id);
        if (workspace == directory.workspaces.end())
            return nullptr;
        auto const found = std::ranges::find(workspace->tabs, tab.tab, &window::DirectoryTab::id);
        return found != workspace->tabs.end() ? &*found : nullptr;
    }

    /// @return The daemons @p links are on.
    [[nodiscard]] std::set<window::DaemonRef> windowsOnOf(std::vector<WindowLinkView> const& links)
    {
        return std::ranges::to<std::set>(links | std::views::transform(&WindowLinkView::daemon));
    }

    /// @return Whether @p daemon is of @p kind.
    [[nodiscard]] bool isOfKind(window::DaemonRef const& daemon, window::DaemonRefKind kind)
    {
        auto const parsed = window::parseDaemonRef(daemon);
        return parsed && parsed->first == kind;
    }
} // namespace

std::string daemonNameOf(window::DaemonRef const& daemon)
{
    auto const parsed = window::parseDaemonRef(daemon);
    if (!parsed)
        return daemon.value;
    auto const& [kind, name] = *parsed;
    switch (kind)
    {
        case window::DaemonRefKind::Local:
        case window::DaemonRefKind::Host: return std::string { name };
        case window::DaemonRefKind::Link: break;
    }
    return std::format("link {}", name);
}

std::vector<WindowLinkView> mergedLinks(std::vector<WindowLinkView> links)
{
    auto const isLink = [](window::DaemonRef const& daemon) {
        return isOfKind(daemon, window::DaemonRefKind::Link);
    };
    // Decided before any is renamed: the name a run goes by is its first link's that is not a `link:` one,
    // else the least `link:` name.
    auto names = std::map<window::DaemonInstance, window::DaemonRef> {};
    for (auto const& link: links)
    {
        if (link.instance == window::DaemonInstance {})
            continue; // the daemon has not said which run it is
        auto const [named, inserted] = names.try_emplace(link.instance, link.daemon);
        if (!inserted && isLink(named->second) && (!isLink(link.daemon) || link.daemon < named->second))
            named->second = link.daemon;
    }
    for (auto& link: links)
        if (isLink(link.daemon))
            if (auto const named = names.find(link.instance); named != names.end())
                link.daemon = named->second;
    return links;
}

DaemonPresence::DaemonPresence(PresenceDeps deps): _deps(std::move(deps))
{
}

DaemonPresence::~DaemonPresence() = default;

void DaemonPresence::settingsChanged()
{
    _settings.reset();
    refresh();
}

void DaemonPresence::shutdown()
{
    _lifecycle = Lifecycle::ShutDown;
    _listed.clear();
    for (auto& observer: _observers | std::views::values)
        detach(observer);
    _observers.clear();
    // Each was asked to stop above, so their detaches run side by side: waited for here, one after another.
    for (auto const& controller: std::exchange(_retired, {}))
        controller->stop();
    notify();
}

ListingSettings const& DaemonPresence::settings()
{
    if (!_settings)
        _settings = _deps.settings();
    return *_settings;
}

std::vector<WindowLinkView> DaemonPresence::windowLinks() const
{
    return mergedLinks(_deps.windowLinks());
}

void DaemonPresence::refresh()
{
    if (_lifecycle == Lifecycle::ShutDown)
        return;
    startDiscovery();
    reconcile();
    if (_deps.visibility() == AppVisibility::Visible)
    {
        // What was not redialled while no window was visible is now.
        for (auto& [daemon, observer]: _observers)
            if (observer.controller && hasGivenUp(observer.controller->connectionState())
                && isRetryable(observer))
                scheduleRedial(daemon, observer);
        armRefresh();
    }
    notify();
}

void DaemonPresence::linkSetChanged()
{
    // A link just started is not attached yet: its daemon's observer feeds the section until it is, and
    // linkChanged() stops it once the link's windows are on the daemon.
    reconcile();
    notify();
}

void DaemonPresence::linkChanged()
{
    // Most changes are a link's directory: what is listed is decided again only when the daemons the links
    // are on changed, which is what the listing depends on.
    auto const links = windowLinks();
    if (windowsOnOf(links) != _windowsOn)
        reconcile(links);
    notify();
}

void DaemonPresence::processFocusChanged(window::WindowFocusState focus)
{
    _focus = wireFocusOf(focus);
    for (auto const& observer: _observers | std::views::values)
        announceFocus(observer);
}

std::vector<cli::DiscoveredDaemon> DaemonPresence::localDaemons() const
{
    return _discovered.value_or(std::vector<cli::DiscoveredDaemon> {});
}

std::vector<window::DaemonRef> DaemonPresence::listed() const
{
    return std::ranges::to<std::vector>(_listed | std::views::transform(&ListedDaemon::daemon));
}

std::unique_ptr<NativeController> DaemonPresence::adoptObserver(window::DaemonRef const& daemon)
{
    auto const found = _observers.find(daemon);
    if (found == _observers.end() || !found->second.controller
        || found->second.controller->connectionState() != ConnectionState::Connected)
        return nullptr;
    auto adopted = std::move(found->second.controller);
    // Its receiver goes with the entry: nothing of the presence listens to it any more.
    _observers.erase(found);
    return adopted;
}

void DaemonPresence::giveBack(window::DaemonRef const& daemon, std::unique_ptr<NativeController> demoted)
{
    if (!demoted)
        return;
    // Stopping it, or keeping it as an observer, would end the panes still bound under their window.
    Require(!demoted->bindsPanes());
    auto const links = windowLinks();
    auto const windowIsOn = std::ranges::contains(links, daemon, &WindowLinkView::daemon);
    auto const found = _observers.find(daemon);
    auto const hasObserver = found != _observers.end() && found->second.controller != nullptr;
    auto const keep = _lifecycle == Lifecycle::Running
                      && demoted->connectionState() == ConnectionState::Connected
                      && demoted->role() == vthost::proto::ConnectionRole::Observe && !windowIsOn
                      && isListed(daemon) && !hasObserver;
    // From now on it dials as any observer does: asking no one, starting no daemon, and recording why a
    // dial failed.
    auto recorder = std::shared_ptr<FailureRecordingTransport> {};
    if (keep)
        if (auto transport = _deps.observerTransport(daemon))
            recorder = std::make_shared<FailureRecordingTransport>(*std::move(transport));
    if (!recorder || !demoted->demote(recorder))
    {
        retire(std::move(demoted));
        return;
    }
    auto& observer = _observers[daemon];
    observer = Observer {};
    observer.controller = std::move(demoted);
    observer.recorder = std::move(recorder);
    watch(daemon, observer);
    announceFocus(observer);
    notify();
}

std::vector<window::DaemonSection> DaemonPresence::sections() const
{
    auto const links = windowLinks();
    return std::ranges::to<std::vector>(_listed | std::views::transform([&](ListedDaemon const& daemon) {
                                            return sectionOf(daemon, links);
                                        }));
}

window::DaemonSection DaemonPresence::sectionOf(ListedDaemon const& daemon,
                                                std::vector<WindowLinkView> const& links) const
{
    auto section = window::DaemonSection {};
    section.daemon = daemon.daemon;
    section.locality = daemon.locality;
    section.name = daemonNameOf(daemon.daemon);

    if (auto const first = std::ranges::find(links, daemon.daemon, &WindowLinkView::daemon);
        first != links.end())
    {
        // Any of the window links: they read the same daemon.
        section.instance = first->instance;
        section.name = first->name.empty() ? section.name : first->name;
        section.state = first->state;
        section.directory = first->directory;
        section.feed = window::SectionFeed::WindowLink;
        for (auto const& link: links)
            if (link.daemon == daemon.daemon)
                section.shownElsewhere.insert(link.shownWorkspaces.begin(), link.shownWorkspaces.end());
    }
    else if (auto const found = _observers.find(daemon.daemon); found != _observers.end())
    {
        auto const& observer = found->second;
        section.feed = window::SectionFeed::Observer;
        if (observer.refusal)
        {
            section.state = observer.refusal->state;
            section.reason = observer.refusal->reason;
        }
        else if (auto const* const controller = observer.controller.get())
        {
            section.instance = window::DaemonInstance { controller->daemonInstance().value_or(0) };
            section.directory = controller->directory();
            auto const lastFailure = observer.recorder ? observer.recorder->lastFailure()
                                                       : std::optional<vthost::TransportErrorKind> {};
            section.state =
                linkStateOf(controller->connectionState(), lastFailure, controller->observeSupport());
            // A dial that may pass is tried again: the row says so rather than that it failed.
            if (hasGivenUp(controller->connectionState()) && isRetryable(observer))
                section.state = DaemonLinkState::Reconnecting;
            else if (section.state == DaemonLinkState::Failed || section.state == DaemonLinkState::NeedsInput)
                section.reason = controller->failure();
        }
    }

    // A local daemon discovery no longer finds serving is not running, unless a live link knows better.
    if (daemon.locality == window::DaemonLocality::Local && _discovered && !isRunningLocal(daemon.daemon))
        section.state = window::precedenceOf(section.state, DaemonLinkState::NotRunning);
    return section;
}

void DaemonPresence::connect(window::DaemonRef const& daemon, vtworkspace::WorkspaceId askingWindow)
{
    if (!isListed(daemon) || _windowsOn.contains(daemon))
        return;
    if (auto const parsed = window::parseDaemonRef(daemon);
        !parsed || parsed->first == window::DaemonRefKind::Link)
        return;
    auto& observer = _observers[daemon];
    observer.redials = 0;
    _awaitingStart.erase(daemon);
    auto transport = _deps.interactiveTransport(daemon, askingWindow);
    if (transport)
        launch(daemon, observer, *std::move(transport));
    else
        refuse(observer, std::move(transport.error()));
    notify();
}

void DaemonPresence::disconnect(window::DaemonRef const& daemon)
{
    auto const found = _observers.find(daemon);
    if (found == _observers.end() || !found->second.controller
        || found->second.controller->connectionState() != ConnectionState::Connecting)
        return;
    // The observer stays, holding nothing: the section reads Not connected, and neither a redial nor a
    // refresh dials it again; connect() does.
    auto& observer = found->second;
    detach(observer);
    observer.recorder.reset();
    observer.redial = RedialState::Idle;
    observer.liveness = LocalLiveness::Unknown;
    notify();
}

void DaemonPresence::start(window::DaemonRef const& daemon)
{
    auto const parsed = window::parseDaemonRef(daemon);
    if (!parsed || parsed->first != window::DaemonRefKind::Local || !isListed(daemon))
        return;
    // Dialled afresh once discovery finds it serving: not before, which would find it absent.
    if (auto const found = _observers.find(daemon); found != _observers.end())
    {
        retire(std::move(found->second.controller));
        _observers.erase(found);
    }
    _awaitingStart.insert(daemon);
    _deps.startDaemon(daemon);
    notify();
}

void DaemonPresence::remove(window::DaemonRef const& daemon)
{
    _removed.insert(daemon.value);
    reconcile();
    notify();
}

void DaemonPresence::setListed(window::DaemonRef const& daemon, window::SidebarListingChoice choice)
{
    // A `link:` daemon is named by a link of this run: a choice kept for it would name another daemon in
    // the next one. Its window being on it lists it anyway.
    if (!window::parseDaemonRef(daemon) || isOfKind(daemon, window::DaemonRefKind::Link))
        return;
    _deps.updateListing([&](config::SidebarDaemons& daemons) {
        switch (choice)
        {
            case window::SidebarListingChoice::Listed:
                daemons.shown.insert(daemon.value);
                daemons.hidden.erase(daemon.value);
                break;
            case window::SidebarListingChoice::Unlisted:
                daemons.hidden.insert(daemon.value);
                daemons.shown.erase(daemon.value);
                break;
        }
    });
    _settings.reset();
    reconcile();
    notify();
}

void DaemonPresence::renameWorkspace(window::DaemonRef const& daemon,
                                     window::DaemonInstance instance,
                                     uint64_t workspace,
                                     std::string name)
{
    if (auto* const controller = controllerFor(daemon, instance))
        std::ignore = controller->requestRemoteRenameWorkspace(workspace, name);
}

void DaemonPresence::renameTab(window::DaemonRef const& daemon,
                               window::DaemonInstance instance,
                               window::TabRef tab,
                               std::string name)
{
    auto* const controller = controllerFor(daemon, instance);
    if (controller == nullptr)
        return;
    // The daemon names a tab by any session of it.
    auto const directory = controller->directory();
    if (auto const* const found = tabOf(directory, tab); found != nullptr && !found->panes.empty())
        std::ignore = controller->requestRemoteTabRename(found->panes.front().key.value, name);
}

void DaemonPresence::setWorkspaceColor(window::DaemonRef const& daemon,
                                       window::DaemonInstance instance,
                                       uint64_t workspace,
                                       std::optional<vtbackend::RGBColor> color)
{
    if (auto* const controller = controllerFor(daemon, instance))
        std::ignore = controller->requestRemoteWorkspaceColor(workspace, color);
}

void DaemonPresence::closeTab(window::DaemonRef const& daemon,
                              window::DaemonInstance instance,
                              window::TabRef tab)
{
    auto* const controller = controllerFor(daemon, instance);
    if (controller == nullptr)
        return;
    auto const directory = controller->directory();
    if (auto const* const found = tabOf(directory, tab))
        for (auto const& pane: found->panes)
            controller->requestClosePane(pane.key.value);
}

void DaemonPresence::closeWorkspace(window::DaemonRef const& daemon,
                                    window::DaemonInstance instance,
                                    uint64_t workspace)
{
    if (auto* const controller = controllerFor(daemon, instance))
        std::ignore = controller->requestRemoteCloseWorkspace(workspace);
}

void DaemonPresence::markSeen(window::DaemonRef const& daemon,
                              window::DaemonInstance instance,
                              window::PaneKey pane,
                              window::PaneSeen seen)
{
    if (auto* const controller = controllerFor(daemon, instance))
        controller->markSeen(pane.value, wireSeenOf(seen));
}

window::SectionsSubscription DaemonPresence::onChanged(std::function<void()> changed)
{
    return _subscribers.subscribe(std::move(changed));
}

void DaemonPresence::reconcile()
{
    reconcile(windowLinks());
}

void DaemonPresence::reconcile(std::vector<WindowLinkView> const& links)
{
    if (_lifecycle == Lifecycle::ShutDown)
        return;
    _windowsOn = windowsOnOf(links);
    if (_windowsOn.empty())
        _listed.clear(); // With no window on a daemon nothing is listed: the settings are not even read.
    else
    {
        auto running = std::vector<std::string> {};
        if (_discovered)
            std::ranges::transform(*_discovered, std::back_inserter(running), &cli::DiscoveredDaemon::label);
        _listed = listedDaemons(PresenceInputs {
            .runningLocal = std::move(running),
            .seenLocalThisRun = _seenLocal,
            .settings = settings(),
            .removedThisRun = _removed,
            .windowsOn = std::ranges::to<std::vector>(_windowsOn),
        });
    }
    for (auto const& daemon: _listed)
        if (auto const parsed = window::parseDaemonRef(daemon.daemon);
            parsed && parsed->first == window::DaemonRefKind::Local)
            _seenLocal.emplace(parsed->second);

    // An observer is wanted only for a listed daemon no window link is on.
    auto const wanted = [&](window::DaemonRef const& daemon) {
        return isListed(daemon) && !_windowsOn.contains(daemon) && !_awaitingStart.contains(daemon);
    };
    auto const unwanted = std::ranges::to<std::vector>(
        _observers | std::views::keys
        | std::views::filter([&](window::DaemonRef const& daemon) { return !wanted(daemon); }));
    for (auto const& daemon: unwanted)
    {
        retire(std::move(_observers.at(daemon).controller));
        _observers.erase(daemon);
    }
    for (auto const& daemon: _listed)
        if (wanted(daemon.daemon) && !_observers.contains(daemon.daemon))
            startObserver(daemon.daemon, _observers[daemon.daemon]);
    pruneRetired();
}

void DaemonPresence::startObserver(window::DaemonRef const& daemon, Observer& observer)
{
    auto transport = _deps.observerTransport(daemon);
    if (transport)
        launch(daemon, observer, *std::move(transport));
    else
        refuse(observer, std::move(transport.error()));
}

void DaemonPresence::refuse(Observer& observer, DialRefusal refusal)
{
    detach(observer);
    observer.refusal = std::move(refusal);
    observer.liveness = LocalLiveness::Unknown;
}

void DaemonPresence::announceFocus(Observer const& observer) const
{
    if (_focus && observer.controller)
        observer.controller->reportShown(*_focus, {});
}

void DaemonPresence::launch(window::DaemonRef const& daemon,
                            Observer& observer,
                            std::shared_ptr<vthost::ITransportFactory> transport)
{
    detach(observer);
    if (_lifecycle == Lifecycle::ShutDown)
        return;
    observer.recorder = std::make_shared<FailureRecordingTransport>(std::move(transport));
    observer.controller = _deps.makeObserver(daemon, observer.recorder, _deps.redialBackoff);
    if (!observer.controller)
    {
        observer.refusal =
            DialRefusal { .state = DaemonLinkState::Failed, .reason = "no observer could be made" };
        return;
    }
    observer.refusal.reset();
    observer.redial = RedialState::Idle;
    observer.liveness = LocalLiveness::Unknown;
    watch(daemon, observer);
    announceFocus(observer);
    observer.controller->startConnecting();
}

void DaemonPresence::watch(window::DaemonRef const& daemon, Observer& observer)
{
    observer.receiver = std::make_unique<QObject>();
    auto* const controller = observer.controller.get();
    QObject::connect(
        controller,
        &NativeController::connectionStateChanged,
        observer.receiver.get(),
        [this, daemon, controller] { observerStateChanged(daemon, controller); },
        Qt::QueuedConnection);
    QObject::connect(
        controller,
        &NativeController::directoryChanged,
        observer.receiver.get(),
        [this, daemon, controller] {
            raiseFor(daemon, *controller);
            notify();
        },
        Qt::QueuedConnection);
    // Its daemon's waiting tabs count in this process only while it holds the raise role (notify()).
    QObject::connect(
        controller, &NativeController::raiseRoleChanged, observer.receiver.get(), [this] { notify(); });
}

void DaemonPresence::observerStateChanged(window::DaemonRef const& daemon, NativeController const* controller)
{
    auto const found = _observers.find(daemon);
    if (found == _observers.end() || found->second.controller.get() != controller)
        return;
    auto& observer = found->second;
    auto const state = controller->connectionState();
    if (state == ConnectionState::Connected)
    {
        observer.redials = 0;
        observer.liveness = LocalLiveness::Served;
    }
    else if (hasGivenUp(state))
    {
        // Gone after it served: what a discovery that finds it serving again answers, however soon that is.
        if (observer.liveness == LocalLiveness::Served && observer.recorder
            && observer.recorder->lastFailure() == vthost::TransportErrorKind::DaemonAbsent)
            observer.liveness = LocalLiveness::GaveUpAbsent;
        if (isRetryable(observer))
            scheduleRedial(daemon, observer);
        // A local daemon whose observer gave up may have stopped: a discovery says so soon, and a later one
        // that finds it serving again dials it once more.
        if (isOfKind(daemon, window::DaemonRefKind::Local))
            startDiscovery();
    }
    notify();
}

void DaemonPresence::scheduleRedial(window::DaemonRef const& daemon, Observer& observer)
{
    // While no window is visible nothing is redialled: refresh() schedules it once one is.
    if (observer.redial == RedialState::Scheduled || _deps.visibility() != AppVisibility::Visible)
        return;
    observer.redial = RedialState::Scheduled;
    auto const delay = vthost::client::backoffDelay(_deps.redialBackoff, ++observer.redials);
    _deps.refreshTimer(
        delay, [alive = std::weak_ptr { _alive }, this, daemon, controller = observer.controller.get()] {
            if (alive.lock())
                redial(daemon, controller);
        });
}

void DaemonPresence::redial(window::DaemonRef const& daemon, NativeController const* controller)
{
    auto const found = _observers.find(daemon);
    if (found == _observers.end() || found->second.controller.get() != controller
        || !hasGivenUp(controller->connectionState()))
        return;
    startObserver(daemon, found->second);
    notify();
}

void DaemonPresence::detach(Observer& observer)
{
    observer.receiver.reset();
    retire(std::move(observer.controller));
}

void DaemonPresence::retire(std::unique_ptr<NativeController> controller)
{
    if (!controller)
        return;
    controller->stopInBackground([] {});
    _retired.push_back(std::move(controller));
    pruneRetired();
}

void DaemonPresence::pruneRetired()
{
    std::erase_if(_retired, [](auto const& controller) {
        return !controller->isWindingDown() && !controller->bindsPanes();
    });
}

void DaemonPresence::startDiscovery()
{
    if (_discovery == Discovery::Running)
        return;
    _discovery = Discovery::Running;
    // Off the GUI thread: each socket is probed.
    _jobs.run([discovery = _deps.discovery] { return discovery->discover(); },
              [this](std::vector<cli::DiscoveredDaemon> found) {
                  _discovery = Discovery::Idle;
                  // A daemon start() started is dialled once it is found serving.
                  std::erase_if(_awaitingStart, [&](window::DaemonRef const& daemon) {
                      auto const parsed = window::parseDaemonRef(daemon);
                      return parsed
                             && std::ranges::contains(found, parsed->second, &cli::DiscoveredDaemon::label);
                  });
                  _discovered = std::move(found);
                  reconcile();
                  redialRestartedLocals();
                  notify();
              });
}

void DaemonPresence::armRefresh()
{
    if (_refreshArm == RefreshArm::Armed)
        return;
    _refreshArm = RefreshArm::Armed;
    _deps.refreshTimer(PresenceRefreshInterval, [alive = std::weak_ptr { _alive }, this] {
        if (!alive.lock())
            return;
        _refreshArm = RefreshArm::Idle;
        // While no window is visible nothing is refreshed, and nothing re-armed: refresh() arms it again.
        // The settings are read again: another process may have changed the listing choices.
        if (_deps.visibility() == AppVisibility::Visible)
            settingsChanged();
    });
}

void DaemonPresence::raiseFor(window::DaemonRef const& daemon, NativeController& controller)
{
    // Handed out only while the daemon has no viewer and picked this process's observer (spec §2): each once,
    // across every process observing the daemon.
    auto const raised = controller.takeRaised();
    if (raised.empty())
        return;
    auto const notices = _deps.elsewhereNotices();
    auto const raising =
        RaisingDaemon { .daemon = daemon,
                        .instance = window::DaemonInstance { controller.daemonInstance().value_or(0) },
                        .name = daemonNameOf(daemon) };
    if (raiseElsewhere(raised, controller.directory(), notices, raising) == RingBell::Yes)
        _deps.ringLastFocusedWindow(notices.bell);
}

void DaemonPresence::recountObservedWaiting()
{
    // A daemon a window is on has no observer: its window counts its tabs. One observed by several processes
    // counts in the one that raises for it, as its alerts come from there (spec §8).
    auto waiting = 0u;
    for (auto const& observer: _observers | std::views::values)
        if (observer.controller && observer.controller->raiseRole() == vthost::proto::RaiseRole::Raise)
            waiting += window::tabsWaitingIn(observer.controller->directory(), std::nullopt);
    if (std::exchange(_observedWaiting, waiting) != waiting)
        _deps.observedWaitingChanged(waiting);
}

void DaemonPresence::notify()
{
    recountObservedWaiting();
    // Once shut down, the application is going: a sidebar told now would rebuild after the windows' world
    // is gone.
    if (_lifecycle == Lifecycle::ShutDown)
        return;
    _subscribers.notify();
}

void DaemonPresence::redialRestartedLocals()
{
    // A local daemon known to have gone, whose observer gave up, and that serves now -- started again outside
    // Contour, perhaps before any discovery saw it gone -- is dialled again, as an observer dials: never
    // prompting, never starting a daemon. One never seen gone waits for Connect, as a remote one does: a dial
    // that finds nothing where discovery finds a daemon is not retried at every discovery it asks for.
    for (auto& [daemon, observer]: _observers)
    {
        auto const idle =
            observer.refusal || !observer.controller || hasGivenUp(observer.controller->connectionState());
        if (!isOfKind(daemon, window::DaemonRefKind::Local) || !idle)
            continue;
        if (!isRunningLocal(daemon))
            observer.liveness = LocalLiveness::GaveUpAbsent;
        else if (observer.liveness == LocalLiveness::GaveUpAbsent)
            startObserver(daemon, observer);
    }
}

bool DaemonPresence::isRunningLocal(window::DaemonRef const& daemon) const
{
    auto const parsed = window::parseDaemonRef(daemon);
    return parsed && parsed->first == window::DaemonRefKind::Local && _discovered
           && std::ranges::contains(*_discovered, parsed->second, &cli::DiscoveredDaemon::label);
}

bool DaemonPresence::isListed(window::DaemonRef const& daemon) const
{
    return std::ranges::contains(_listed, daemon, &ListedDaemon::daemon);
}

bool DaemonPresence::isRetryable(Observer const& observer)
{
    if (!observer.controller || !observer.recorder
        || observer.controller->observeSupport() == ObserveSupport::TooOld)
        return false;
    // The transport's failure says most. A connection it made that the daemon did not take says the rest:
    // one never answered (Protocol) may be answered later, one refused (Refused) will not be.
    auto const failure =
        observer.recorder->lastFailure().or_else([&] { return observer.controller->failureKind(); });
    return failure && !linkStateOfFailure(*failure);
}

NativeController* DaemonPresence::controllerFor(window::DaemonRef const& daemon,
                                                window::DaemonInstance instance) const
{
    if (!isListed(daemon))
        return nullptr;
    for (auto const& link: windowLinks())
        if (link.daemon == daemon && link.controller != nullptr)
            return link.instance == instance ? link.controller : nullptr;
    auto const found = _observers.find(daemon);
    if (found == _observers.end() || !found->second.controller)
        return nullptr;
    auto* const controller = found->second.controller.get();
    return controller->daemonInstance().value_or(0) == instance.value ? controller : nullptr;
}

} // namespace contour::remote
