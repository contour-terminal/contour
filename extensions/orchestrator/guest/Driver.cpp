// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/guest/Driver.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::guest
{

namespace
{
    /// The names of HostError, by value, as the daemon world spells them.
    constexpr auto HostErrorNames = std::array<std::string_view, HostErrorCount> {
        "not-granted",       "not-owned",       "bad-argument", "limit",           "gone",
        "not-supported-yet", "not-a-directory", "too-many",     "program-missing",
    };

    /// The names of FileError, by value, as the daemon world spells them.
    constexpr auto FileErrorNames = std::array<std::string_view, FileErrorCount> {
        "not-granted", "bad-argument", "directory-gone", "not-regular",
        "unreadable",  "too-large",    "escapes",        "not-supported-yet",
    };

    /// The names of DispatchRefusal, by value, for the log.
    constexpr auto DispatchRefusalNames = std::array<std::string_view, 9> {
        "not-configured", "agent-limit",     "repo-limit",   "unknown-repo", "unknown-task",
        "unknown-model",  "already-running", "base-unknown", "bad-prompt",
    };

    static_assert(HostErrorNames.size() == HostErrorCount);
    static_assert(FileErrorNames.size() == FileErrorCount);
    static_assert(DispatchRefusalNames.size() == std::to_underlying(DispatchRefusal::BadPrompt) + 1U);

    /// What a FileError tells the engine; every error a `.git` file read can meet that is not one of these is
    /// FileReadError::Unreadable.
    struct FileErrorMapping
    {
        FileError error;      ///< The host's.
        FileReadError reason; ///< The engine's.
    };

    constexpr auto FileErrorMappings = std::array {
        FileErrorMapping { .error = FileError::DirectoryGone, .reason = FileReadError::DirectoryMissing },
        FileErrorMapping { .error = FileError::NotRegular, .reason = FileReadError::NotRegular },
        FileErrorMapping { .error = FileError::TooLarge, .reason = FileReadError::TooLarge },
        FileErrorMapping { .error = FileError::Escapes, .reason = FileReadError::Escapes },
    };

    /// The driver's default observer: tells nobody.
    class NoObserver final: public IDriverObserver
    {
      public:
        void starting(StartContext const& /*context*/) override {}
        void started(std::span<Effect const> /*effects*/) override {}
        void stepped(Input const& /*input*/, std::span<Effect const> /*effects*/) override {}
    };

    [[nodiscard]] IDriverObserver& noObserver()
    {
        static auto observer = NoObserver {};
        return observer;
    }

    /// @return What the engine is told of a command the host did not start.
    [[nodiscard]] ProcessOutcome notStarted(HostError error)
    {
        // A missing working directory is the one refusal that says the worktree is gone; a missing program
        // arrives as BadArgument, and is a refusal like any other.
        return ProcessOutcome { .start =
                                    error == HostError::Gone ? StartResult::CwdMissing : StartResult::Refused,
                                .completion = Completion::Exited,
                                .exitCode = std::nullopt,
                                .extent = OutputExtent::Whole,
                                .stdoutText = {},
                                .stderrText = {} };
    }

    [[nodiscard]] FileReadError readErrorOf(FileError error) noexcept
    {
        auto const mapping = std::ranges::find(FileErrorMappings, error, &FileErrorMapping::error);
        return mapping != FileErrorMappings.end() ? mapping->reason : FileReadError::Unreadable;
    }

    /// @return @p path relative to @p root, if it lies inside it; else @p path, which the host refuses.
    [[nodiscard]] std::string_view relativeTo(std::string_view root, std::string_view path) noexcept
    {
        if (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/')
            return path.substr(root.size() + 1);
        return path;
    }

    /// A stream of a finished process, as its pages were read.
    struct TakenStream
    {
        std::string text;                          ///< Its bytes.
        OutputExtent extent = OutputExtent::Whole; ///< Truncated if a page could not be read.
    };

    /// Reads a stream of a finished process page by page, from offset 0 until nothing remains.
    [[nodiscard]] TakenStream takeStream(IGuestHost& host, uint32_t handle, ProcStream stream)
    {
        auto taken = TakenStream {};
        while (true)
        {
            auto const offset = static_cast<uint32_t>(taken.text.size());
            auto chunk = host.procTake(handle, stream, offset);
            if (!chunk)
            {
                host.log(
                    LogLevel::Warn,
                    std::format("proc-take of process {} at {}: {}", handle, offset, nameOf(chunk.error())));
                taken.extent = OutputExtent::Truncated;
                return taken;
            }
            taken.text += chunk->bytes;
            if (chunk->remaining == 0)
                return taken;
            if (chunk->bytes.empty())
            {
                // The host promised more and gave nothing: what was read is all there is.
                taken.extent = OutputExtent::Truncated;
                return taken;
            }
        }
    }

    /// @return What a finished process left, as the engine takes it.
    [[nodiscard]] ProcessOutcome takeOutcome(IGuestHost& host, uint32_t handle, ProcResult const& result)
    {
        auto standardOutput = takeStream(host, handle, ProcStream::Stdout);
        auto standardError = takeStream(host, handle, ProcStream::Stderr);
        auto const truncated = std::ranges::contains(
            std::array {
                result.stdoutExtent, result.stderrExtent, standardOutput.extent, standardError.extent },
            OutputExtent::Truncated);
        return ProcessOutcome {
            .start = StartResult::Started,
            .completion =
                result.completion == ProcCompletion::TimedOut ? Completion::TimedOut : Completion::Exited,
            // A signalled process has no exit code.
            .exitCode = result.completion == ProcCompletion::Exited ? result.exitCode : std::nullopt,
            .extent = truncated ? OutputExtent::Truncated : OutputExtent::Whole,
            .stdoutText = std::move(standardOutput.text),
            .stderrText = std::move(standardError.text),
        };
    }

    // The engine input each of a dashboard's actions is stepped as, arriving at @p monoMs: one overload per
    // UiAction the engine steps.

    [[nodiscard]] Input inputOf(view::RefreshNowAction const& /*refresh*/, uint64_t monoMs)
    {
        return RefreshNow { .monoMs = monoMs };
    }

    [[nodiscard]] Input inputOf(view::EndRunAction const& end, uint64_t monoMs)
    {
        return EndRun { .run = end.run, .monoMs = monoMs };
    }

    [[nodiscard]] Input inputOf(view::RetryRemovalAction const& retry, uint64_t monoMs)
    {
        return RetryRemovalOf { .run = retry.run, .monoMs = monoMs };
    }

    [[nodiscard]] Input inputOf(view::DismissAction const& dismiss, uint64_t monoMs)
    {
        return DismissRun { .run = dismiss.run, .monoMs = monoMs };
    }

    [[nodiscard]] Input inputOf(view::MarkSeenAction const& seen, uint64_t monoMs)
    {
        return MarkRunSeen { .run = seen.run, .monoMs = monoMs };
    }

    /// @return A listed tab's size, as the dashboard takes it.
    [[nodiscard]] core::tui::Size sizeOf(TabInfo const& tab) noexcept
    {
        return core::tui::Size { .width = static_cast<int>(tab.cols), .height = static_cast<int>(tab.rows) };
    }

    /// @return @p time in the host's monotonic milliseconds, rounded up: a wake then never comes early.
    [[nodiscard]] uint64_t monoMsOf(core::SteadyTimePoint time) noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::ceil<std::chrono::milliseconds>(time.time_since_epoch()).count());
    }

    [[nodiscard]] PaneFact paneFactOf(PaneInfo const& pane)
    {
        return PaneFact {
            .pane = pane.pane, .tag = pane.tag, .status = pane.status, .program = pane.program
        };
    }

    /// The panes, as the engine takes them: the host's tagged listing, or none known while it serves none.
    struct ListedPanes
    {
        std::vector<PaneFact> panes;                ///< The listed panes.
        PaneListing listing = PaneListing::Unknown; ///< Where their tags come from; Unknown: nothing listed.
    };

    /// @return The panes; std::nullopt if the host failed to list them -- `limit` before it restored its
    ///         sessions, or any other failure -- which tells nothing about any run. A host that serves no
    ///         listing (`not-supported-yet`) answers none, as PaneListing::OwnRecordsOnly: no running run is
    ///         ended on it.
    [[nodiscard]] std::optional<ListedPanes> listedPanes(IGuestHost& host)
    {
        auto listed = host.panes();
        if (!listed)
        {
            if (listed.error() == HostError::NotSupportedYet)
                return ListedPanes { .panes = {}, .listing = PaneListing::OwnRecordsOnly };
            // `limit` is a host still restoring its sessions, as every boot after an upgrade is: not news.
            host.log(listed.error() == HostError::Limit ? LogLevel::Debug : LogLevel::Warn,
                     std::format("panes: {}", nameOf(listed.error())));
            return std::nullopt;
        }
        auto facts = std::vector<PaneFact> {};
        facts.reserve(listed->size());
        for (auto const& pane: *listed)
            facts.push_back(paneFactOf(pane));
        return ListedPanes { .panes = std::move(facts), .listing = PaneListing::Tagged };
    }
} // namespace

std::string_view nameOf(HostError error) noexcept
{
    return HostErrorNames[std::to_underlying(error)];
}

std::string_view nameOf(FileError error) noexcept
{
    return FileErrorNames[std::to_underlying(error)];
}

// {{{ WakeTimer

std::optional<uint64_t> WakeTimer::takeDue(uint64_t nowMs) noexcept
{
    if (!_due || *_due > nowMs)
        return std::nullopt;
    return std::exchange(_due, std::nullopt);
}

std::optional<uint64_t> WakeTimer::unasked(std::optional<uint64_t> alsoDueMs) const noexcept
{
    auto due = _due;
    if (alsoDueMs)
        due = due ? std::min(*due, *alsoDueMs) : *alsoDueMs;
    if (!due || _asked == due)
        return std::nullopt;
    return due;
}

uint32_t WakeTimer::delayUntil(uint64_t dueMs, uint64_t nowMs) noexcept
{
    if (dueMs <= nowMs)
        return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(dueMs - nowMs, std::numeric_limits<uint32_t>::max()));
}

// }}}
// {{{ Driver

std::array<Driver::ActionHandler, 3> const Driver::ActionHandlers {
    &Driver::openDashboard, // ActionKind::Open
    &Driver::refreshNow,    // ActionKind::Refresh
    &Driver::showBlocked,   // ActionKind::ShowBlocked
};

Driver::Driver(
    IGuestHost& host, IUuidSource& ids, IDriverObserver& observer, std::string worktreeRoot, Engine engine):
    _host { host },
    _ids { ids },
    _observer { observer },
    _worktreeRoot { std::move(worktreeRoot) },
    _engine { std::make_unique<Engine>(std::move(engine)) },
    _clock { std::make_unique<GuestClock>() }
{
}

Driver Driver::load(IGuestHost& host, IUuidSource& ids, std::string_view configJson, HostInfo hostInfo)
{
    return load(host, ids, configJson, std::move(hostInfo), noObserver());
}

Driver Driver::load(IGuestHost& host,
                    IUuidSource& ids,
                    std::string_view configJson,
                    HostInfo hostInfo,
                    IDriverObserver& observer)
{
    auto stored = std::vector<StoredEntry> {};
    if (auto const keys = host.storeKeys(""))
    {
        for (auto const& key: *keys)
        {
            if (auto value = host.storeGet(key); value && *value)
                stored.push_back(StoredEntry { .key = key, .value = std::move(**value) });
            else if (!value)
                host.log(LogLevel::Error, std::format("store-get {}: {}", key, nameOf(value.error())));
        }
    }
    else
        host.log(LogLevel::Error, std::format("store-keys: {}", nameOf(keys.error())));

    auto inFlight = std::vector<std::string> {};
    auto finished = std::vector<FinishedResult> {};
    auto taken = std::vector<uint32_t> {};
    if (auto const processes = host.procs())
    {
        for (auto const& process: *processes)
        {
            if (!process.result)
            {
                inFlight.push_back(process.tag);
                continue;
            }
            finished.push_back(FinishedResult {
                .tag = process.tag, .outcome = takeOutcome(host, process.handle, *process.result) });
            taken.push_back(process.handle);
        }
    }
    else
        host.log(LogLevel::Error, std::format("procs: {}", nameOf(processes.error())));

    // Unlisted, the panes are not known (PaneListing::Unknown): no run is reconciled, so none ends because
    // the host could not answer yet; the next listing that is known reconciles them.
    auto panes = listedPanes(host).value_or(ListedPanes {});
    auto const clock = host.now();
    auto worktreeRoot = hostInfo.worktreeRoot;
    auto context = StartContext { .monoMs = clock.monoMs,
                                  .wallMs = clock.wallMs,
                                  .host = std::move(hostInfo),
                                  .configJson = std::string(configJson),
                                  .stored = std::move(stored),
                                  .inFlight = std::move(inFlight),
                                  .finished = std::move(finished),
                                  .panes = std::move(panes.panes),
                                  .paneListing = panes.listing };
    observer.starting(context);
    auto started = Engine::start(std::move(context));
    observer.started(started.effects);

    auto driver = Driver { host, ids, observer, std::move(worktreeRoot), std::move(started.engine) };
    driver.carryOut(started.effects);
    // The results the start consumed are released once its effects, its store writes among them, are done.
    for (auto const handle: taken)
        driver.release(handle);
    driver.stepReplies();
    // A restarted guest finds its tabs open, and draws each in full; actions invoked while it was away wait.
    driver.followTabs();
    driver.takeActions();
    driver.drawTabs();
    driver.askForWake();
    return driver;
}

void Driver::update(Hint hint)
{
    switch (hint)
    {
        case Hint::Wake: wake(); break;
        case Hint::Procs: consumeFinished(); break;
        case Hint::Tabs:
            followTabs();
            readTabs();
            break;
        case Hint::Actions: takeActions(); break;
        // The host's registry changed: a pane opened, went, or its status changed.
        case Hint::Panes: listPanes(); break;
    }
    drawTabs();
    askForWake();
}

view::Dashboard const* Driver::dashboard(std::string_view tab) const noexcept
{
    auto const found = _dashboards.find(tab);
    return found != _dashboards.end() ? found->second.get() : nullptr;
}

view::Dashboard* Driver::dashboard(std::string_view tab) noexcept
{
    auto const found = _dashboards.find(tab);
    return found != _dashboards.end() ? found->second.get() : nullptr;
}

// {{{ the served tabs

void Driver::takeActions()
{
    static_assert(ActionHandlers.size() == std::to_underlying(ActionKind::ShowBlocked) + 1U);
    auto taken = _host.actionsTake();
    if (!taken)
    {
        if (taken.error() != HostError::NotSupportedYet)
            _host.log(LogLevel::Warn, std::format("actions-take: {}", nameOf(taken.error())));
        return;
    }
    for (auto const& action: *taken)
    {
        auto const* const declared = findAction(action.id);
        if (declared == nullptr)
        {
            _host.log(LogLevel::Warn, std::format("unknown action {}", action.id));
            continue;
        }
        (this->*ActionHandlers[std::to_underlying(declared->kind)])(action.origin);
    }
}

void Driver::openDashboard(uint32_t origin)
{
    static_cast<void>(openTab(origin));
}

void Driver::refreshNow(uint32_t /*origin*/)
{
    step(RefreshNow { .monoMs = nowMs() });
    stepReplies();
}

void Driver::showBlocked(uint32_t origin)
{
    auto const tab = openTab(origin);
    if (!tab)
        return;
    auto const dashboard = _dashboards.find(*tab);
    if (dashboard == _dashboards.end())
        return;
    auto const runs = _engine->runs();
    auto const blocked =
        std::ranges::find_if(runs, [](Run const& run) { return std::holds_alternative<Blocked>(run.state); });
    if (blocked != runs.end())
        dashboard->second->select(blocked->id);
}

std::optional<std::string> Driver::openTab(uint32_t origin)
{
    // The host reveals the tab already open in the origin's workspace, and answers its id.
    auto tab = _host.tabOpen(ServedScheme, origin);
    if (!tab)
    {
        _host.log(LogLevel::Warn, std::format("tab-open: {}", nameOf(tab.error())));
        return std::nullopt;
    }
    if (!_dashboards.contains(*tab))
        followTabs();
    // An agent revealed from this dashboard goes to the window that last opened or revealed it.
    _origins.insert_or_assign(*tab, origin);
    return std::move(*tab);
}

void Driver::apply(std::string_view tab, view::UiAction const& action)
{
    std::visit(Overloaded {
                   [&](view::RevealAgentAction const& reveal) { revealAgent(tab, reveal.run); },
                   [&](view::ShowBoardAction const& show) {
                       step(ShowBoard { .run = show.run, .tab = std::string { tab }, .monoMs = nowMs() });
                       stepReplies();
                   },
                   [&](view::HideBoardAction const&) {
                       step(HideBoard { .tab = std::string { tab }, .monoMs = nowMs() });
                       stepReplies();
                   },
                   [&](view::DispatchAction const& dispatch) {
                       // Stamped with the time it arrived: the run's names carry the wall time.
                       auto const clock = readClock();
                       step(Dispatch { .repo = dispatch.repo,
                                       .item = dispatch.item,
                                       .url = dispatch.url,
                                       .task = dispatch.task,
                                       .model = dispatch.model,
                                       .note = dispatch.note,
                                       .session = dispatch.session,
                                       .near = dispatch.near,
                                       .wallMs = clock.wallMs,
                                       .monoMs = clock.monoMs });
                       stepReplies();
                   },
                   [&](auto const& engineAction) {
                       step(inputOf(engineAction, nowMs()));
                       stepReplies();
                   },
               },
               action);
}

void Driver::revealAgent(std::string_view tab, std::string_view run)
{
    auto const runs = _engine->runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    if (found == runs.end() || !found->pane)
    {
        _host.log(LogLevel::Debug, std::format("reveal {}: no agent tab", run));
        return;
    }
    // 0 when no action opened this dashboard since the guest loaded: the host then reveals it only in the
    // window a key or a click into one of our tabs came from within the last 2 s, and answers gone otherwise.
    auto const origin = _origins.find(tab);
    auto const revealed = _host.paneReveal(*found->pane, origin != _origins.end() ? origin->second : 0);
    if (!revealed)
    {
        // A pane closed meanwhile, or a dashboard no window opened since the guest loaded: expected, not
        // news.
        auto const expected = revealed.error() == HostError::Gone || revealed.error() == HostError::NotOwned;
        _host.log(expected ? LogLevel::Debug : LogLevel::Warn,
                  std::format("pane-reveal {}: {}", *found->pane, nameOf(revealed.error())));
    }
}

void Driver::followTabs()
{
    auto listed = _host.tabs();
    if (!listed)
    {
        // A host that serves no tab has none open; any other failure keeps the dashboards as they are.
        if (listed.error() != HostError::NotSupportedYet)
            _host.log(LogLevel::Warn, std::format("tabs: {}", nameOf(listed.error())));
        return;
    }
    auto const unlisted = [&](auto const& entry) {
        return std::ranges::none_of(*listed, [&](TabInfo const& tab) { return tab.tab == entry.first; });
    };
    auto closed = std::vector<std::string> {};
    for (auto const& entry: _dashboards)
        if (unlisted(entry))
            closed.push_back(entry.first);
    std::erase_if(_dashboards, unlisted);
    std::erase_if(_origins, unlisted);
    // A closed tab shows no board: it is read no more for it.
    for (auto& tab: closed)
    {
        step(HideBoard { .tab = std::move(tab), .monoMs = nowMs() });
        stepReplies();
    }
    for (auto const& tab: *listed)
    {
        if (auto const found = _dashboards.find(tab.tab); found != _dashboards.end())
            found->second->resize(sizeOf(tab));
        else
            _dashboards.emplace(tab.tab,
                                std::make_unique<view::Dashboard>(
                                    *_engine, view::dashboardTheme(), *_clock, sizeOf(tab), _ids, tab.tab));
    }
}

void Driver::readTabs()
{
    for (auto const& [tab, dashboard]: _dashboards)
    {
        auto const bytes = _host.tabRead(tab);
        if (!bytes)
        {
            _host.log(LogLevel::Debug, std::format("tab-read {}: {}", tab, nameOf(bytes.error())));
            continue;
        }
        // Nothing typed is not an input: it would end the wait of an ESC held for the rest of its sequence.
        if (bytes->empty())
            continue;
        static_cast<void>(readClock());
        for (auto const& action: dashboard->input(*bytes))
            apply(tab, action);
    }
}

void Driver::drawTabs()
{
    if (_dashboards.empty())
        return;
    // The frame resolves an ESC held past its deadline, and measures the agents' ages, by the time now.
    static_cast<void>(readClock());
    for (auto const& [tab, dashboard]: _dashboards)
        if (auto const bytes = dashboard->frame(); !bytes.empty())
            write(tab, bytes);
}

void Driver::write(std::string const& tab, std::string_view bytes)
{
    while (!bytes.empty())
    {
        auto const piece = bytes.substr(0, MaxTabWriteBytes);
        if (auto const written = _host.tabWrite(tab, piece); !written)
        {
            // The rest of the frame is dropped; the other tabs are drawn still.
            _host.log(LogLevel::Debug, std::format("tab-write {}: {}", tab, nameOf(written.error())));
            return;
        }
        bytes.remove_prefix(piece.size());
    }
}

std::optional<uint64_t> Driver::dashboardsDueMs() const
{
    // A held ESC becomes the Escape key, and an agent's shown age moves on, only in a frame.
    auto earliest = std::optional<uint64_t> {};
    for (auto const& dashboard: _dashboards | std::views::values)
        for (auto const due: { dashboard->pendingEscapeDeadline(), dashboard->nextRepaintDue() })
            if (due)
            {
                auto const dueMs = monoMsOf(*due);
                earliest = earliest ? std::min(*earliest, dueMs) : dueMs;
            }
    return earliest;
}

// }}}

void Driver::wake()
{
    _timer.requestUsed();
    // A lost Procs hint only delays a result: a wake takes it in first.
    consumeFinished();
    auto const clock = readClock();
    if (_timer.takeDue(clock.monoMs))
    {
        step(Wake { .monoMs = clock.monoMs, .wallMs = clock.wallMs });
        stepReplies();
        listPanes();
    }
    else if (_paneListingDueMs && clock.monoMs >= *_paneListingDueMs)
    {
        // Asked once: a listing the host cannot answer yet is followed by its Panes hint once it can.
        _paneListingDueMs.reset();
        listPanes();
    }
}

void Driver::listPanes()
{
    // The request time is read before the listing is asked for.
    auto const requestedMs = nowMs();
    auto listed = listedPanes(_host);
    if (!listed)
        return;
    if (_paneListingDueMs && requestedMs >= *_paneListingDueMs)
        _paneListingDueMs.reset();
    step(PanesListed {
        .panes = std::move(listed->panes), .requestedMonoMs = requestedMs, .listing = listed->listing });
    stepReplies();
}

void Driver::consumeFinished()
{
    auto const processes = _host.procs();
    if (!processes)
    {
        _host.log(LogLevel::Error, std::format("procs: {}", nameOf(processes.error())));
        return;
    }
    // A handle the host no longer lists is never listed again.
    std::erase_if(_consumed, [&](uint32_t handle) {
        return std::ranges::none_of(*processes,
                                    [&](ProcInfo const& process) { return process.handle == handle; });
    });
    for (auto const& process: *processes)
    {
        if (!process.result || _consumed.contains(process.handle))
            continue;
        auto outcome = takeOutcome(_host, process.handle, *process.result);
        step(ProcessFinished { .tag = process.tag, .outcome = std::move(outcome), .monoMs = nowMs() });
        release(process.handle);
        stepReplies();
    }
}

void Driver::step(Input const& input)
{
    auto const effects = _engine->step(input);
    _observer.stepped(input, effects);
    carryOut(effects);
}

void Driver::stepReplies()
{
    while (!_replies.empty())
    {
        auto const input = std::move(_replies.front());
        _replies.pop_front();
        step(input);
    }
}

void Driver::release(uint32_t handle)
{
    if (auto const released = _host.procRelease(handle); !released)
    {
        _host.log(LogLevel::Warn, std::format("proc-release {}: {}", handle, nameOf(released.error())));
        _consumed.insert(handle);
    }
}

void Driver::askForWake()
{
    auto due = dashboardsDueMs();
    if (_paneListingDueMs)
        due = std::min(due.value_or(*_paneListingDueMs), *_paneListingDueMs);
    due = _timer.unasked(due);
    if (!due)
        return;
    _host.wake(WakeTimer::delayUntil(*due, nowMs()));
    _timer.asked(*due);
}

uint64_t Driver::nowMs()
{
    return readClock().monoMs;
}

ClockReading Driver::readClock()
{
    auto const clock = _host.now();
    _clock->set(clock.monoMs);
    return clock;
}

void Driver::carryOut(std::span<Effect const> effects)
{
    for (auto const& effect: effects)
    {
        std::visit(
            Overloaded {
                [&](RunCommand const& command) {
                    auto const started =
                        _host.procStart(command.command, command.arguments, command.cwd, command.tag);
                    if (!started)
                        _replies.emplace_back(ProcessFinished {
                            .tag = command.tag, .outcome = notStarted(started.error()), .monoMs = nowMs() });
                },
                [&](ReadFile const& read) {
                    auto file = _host.fileRead(WorktreesRoot, relativeTo(_worktreeRoot, read.path));
                    auto const clock = readClock();
                    auto reply = FileRead { .tag = read.tag,
                                            .content = std::unexpected { FileReadError::Unreadable },
                                            .modifiedWallMs = 0,
                                            .monoMs = clock.monoMs,
                                            .wallMs = clock.wallMs };
                    if (file)
                    {
                        reply.content = std::move(file->bytes);
                        reply.modifiedWallMs = file->modifiedUnixMs;
                    }
                    else
                        reply.content = std::unexpected { readErrorOf(file.error()) };
                    _replies.emplace_back(std::move(reply));
                },
                [&](OpenPane const& open) {
                    auto pane = _host.paneOpen(
                        open.paneTemplate,
                        open.arguments,
                        open.cwd,
                        open.title,
                        open.run,
                        open.near.transform([](std::string const& tab) { return std::string_view { tab }; }));
                    if (pane)
                        _replies.emplace_back(
                            PaneOpenedFor { .run = open.run, .pane = std::move(*pane), .monoMs = nowMs() });
                    else
                    {
                        auto const refusedMs = nowMs();
                        _replies.emplace_back(PaneOpenFailedFor {
                            .run = open.run, .why = std::string(nameOf(pane.error())), .monoMs = refusedMs });
                        // Nothing was opened, so no Panes hint follows.
                        _paneListingDueMs = refusedMs + 1;
                    }
                },
                [&](ClosePane const& close) {
                    if (auto const closed = _host.paneClose(close.pane); !closed)
                        _host.log(LogLevel::Debug,
                                  std::format("pane-close {}: {}", close.pane, nameOf(closed.error())));
                },
                [&](StoreSet const& set) {
                    if (auto const stored = _host.storeSet(set.key, set.value); !stored)
                        _host.log(LogLevel::Error,
                                  std::format("store-set {}: {}", set.key, nameOf(stored.error())));
                },
                [&](StoreRemove const& remove) {
                    if (auto const removed = _host.storeRemove(remove.key); !removed)
                        _host.log(LogLevel::Error,
                                  std::format("store-remove {}: {}", remove.key, nameOf(removed.error())));
                },
                [&](PublishSummary const& summary) {
                    if (auto const set = _host.summarySet(summary.shortText, summary.detail); !set)
                        _host.log(LogLevel::Warn, std::format("summary-set: {}", nameOf(set.error())));
                },
                [&](WakeAt const& wake) { _timer.set(wake.monoMs); },
                // Drawn once, when the update ends.
                [&](Redraw const&) {
                    for (auto const& dashboard: _dashboards | std::views::values)
                        dashboard->engineChanged();
                },
                [&](DispatchRefused const& refused) {
                    _host.log(LogLevel::Info,
                              std::format("dispatch for {} refused: {}",
                                          refused.repo,
                                          DispatchRefusalNames[std::to_underlying(refused.reason)]));
                    // The dashboard it was confirmed in says why.
                    if (auto const dashboard =
                            refused.near ? _dashboards.find(*refused.near) : _dashboards.end();
                        dashboard != _dashboards.end())
                        dashboard->second->dispatchRefused(refused.reason);
                },
            },
            effect);
    }
}

// }}}

} // namespace orchestrator::guest
