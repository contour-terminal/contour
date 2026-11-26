// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/remote/PresencePlan.hpp>
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
    /// Why a switch the user cancelled ended.
    constexpr auto SwitchCancelled = std::string_view { "the user cancelled it" };
    /// Why a switch naming a workspace of a run of its daemon that has ended failed.
    constexpr auto DaemonRestarted = std::string_view { "the daemon restarted" };

    /// @param cause What ended a switch.
    /// @return Why, for the log and the caller.
    [[nodiscard]] std::string_view reasonOf(SupersedeCause cause) noexcept
    {
        return cause == SupersedeCause::UserCancelled ? SwitchCancelled : SwitchSuperseded;
    }

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
                               IObserverPool& pool,
                               std::unique_ptr<IWorkspaceLister> lister,
                               SwitchPicker picker,
                               SwitchDeadline deadline,
                               core::IRandom& random):
    _sessions(sessions),
    _host(host),
    _pool(pool),
    _lister(std::move(lister)),
    _picker(std::move(picker)),
    _deadline(std::move(deadline)),
    _random(random),
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
    // The user's newer switch ends the one under way, whatever its stage: the last click wins. A program's
    // ends only a switch waiting for a tab in an empty workspace (that is for the user to end, and a window
    // must not stay locked by it); any other switch under way refuses it (supersessionOf()). A switch in
    // place and an Open wait in slots of their own: a newer click of one kind never ends the other's, as a
    // window opening elsewhere is not what a click in this one replaces.
    if (auto const busy = _busy.find(slotOf(window, mode)); busy != _busy.end())
    {
        // Copied: ending the switch erases its entry.
        auto const endUnderWay = busy->second.supersede;
        if (!endUnderWay || endUnderWay(newerRequestCause(landBy)) == Supersession::Kept)
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
    // While it is resolved the request waits in one slot: the listing's answer and whatever ends it first
    // take it, and the other finds it taken (a late listing then does nothing).
    auto const slot = std::make_shared<std::optional<Request>>();
    _busy.insert_or_assign(slotOf(window, mode),
                           Busy { .serial = request.serial, .supersede = [this, slot](SupersedeCause cause) {
                                     if (supersessionOf(cause, SwitchStage::Resolving) == Supersession::Kept)
                                         return Supersession::Kept;
                                     if (auto taken = std::exchange(*slot, std::nullopt))
                                         cancel(*taken, std::string { reasonOf(cause) });
                                     return Supersession::Ended;
                                 } });
    if (!destination.config)
    {
        fail(request, destination.config.error());
        return;
    }
    // A window's link dials as Attach to Host's does, listed or not: every dial may ask in the window that
    // asked (a password host's prompt comes back with each reconnect), and a remote daemon is started if
    // absent. The presence's first-dial-only transport is for its own observers' Connect.
    request.config.emplace(*std::move(destination.config));
    // Another daemon the sidebar lists, connected: its observer holds the directory of every workspace, so
    // nothing is listed -- and a workspace named in a run of it that has ended names nothing any more.
    if (auto const section = connectedSectionOf(request))
    {
        if (request.target.instance != window::DaemonInstance {}
            && section->instance != request.target.instance)
        {
            fail(request, std::string { DaemonRestarted });
            return;
        }
        if (resolvesFromDirectory(request.target.workspace, section->directory))
        {
            auto const rows = workspaceRowsOf(section->directory, [&](uint64_t daemonWorkspace) {
                return shownHere(SameDaemon::No, request.address, *own, daemonWorkspace);
            });
            resolve(std::move(request), rows);
            return;
        }
    }
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
    slot->emplace(std::move(request));
    _lister->list(std::move(list), [alive = std::weak_ptr { _alive }, slot](auto listed) mutable {
        auto const self = alive.lock();
        auto taken = std::exchange(*slot, std::nullopt);
        // Ended meanwhile -- a newer click, a cancel, the window's close: the answer is nobody's.
        if (!self || !taken)
            return;
        auto& switcher = **self;
        if (!listed && listed.error().kind == ListFailureKind::Declined)
            switcher.decline(*taken, listed.error().reason);
        else if (!listed)
            switcher.fail(*taken, listed.error().reason);
        else
            switcher.resolveListed(*std::move(taken), *listed);
    });
}

std::optional<window::DaemonSection> WindowSwitcher::connectedSectionOf(Request const& request) const
{
    if (request.sameDaemon == SameDaemon::Yes
        || request.address.kind == vthost::proto::SwitchDaemonKind::Same)
        return std::nullopt;
    auto section = _pool.listedSection(daemonRefOf(request.address, DaemonLinkId {}));
    if (!section || section->state != window::DaemonLinkState::Connected)
        return std::nullopt;
    return section;
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
    // The creation is the window's switch under way from now on, in the slot of its mode: the user's newer
    // request or cancel ends it as one being resolved, and the window's close does. A workspace the daemon
    // makes all the same stays listed, for the user to show or close.
    auto const slot = slotOf(window, mode);
    if (auto const busy = _busy.find(slot); busy != _busy.end())
    {
        // Copied: ending the switch erases its entry.
        auto const endUnderWay = busy->second.supersede;
        if (!endUnderWay || endUnderWay(newerRequestCause(session::NoLandingDeadline)) == Supersession::Kept)
        {
            done(std::unexpected(std::string { "a switch of this window is under way" }));
            return;
        }
    }
    auto const serial = _nextSerial++;
    auto const waiting = std::make_shared<session::SwitchCompletion>(std::move(done));
    _busy.insert_or_assign(slot,
                           Busy { .serial = serial, .supersede = [this, slot, waiting](SupersedeCause cause) {
                                     if (supersessionOf(cause, SwitchStage::Resolving) == Supersession::Kept)
                                         return Supersession::Kept;
                                     _busy.erase(slot);
                                     if (auto told = std::exchange(*waiting, {}))
                                     {
                                         attachLog()("switch: window {}: {}", slot.window, reasonOf(cause));
                                         told(std::unexpected(std::string { reasonOf(cause) }));
                                     }
                                     return Supersession::Ended;
                                 } });
    auto target =
        ListTarget { .address = destination.address, .transport = destination.config->controller.transport };
    _lister->create(
        std::move(target),
        std::move(label),
        [alive = std::weak_ptr { _alive },
         window,
         daemon = session::SwitchTarget { .kind = daemon.kind, .daemon = daemon.daemon, .workspace = {} },
         mode,
         slot,
         serial,
         waiting](auto created) mutable {
            auto const self = alive.lock();
            // Ended meanwhile: what the daemon made stays listed, and nothing is shown.
            if (!self || !*waiting)
                return;
            auto done = std::exchange(*waiting, {});
            if (auto const busy = (*self)->_busy.find(slot);
                busy != (*self)->_busy.end() && busy->second.serial == serial)
                (*self)->_busy.erase(busy);
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
                applyFocus(request.window, request.target.focus);
                succeed(request, vthost::proto::SwitchOutcome::FocusedExisting);
            }
            else if constexpr (std::is_same_v<T, SwitchFocus>)
            {
                if (auto* const controller = _sessions.controllerDisplaying(decided.window))
                    controller->raiseAndActivate();
                applyFocus(decided.window, request.target.focus);
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
            workspace,
            [alive = std::weak_ptr { _alive }, take, link = own->id()](
                std::expected<uint64_t, std::string> landed) {
                auto const self = alive.lock();
                auto taken = take();
                if (!self || !taken)
                    return;
                if (!landed)
                {
                    (*self)->fail(*taken, landed.error());
                    return;
                }
                // The window shows what it landed on in a local workspace of its own, which is not the one
                // the request named it by: found by its link.
                auto const links = (*self)->_host.switchableLinks();
                auto const found = std::ranges::find(links, link, &DaemonLink::id);
                auto const shown = found != links.end() ? (*found)->windowShowing(*landed) : std::nullopt;
                (*self)->applyFocus(shown.value_or(taken->window), taken->target.focus);
                (*self)->succeed(*taken, vthost::proto::SwitchOutcome::Switched);
            });
        if (!selected)
        {
            fail(*take(), selected.error());
            return;
        }
        // Undone as a lost connection undoes it: the window's own workspace is selected again, and the link
        // tells the callback above, which fails the request.
        armDeadline(slotOf(window, mode), serial, landBy, [this, window, take, landBy] {
            auto* const link = linkOf(window);
            if (link != nullptr && expiryOf(link->switchWait(), landBy) == Expiry::Disarmed)
                return Expiry::Disarmed;
            if (link != nullptr)
                link->cancelSwitch(window, std::string { SwitchTimedOut });
            if (auto taken = take())
                fail(*taken, std::string { SwitchTimedOut });
            return Expiry::GaveUp;
        });
        onSupersede(slotOf(window, mode), serial, [this, window, take](SupersedeCause cause) {
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
        // The new link activates the row's tab and pane as it first shows the workspace.
        .focus = (*pending)->target.focus,
    };
    // A switch, or an Open, to a daemon the presence observes takes its observer over: no second connection,
    // no listing, no prompt, and every workspace's status is there the moment the window lands. The presence
    // observes a daemon only while no window of this process is on it; otherwise the link dials its own.
    auto* const adopted = adoptLink(**pending, workspace, config);
    *madeAs = adopted != nullptr ? adopted->id() : _host.startSwitchLink(std::move(config)).id();
    // The link is not wanted any more: it ends, telling nothing, and its selection goes with its connection.
    armDeadline(slotOf(window, mode), serial, landBy, [this, madeAs, take, landBy] {
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
    onSupersede(slotOf(window, mode), serial, [this, madeAs, take](SupersedeCause cause) {
        return supersedeLinked(*madeAs, cause, take);
    });
}

SwitchStage WindowSwitcher::stageOf(DaemonLink const* link)
{
    return link != nullptr && link->switchWait() == SwitchWait::ForFirstTab ? SwitchStage::WaitingForTab
                                                                            : SwitchStage::Connecting;
}

Supersession WindowSwitcher::supersedeOwn(vtworkspace::WorkspaceId window,
                                          SupersedeCause cause,
                                          TakeRequest const& take)
{
    auto* const link = linkOf(window);
    if (supersessionOf(cause, stageOf(link)) == Supersession::Kept)
        return Supersession::Kept;
    // Taken first: the link's callback then finds nothing to fail, and no notice is posted.
    auto taken = take();
    auto const reason = std::string { reasonOf(cause) };
    if (link != nullptr)
        link->cancelSwitch(window, reason);
    if (taken)
        cancel(*taken, reason);
    return Supersession::Ended;
}

Supersession WindowSwitcher::supersedeLinked(DaemonLinkId madeAs,
                                             SupersedeCause cause,
                                             TakeRequest const& take)
{
    auto const links = _host.switchableLinks();
    auto const found = std::ranges::find(links, madeAs, &DaemonLink::id);
    if (supersessionOf(cause, stageOf(found != links.end() ? *found : nullptr)) == Supersession::Kept)
        return Supersession::Kept;
    auto taken = take();
    // An adopted link gives its observer back as it is abandoned (DaemonLink::abandon()).
    if (found != links.end())
        (*found)->abandon();
    if (taken)
        cancel(*taken, std::string { reasonOf(cause) });
    return Supersession::Ended;
}

void WindowSwitcher::onSupersede(SwitchSlot slot,
                                 uint64_t serial,
                                 std::function<Supersession(SupersedeCause)> supersede)
{
    if (auto const busy = _busy.find(slot); busy != _busy.end() && busy->second.serial == serial)
        busy->second.supersede = std::move(supersede);
}

void WindowSwitcher::supersede(SwitchSlot slot, SupersedeCause cause)
{
    // Copied: ending the switch erases its entry.
    if (auto const busy = _busy.find(slot); busy != _busy.end() && busy->second.supersede)
        std::ignore = std::function { busy->second.supersede }(cause);
}

void WindowSwitcher::windowClosed(vtworkspace::WorkspaceId window)
{
    // Whatever the window waits on goes with it.
    for (auto const mode: SwitchModes)
        supersede(slotOf(window, mode), SupersedeCause::WindowClosed);
}

void WindowSwitcher::cancel(vtworkspace::WorkspaceId window, session::SwitchMode mode)
{
    supersede(slotOf(window, mode), SupersedeCause::UserCancelled);
}

void WindowSwitcher::applyFocus(vtworkspace::WorkspaceId window, session::LandingFocus const& focus)
{
    if (focus == session::LandingFocus {})
        return;
    if (auto* const controller = _sessions.controllerDisplaying(window))
        controller->land(focus);
}

DaemonLink* WindowSwitcher::adoptLink(Request const& request, uint64_t workspace, DaemonLinkConfig& config)
{
    auto* const own = linkOf(request.window);
    auto* const ownController = own != nullptr ? own->liveController() : nullptr;
    // The area of the window that shows the workspace: the one that asked, for a switch in place; for a
    // window of its own (Open), the profile's terminal size, which a dialled link's hello states too, else
    // the asking window's, which the new one corrects once it is realized.
    auto const askingArea = ownController != nullptr ? ownController->clientArea() : std::nullopt;
    auto const area =
        request.mode == session::SwitchMode::Open
            ? config.controller.attachSettings
                  .transform([](auto const& settings) -> vtpty::PageSize { return settings.pageSize; })
                  .or_else([&] { return askingArea; })
            : askingArea;
    if (!area)
        return nullptr;
    auto const daemon = daemonRefOf(request.address, DaemonLinkId {});
    auto adoption = _pool.adoptObserver(daemon, request.target.instance);
    if (adoption.controller == nullptr)
        return nullptr;
    auto& observing = adoption.controller;
    auto promotion = PromotionRequest {
        .workspace = workspace,
        .daemonInstance = workspace == 0                       ? 0
                          : request.target.instance.value != 0 ? request.target.instance.value
                                                               : observing->daemonInstance().value_or(0),
        .directoryFromSession = std::nullopt,
        .area = *area,
        .cell = ownController != nullptr ? ownController->lastStatedCell() : std::nullopt,
        .creation = std::nullopt,
    };
    // One workspace per token: a re-ask after a dropped connection selects what the first ask made.
    if (workspace == 0)
    {
        auto token = vthost::proto::CreationToken {};
        _random.fill(token);
        promotion.creation = token;
    }
    // Its later dials are the window's, as those of a link the switch would have dialled: they may ask in the
    // window that jumped, every time.
    auto interactive = config.controller.transport;
    return &_host.adoptSwitchLink(std::move(config),
                                  std::move(observing),
                                  promotion,
                                  std::move(interactive),
                                  giveBackTo(daemon, adoption.handOff));
}

GiveBack WindowSwitcher::giveBackTo(window::DaemonRef daemon, HandOffId handOff) const
{
    // The switcher is destroyed before the presence and the links after both: a link that leaves its daemon
    // once the switcher is gone stops its connection instead.
    return [alive = std::weak_ptr { _alive }, daemon = std::move(daemon), handOff](
               std::unique_ptr<NativeController> returned) {
        if (auto const self = alive.lock())
            (*self)->_pool.giveBack(daemon, handOff, std::move(returned));
        else if (returned != nullptr)
            returned->stop();
    };
}

WindowSwitcher::Expiry WindowSwitcher::expiryOf(SwitchWait wait, session::SwitchLandBy landBy) noexcept
{
    return wait == SwitchWait::ForFirstTab && !landBy ? Expiry::Disarmed : Expiry::GaveUp;
}

void WindowSwitcher::armDeadline(SwitchSlot slot,
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
    _deadline.timer(timeout, [alive = std::weak_ptr { _alive }, slot, serial, undo] {
        auto const self = alive.lock();
        if (!self)
            return;
        // Ended meanwhile, and maybe followed by another request of the same window.
        auto const busy = (*self)->_busy.find(slot);
        if (busy == (*self)->_busy.end() || busy->second.serial != serial)
            return;
        // A switch waiting for a tab said so as its target's layout arrived
        // (DaemonLink::noteSwitchTargetSettled()).
        if ((*undo)() == Expiry::GaveUp)
            attachLog()("switch: window {} gave up waiting for its target to show.", slot.window);
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
    // A switch a program asked for is told first, while the window is still the old link's: it is answered on
    // the old link's connection, which the rebinding below ends. The user's is told once it landed.
    auto const told = switchToldOf(request.landBy);
    if (told == SwitchTold::BeforeLeaving)
        succeed(request, vthost::proto::SwitchOutcome::Switched);
    // The daemon left keeps the connection as the presence's observer while it stays listed and no other
    // window is on it; else the connection ends with the link. Said before the rebinding below ends it.
    if (auto const left = own->daemonRef())
    {
        auto const kind = _pool.wantsObserver(*left, own->id()) == ObserverWanted::Yes
                              ? LeaveKind::KeepAsObserver
                              : LeaveKind::End;
        auto const handOff =
            kind == LeaveKind::KeepAsObserver ? _pool.expectGiveBack(*left, own->id()) : HandOffId {};
        own->leaveDaemon(kind, giveBackTo(*left, handOff));
    }
    // Detach last: the old workspace stays in its daemon for whichever window selects it next, the window is
    // the new link's (rebound before its old panes are detached), and the old link ends with it.
    own->switchedAway(request.window);
    auto adopted = target->adoptWindow(request.window);
    switch (told)
    {
        case SwitchTold::OnceLanded:
            // The window is the other daemon's now: what its completion runs on Switched runs there.
            if (adopted)
                succeed(request, vthost::proto::SwitchOutcome::Switched);
            else
                fail(request, adopted.error());
            break;
        case SwitchTold::BeforeLeaving:
            if (!adopted)
                errorLog()("switch: window {} stays as it was: {}", request.window.value, adopted.error());
            break;
    }
}

void WindowSwitcher::succeed(Request const& request, vthost::proto::SwitchOutcome outcome)
{
    _busy.erase(slotOf(request.window, request.mode));
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
    _busy.erase(slotOf(request.window, request.mode));
    // Not a fault, and nobody to tell in the window: a newer switch or the window's close ended it.
    attachLog()("switch: window {}: {}", request.window.value, reason);
    request.done(std::unexpected(reason));
}

void WindowSwitcher::fail(Request const& request, std::string const& reason)
{
    _busy.erase(slotOf(request.window, request.mode));
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
    _busy.erase(slotOf(request.window, request.mode));
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
