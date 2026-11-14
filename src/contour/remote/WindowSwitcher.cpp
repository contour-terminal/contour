// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/WindowSwitcher.hpp>
#include <contour/session/PermissionPrompt.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QTimer>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>
#include <variant>

namespace contour::remote
{

namespace
{
    /// What a window says when a switch did not get there, in Attach to Host's words: {} is where, then why.
    constexpr auto SwitchFailedText = std::string_view { "Could not switch to {}: {}." };

    /// What a window says when the user declined a question a switch asked: the window stays as it was.
    constexpr auto SwitchDeclinedText = std::string_view { "Did not switch to {}: {}." };

    /// What a window says when the workspace a switch was to go to could not be created: where, then why.
    constexpr auto CreateFailedText = std::string_view { "Could not create a workspace on {}: {}." };

    /// Why a switch whose target did not show within its deadline failed.
    constexpr auto SwitchSuperseded =
        std::string_view { "a newer switch of the window, or its close, ended it" };
    constexpr auto SwitchTimedOut = std::string_view { "it did not show the workspace in time" };

    /// Why a switch that must land by a time (session::SwitchLandBy) was not started.
    constexpr auto SwitchTooLateText =
        std::string_view { "too little time was left before the program that asked for it stopped waiting" };

    /// @return What a link's first connection selects to show @p workspace: it, or a new one for 0.
    [[nodiscard]] vthost::proto::AttachIntent intentFor(uint64_t workspace)
    {
        if (workspace == 0)
            return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::NewWorkspace,
                                                 .workspace = 0 };
        return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Select,
                                             .workspace = workspace };
    }

} // namespace

SwitchTimer qtSwitchTimer()
{
    return [](std::chrono::milliseconds delay, std::function<void()> fire) {
        QTimer::singleShot(delay, std::move(fire));
    };
}

WindowSwitcher::WindowSwitcher(session::TerminalSessionManager& sessions,
                               ISwitchHost& host,
                               std::unique_ptr<IWorkspaceLister> lister,
                               SwitchPicker picker,
                               SwitchDeadline deadline):
    _sessions(sessions),
    _host(host),
    _lister(std::move(lister)),
    _picker(std::move(picker)),
    _deadline(std::move(deadline)),
    _alive(std::make_shared<WindowSwitcher*>(this))
{
}

WindowSwitcher::~WindowSwitcher() = default;

DaemonLink* WindowSwitcher::linkOf(vtworkspace::WorkspaceId window)
{
    auto const* const binding = _sessions.bindingOf(window);
    if (binding == nullptr)
        return nullptr;
    auto const links = _host.switchableLinks();
    auto const found = std::ranges::find_if(links, [binding](DaemonLink const* link) {
        return static_cast<session::WindowBinding const*>(link) == binding;
    });
    return found != links.end() ? *found : nullptr;
}

std::optional<vtworkspace::WorkspaceId> WindowSwitcher::shownHere(SameDaemon sameDaemon,
                                                                  DaemonAddress const& address,
                                                                  DaemonLink& own,
                                                                  uint64_t daemonWorkspace)
{
    // The links on the target daemon: by its name, and, for the window's own, also every link to the daemon
    // instance it is on (one `--connect-tcp` names no other way).
    auto const ownInstance =
        own.liveController() != nullptr ? own.liveController()->daemonInstance() : std::nullopt;
    auto const onTarget = [&](DaemonLink* link) {
        if (sameDaemon == SameDaemon::Yes)
        {
            if (link == &own)
                return true;
            auto const* const controller = link->liveController();
            if (ownInstance && *ownInstance != 0 && controller != nullptr
                && controller->daemonInstance() == ownInstance)
                return true;
        }
        return address.kind != vthost::proto::SwitchDaemonKind::Same && link->address() == address;
    };
    for (auto* const link: _host.switchableLinks())
        if (onTarget(link))
            if (auto const window = link->windowShowing(daemonWorkspace))
                return window;
    return std::nullopt;
}

void WindowSwitcher::request(vtworkspace::WorkspaceId window,
                             session::SwitchTarget target,
                             session::SwitchMode mode,
                             session::SwitchLandBy landBy,
                             session::SwitchCompletion done)
{
    auto* const own = linkOf(window);
    if (own == nullptr)
    {
        if (_sessions.isInProcessWindow(window))
        {
            switchInProcess(window, target, mode, {}, done);
            return;
        }
        attachLog()("switch: window {} has no daemon behind it; nothing to switch.", window.value);
        done(std::unexpected(std::string { "this window has no daemon behind it" }));
        return;
    }
    // A switch waiting for a tab in an empty workspace ends for a newer one (it is for the user to end,
    // and a window must not stay locked by it); any other switch under way refuses it.
    if (auto const busy = _busy.find(window.value); busy != _busy.end())
    {
        auto const supersede = busy->second.supersede;
        if (!supersede || supersede(SupersedeCause::NewerRequest) == Supersession::Kept)
        {
            done(std::unexpected(std::string { "a switch of this window is under way" }));
            return;
        }
    }
    auto destination = destinationOf(*own, target, window);
    auto request = Request {
        .window = window,
        .target = std::move(target),
        .mode = mode,
        .landBy = landBy,
        .done = std::move(done),
        .sameDaemon = destination.sameDaemon,
        .serial = _nextSerial++,
        .address = destination.address,
        .config = std::nullopt,
    };
    _busy.insert_or_assign(window.value, Busy { .serial = request.serial, .supersede = {} });
    if (!destination.config)
    {
        fail(request, destination.config.error());
        return;
    }
    request.config.emplace(*std::move(destination.config));
    // The window's own daemon: its connection holds the directory of every workspace, control ids and all, so
    // nothing is listed -- but for a workspace named by its control id on a daemon that states none.
    if (request.sameDaemon == SameDaemon::Yes)
    {
        auto const directory = own->liveController() != nullptr ? own->liveController()->directory()
                                                                : window::WorkspaceDirectory {};
        if (resolvesFromDirectory(request.target.workspace, directory))
        {
            auto const rows = workspaceRowsOf(directory, [&](uint64_t daemonWorkspace) {
                return shownHere(SameDaemon::Yes, request.address, *own, daemonWorkspace);
            });
            resolve(std::move(request), rows);
            return;
        }
    }
    auto list = ListTarget { .address = request.address, .transport = request.config->controller.transport };
    _lister->list(std::move(list),
                  [alive = std::weak_ptr { _alive }, request = std::move(request)](auto listed) mutable {
                      auto const self = alive.lock();
                      if (!self)
                          return;
                      auto& switcher = **self;
                      if (!listed && listed.error().kind == ListFailureKind::Declined)
                          switcher.decline(request, listed.error().reason);
                      else if (!listed)
                          switcher.fail(request, listed.error().reason);
                      else
                          switcher.resolveListed(std::move(request), *listed);
                  });
}

WindowSwitcher::Destination WindowSwitcher::destinationOf(DaemonLink& own,
                                                          session::SwitchTarget const& target,
                                                          vtworkspace::WorkspaceId window)
{
    auto const named = daemonAddressOf(target);
    auto const sameDaemon = target.kind == vthost::proto::SwitchDaemonKind::Same || named == own.address()
                                ? SameDaemon::Yes
                                : SameDaemon::No;
    if (sameDaemon == SameDaemon::Yes)
        return Destination { .sameDaemon = sameDaemon,
                             .address = own.address(),
                             .config = own.siblingConfig() };
    return Destination { .sameDaemon = sameDaemon,
                         .address = named,
                         .config = _host.linkConfigFor(named, window) };
}

void WindowSwitcher::listWorkspaces(vtworkspace::WorkspaceId window,
                                    session::SwitchTarget const& daemon,
                                    DaemonWorkspacesListed done)
{
    auto* const own = linkOf(window);
    if (own == nullptr)
    {
        done(std::unexpected(std::string { "this window has no daemon behind it" }));
        return;
    }
    auto destination = destinationOf(*own, daemon, window);
    if (!destination.config)
    {
        done(std::unexpected(destination.config.error()));
        return;
    }
    auto list =
        ListTarget { .address = destination.address, .transport = destination.config->controller.transport };
    _lister->list(std::move(list),
                  [alive = std::weak_ptr { _alive },
                   window,
                   sameDaemon = destination.sameDaemon,
                   address = destination.address,
                   done = std::move(done)](auto listed) {
                      auto const self = alive.lock();
                      if (!self)
                          return;
                      if (!listed)
                      {
                          done(std::unexpected(listed.error().reason));
                          return;
                      }
                      auto& switcher = **self;
                      auto* const link = switcher.linkOf(window);
                      if (link == nullptr)
                      {
                          done(std::unexpected(std::string { "the window went away" }));
                          return;
                      }
                      done(workspaceRowsOf(*listed, [&](uint64_t daemonWorkspace) {
                          return switcher.shownHere(sameDaemon, address, *link, daemonWorkspace);
                      }));
                  });
}

void WindowSwitcher::createWorkspace(vtworkspace::WorkspaceId window,
                                     session::SwitchTarget const& daemon,
                                     std::string label,
                                     session::SwitchMode mode,
                                     session::SwitchCompletion done)
{
    auto* const own = linkOf(window);
    if (own == nullptr)
    {
        if (_sessions.isInProcessWindow(window))
            switchInProcess(window,
                            session::SwitchTarget { .kind = daemon.kind,
                                                    .daemon = daemon.daemon,
                                                    .workspace = session::NewWorkspace {} },
                            mode,
                            std::move(label),
                            done);
        else
            failCreate(window, daemon, "this window has no daemon behind it", done);
        return;
    }
    auto destination = destinationOf(*own, daemon, window);
    if (!destination.config)
    {
        failCreate(window, daemon, destination.config.error(), done);
        return;
    }
    auto target =
        ListTarget { .address = destination.address, .transport = destination.config->controller.transport };
    _lister->create(
        std::move(target),
        std::move(label),
        [alive = std::weak_ptr { _alive },
         window,
         daemon = session::SwitchTarget { .kind = daemon.kind, .daemon = daemon.daemon, .workspace = {} },
         mode,
         done = std::move(done)](auto created) mutable {
            auto const self = alive.lock();
            if (!self)
                return;
            if (!created)
            {
                (*self)->failCreate(window, daemon, created.error(), done);
                return;
            }
            // The new workspace, by the control id the daemon gave it: never mistaken for another.
            daemon.workspace = *created;
            (*self)->request(window, std::move(daemon), mode, session::NoLandingDeadline, std::move(done));
        });
}

std::vector<WorkspaceRow> WindowSwitcher::inProcessRows(vtworkspace::WorkspaceId window)
{
    auto const* const controller = _sessions.controllerDisplaying(window);
    auto const* const source = controller != nullptr ? controller->directorySource() : nullptr;
    return inProcessWorkspaceRowsOf(source != nullptr ? source->directory() : window::WorkspaceDirectory {},
                                    window);
}

void WindowSwitcher::switchInProcess(vtworkspace::WorkspaceId window,
                                     session::SwitchTarget const& target,
                                     session::SwitchMode mode,
                                     std::string label,
                                     session::SwitchCompletion const& done)
{
    auto* const controller = _sessions.controllerDisplaying(window);
    if (controller == nullptr)
    {
        done(std::unexpected(std::string { "the window went away" }));
        return;
    }
    if (target.kind != vthost::proto::SwitchDaemonKind::Same)
    {
        done(std::unexpected(std::string { "this window has no daemon behind it" }));
        return;
    }
    if (mode == session::SwitchMode::Open)
    {
        done(
            std::unexpected(std::string { "this window has no daemon to open a workspace of in a new one" }));
        return;
    }
    auto const rows = inProcessRows(window);
    auto const decision = planSwitch(target, rows, window.value, SameDaemon::Yes);
    if (auto const* const to = std::get_if<SwitchTo>(&decision))
    {
        if (to->workspace == 0)
        {
            if (auto const made = _sessions.newWorkspaceFor(window, std::move(label)); !made)
            {
                done(std::unexpected(std::string { session::describe(made.error()) }));
                return;
            }
        }
        else if (controller->showWorkspace(to->workspace) == window::SelectResult::Refused)
        {
            done(std::unexpected(std::string { "this window cannot show that workspace" }));
            return;
        }
        // What was chosen is what the user types into next.
        controller->focusActiveDisplay();
        done(vthost::proto::SwitchOutcome::Switched);
    }
    else if (std::holds_alternative<SwitchNoop>(decision))
        done(vthost::proto::SwitchOutcome::Switched);
    else if (std::holds_alternative<SwitchPick>(decision))
    {
        // Several workspaces and none named: the user's to pick, among this window's own.
        auto shown = _picker(
            SwitchPickRequest { .window = window, .target = target, .mode = mode, .workspaces = rows });
        if (shown)
            done(vthost::proto::SwitchOutcome::PickerShown);
        else
            done(std::unexpected(std::move(shown.error())));
    }
    else
        done(std::unexpected(std::string { "no such workspace in this window" }));
}

std::vector<WorkspaceRow> WindowSwitcher::windowWorkspaceRows(vtworkspace::WorkspaceId window)
{
    auto* const own = linkOf(window);
    if (own == nullptr)
        return _sessions.isInProcessWindow(window) ? inProcessRows(window) : std::vector<WorkspaceRow> {};
    if (own->liveController() == nullptr)
        return {};
    auto const address = own->address();
    return workspaceRowsOf(own->liveController()->directory(), [&](uint64_t daemonWorkspace) {
        return shownHere(SameDaemon::Yes, address, *own, daemonWorkspace);
    });
}

void WindowSwitcher::resolveListed(Request request, std::vector<vtcontrol::WorkspaceInfo> const& workspaces)
{
    // The window may have closed, or moved to another link, while the daemon was asked.
    auto* const own = linkOf(request.window);
    if (own == nullptr)
    {
        fail(request, "the window went away");
        return;
    }
    auto const rows = workspaceRowsOf(workspaces, [&](uint64_t daemonWorkspace) {
        return shownHere(request.sameDaemon, request.address, *own, daemonWorkspace);
    });
    resolve(std::move(request), rows);
}

void WindowSwitcher::resolve(Request request, std::vector<WorkspaceRow> const& rows)
{
    auto* const own = linkOf(request.window);
    if (own == nullptr)
    {
        fail(request, "the window went away");
        return;
    }
    auto const decision =
        planSwitch(request.target, rows, own->workspaceOf(request.window), request.sameDaemon);
    std::visit(
        [&]<typename T>(T const& decided) {
            if constexpr (std::is_same_v<T, SwitchUnresolved>)
                fail(request, decided.reason);
            else if constexpr (std::is_same_v<T, SwitchNoop>)
            {
                // Open shows it in a new window, but this window shows it already: that is the one to focus.
                if (request.mode == session::SwitchMode::Open)
                    if (auto* const controller = _sessions.controllerDisplaying(request.window))
                        controller->raiseAndActivate();
                succeed(request, vthost::proto::SwitchOutcome::FocusedExisting);
            }
            else if constexpr (std::is_same_v<T, SwitchFocus>)
            {
                if (auto* const controller = _sessions.controllerDisplaying(decided.window))
                    controller->raiseAndActivate();
                succeed(request, vthost::proto::SwitchOutcome::FocusedExisting);
            }
            else if constexpr (std::is_same_v<T, SwitchPick>)
            {
                auto shown = _picker(SwitchPickRequest { .window = request.window,
                                                         .target = request.target,
                                                         .mode = request.mode,
                                                         .workspaces = rows });
                if (!shown)
                    fail(request, shown.error());
                else
                    succeed(request, vthost::proto::SwitchOutcome::PickerShown);
            }
            else
                land(std::move(request), decided);
        },
        decision);
}

void WindowSwitcher::land(Request request, SwitchTo const& to)
{
    auto const workspace = to.workspace;
    auto const window = request.window;
    auto const mode = request.mode;
    auto const sameDaemon = request.sameDaemon;
    auto const serial = request.serial;
    auto const landBy = request.landBy;
    // Listing the target daemon's workspaces may have taken until the switch must have landed: one started
    // now would move a window nobody waits for any more.
    if (landBy && _deadline.clock.now() >= *landBy)
    {
        fail(request, std::string { SwitchTooLateText });
        return;
    }
    // One callback for each outcome, whichever tells it first -- a link, or the deadline: the request moves
    // into it, and the others find it taken.
    auto pending = std::make_shared<std::optional<Request>>(std::move(request));
    auto const take = [pending] {
        return std::exchange(*pending, std::nullopt);
    };
    if (mode == session::SwitchMode::Switch && sameDaemon == SameDaemon::Yes)
    {
        auto* const own = linkOf(window);
        if (own == nullptr)
        {
            fail(*take(), "the window went away");
            return;
        }
        auto selected = own->selectWorkspace(
            workspace, [alive = std::weak_ptr { _alive }, take](std::expected<uint64_t, std::string> landed) {
                auto const self = alive.lock();
                auto taken = take();
                if (!self || !taken)
                    return;
                if (landed)
                    (*self)->succeed(*taken, vthost::proto::SwitchOutcome::Switched);
                else
                    (*self)->fail(*taken, landed.error());
            });
        if (!selected)
        {
            fail(*take(), selected.error());
            return;
        }
        // Undone as a lost connection undoes it: the window's own workspace is selected again, and the link
        // tells the callback above, which fails the request.
        armDeadline(window, serial, landBy, [this, window, take, landBy] {
            auto* const link = linkOf(window);
            if (link != nullptr && expiryOf(link->switchWait(), landBy) == Expiry::Disarmed)
                return Expiry::Disarmed;
            if (link != nullptr)
                link->cancelSwitch(window, std::string { SwitchTimedOut });
            if (auto taken = take())
                fail(*taken, std::string { SwitchTimedOut });
            return Expiry::GaveUp;
        });
        onSupersede(window, serial, [this, window, take](SupersedeCause cause) {
            return supersedeOwn(window, cause, take);
        });
        return;
    }
    // A link of its own: for a new window (Open), or the window's link to another daemon.
    auto config = *(*pending)->config;
    config.controller.intent = intentFor(workspace);
    config.controller.resumeFallback = NativeController::ResumeFallback::Nothing;
    config.opening.reset();
    // The link's id is known once it is made, which is after its callback was: shared with the callback.
    auto const madeAs = std::make_shared<DaemonLinkId>();
    config.arrival = SwitchArrival {
        .mode = mode,
        .landed =
            [alive = std::weak_ptr { _alive }, take, mode, madeAs](auto landed) {
                auto const self = alive.lock();
                auto taken = take();
                if (!self || !taken)
                    return;
                if (!landed)
                    (*self)->fail(*taken, landed.error());
                else if (mode == session::SwitchMode::Open)
                    (*self)->succeed(*taken, vthost::proto::SwitchOutcome::Switched);
                else
                    (*self)->handOver(*taken, *madeAs);
            },
    };
    *madeAs = _host.startSwitchLink(std::move(config)).id();
    // The link is not wanted any more: it ends, telling nothing, and its selection goes with its connection.
    armDeadline(window, serial, landBy, [this, madeAs, take, landBy] {
        auto const links = _host.switchableLinks();
        auto const found = std::ranges::find(links, *madeAs, &DaemonLink::id);
        if (found != links.end() && expiryOf((*found)->switchWait(), landBy) == Expiry::Disarmed)
            return Expiry::Disarmed;
        if (found != links.end())
            (*found)->abandon();
        if (auto taken = take())
            fail(*taken, std::string { SwitchTimedOut });
        return Expiry::GaveUp;
    });
    onSupersede(window, serial, [this, madeAs, take](SupersedeCause cause) {
        return supersedeLinked(*madeAs, cause, take);
    });
}

WindowSwitcher::Supersession WindowSwitcher::supersedeOwn(vtworkspace::WorkspaceId window,
                                                          SupersedeCause cause,
                                                          TakeRequest const& take)
{
    auto* const link = linkOf(window);
    if (cause == SupersedeCause::NewerRequest
        && (link == nullptr || link->switchWait() != SwitchWait::ForFirstTab))
        return Supersession::Kept;
    // Taken first: the link's callback then finds nothing to fail, and no notice is posted.
    auto taken = take();
    if (link != nullptr)
        link->cancelSwitch(window, std::string { SwitchSuperseded });
    if (taken)
        cancel(*taken, std::string { SwitchSuperseded });
    return Supersession::Ended;
}

WindowSwitcher::Supersession WindowSwitcher::supersedeLinked(DaemonLinkId madeAs,
                                                             SupersedeCause cause,
                                                             TakeRequest const& take)
{
    auto const links = _host.switchableLinks();
    auto const found = std::ranges::find(links, madeAs, &DaemonLink::id);
    if (cause == SupersedeCause::NewerRequest
        && (found == links.end() || (*found)->switchWait() != SwitchWait::ForFirstTab))
        return Supersession::Kept;
    auto taken = take();
    if (found != links.end())
        (*found)->abandon();
    if (taken)
        cancel(*taken, std::string { SwitchSuperseded });
    return Supersession::Ended;
}

void WindowSwitcher::onSupersede(vtworkspace::WorkspaceId window,
                                 uint64_t serial,
                                 std::function<Supersession(SupersedeCause)> supersede)
{
    if (auto const busy = _busy.find(window.value); busy != _busy.end() && busy->second.serial == serial)
        busy->second.supersede = std::move(supersede);
}

void WindowSwitcher::windowClosed(vtworkspace::WorkspaceId window)
{
    // Copied: ending the switch erases its entry.
    if (auto const busy = _busy.find(window.value); busy != _busy.end() && busy->second.supersede)
        std::ignore = std::function { busy->second.supersede }(SupersedeCause::WindowClosed);
}

WindowSwitcher::Expiry WindowSwitcher::expiryOf(SwitchWait wait, session::SwitchLandBy landBy) noexcept
{
    return wait == SwitchWait::ForFirstTab && !landBy ? Expiry::Disarmed : Expiry::GaveUp;
}

void WindowSwitcher::armDeadline(vtworkspace::WorkspaceId window,
                                 uint64_t serial,
                                 session::SwitchLandBy landBy,
                                 std::function<Expiry()> expire)
{
    auto const timeout =
        landBy ? std::clamp(std::chrono::ceil<std::chrono::milliseconds>(*landBy - _deadline.clock.now()),
                            std::chrono::milliseconds::zero(),
                            _deadline.timeout)
               : _deadline.timeout;
    // Shared, so that the timer's own copy of what it fires copies a pointer, not what the switch undoes.
    auto const undo = std::make_shared<std::function<Expiry()>>(std::move(expire));
    _deadline.timer(timeout, [alive = std::weak_ptr { _alive }, window, serial, undo] {
        auto const self = alive.lock();
        if (!self)
            return;
        // Ended meanwhile, and maybe followed by another request of the same window.
        auto const busy = (*self)->_busy.find(window.value);
        if (busy == (*self)->_busy.end() || busy->second.serial != serial)
            return;
        // A switch waiting for a tab said so as its target's layout arrived
        // (DaemonLink::noteSwitchTargetSettled()).
        if ((*undo)() == Expiry::GaveUp)
            attachLog()("switch: window {} gave up waiting for its target to show.", window.value);
    });
}

void WindowSwitcher::handOver(Request const& request, DaemonLinkId to)
{
    auto const links = _host.switchableLinks();
    auto const found = std::ranges::find(links, to, &DaemonLink::id);
    auto* const target = found != links.end() ? *found : nullptr;
    auto* const own = linkOf(request.window);
    if (target == nullptr || own == nullptr)
    {
        // The window closed, or was rebound, while the other daemon was connected: nothing to switch.
        if (target != nullptr)
            target->abandon();
        fail(request, "the window went away");
        return;
    }
    if (auto adoptable = target->canAdoptWindow(request.window); !adoptable)
    {
        target->abandon();
        fail(request, adoptable.error());
        return;
    }
    // Told first, while the window is still the old link's: a switch a program asked for is answered on the
    // old link's connection, which the rebinding below ends.
    succeed(request, vthost::proto::SwitchOutcome::Switched);
    // Detach last: the old workspace stays in its daemon for whichever window selects it next, the window is
    // the new link's, and the old link ends with it.
    own->switchedAway(request.window);
    if (auto adopted = target->adoptWindow(request.window); !adopted)
        errorLog()("switch: window {} stays as it was: {}", request.window.value, adopted.error());
}

void WindowSwitcher::succeed(Request const& request, vthost::proto::SwitchOutcome outcome)
{
    _busy.erase(request.window.value);
    request.done(outcome);
}

void WindowSwitcher::failCreate(vtworkspace::WorkspaceId window,
                                session::SwitchTarget const& daemon,
                                std::string const& reason,
                                session::SwitchCompletion const& done)
{
    // Asked for from a popup that has closed by now: the window says it, as a failed switch does.
    auto const text = std::format(
        CreateFailedText, session::daemonPhrase(daemon.kind, daemon.daemon, "this daemon"), reason);
    errorLog()("switch: {}", text);
    if (auto* const controller = _sessions.controllerDisplaying(window))
        controller->notices().post(window::Notice { .kind = window::NoticeKind::SwitchFailed, .text = text });
    done(std::unexpected(reason));
}

void WindowSwitcher::cancel(Request const& request, std::string const& reason)
{
    _busy.erase(request.window.value);
    // Not a fault, and nobody to tell in the window: a newer switch or the window's close ended it.
    attachLog()("switch: window {}: {}", request.window.value, reason);
    request.done(std::unexpected(reason));
}

void WindowSwitcher::fail(Request const& request, std::string const& reason)
{
    _busy.erase(request.window.value);
    auto const text =
        std::format(SwitchFailedText,
                    session::daemonPhrase(request.address.kind, request.address.name, "this daemon"),
                    reason);
    errorLog()("switch: {}", text);
    if (auto* const controller = _sessions.controllerDisplaying(request.window))
        controller->notices().post(window::Notice { .kind = window::NoticeKind::SwitchFailed, .text = text });
    request.done(std::unexpected(reason));
}

void WindowSwitcher::decline(Request const& request, std::string const& reason)
{
    _busy.erase(request.window.value);
    auto const text =
        std::format(SwitchDeclinedText,
                    session::daemonPhrase(request.address.kind, request.address.name, "this daemon"),
                    reason);
    // The user's own answer, not a fault: logged as the switch's other outcomes are, and said without alarm.
    attachLog()("switch: {}", text);
    if (auto* const controller = _sessions.controllerDisplaying(request.window))
        controller->notices().post(
            window::Notice { .kind = window::NoticeKind::SwitchDeclined, .text = text });
    request.done(std::unexpected(reason));
}

} // namespace contour::remote
