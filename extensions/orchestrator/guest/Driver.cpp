// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/guest/Driver.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Overloaded.hpp>

namespace orchestrator::guest
{

namespace
{
    /// The names of HostError, by value, as the daemon world spells them.
    constexpr auto HostErrorNames = std::array<std::string_view, 6> {
        "not-granted", "not-owned", "bad-argument", "limit", "gone", "not-supported-yet",
    };

    /// The names of FileError, by value, as the daemon world spells them.
    constexpr auto FileErrorNames = std::array<std::string_view, 8> {
        "not-granted", "bad-argument", "directory-gone", "not-regular",
        "unreadable",  "too-large",    "escapes",        "not-supported-yet",
    };

    /// The names of DispatchRefusal, by value, for the log.
    constexpr auto DispatchRefusalNames = std::array<std::string_view, 9> {
        "not-configured", "agent-limit",     "repo-limit",   "unknown-repo", "unknown-task",
        "unknown-model",  "already-running", "base-unknown", "bad-prompt",
    };

    static_assert(HostErrorNames.size() == std::to_underlying(HostError::NotSupportedYet) + 1U);
    static_assert(FileErrorNames.size() == std::to_underlying(FileError::NotSupportedYet) + 1U);
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

    [[nodiscard]] PaneFact paneFactOf(PaneInfo const& pane)
    {
        return PaneFact {
            .pane = pane.pane, .tag = pane.tag, .status = pane.status, .program = pane.program
        };
    }

    /// The panes, as the engine takes them: the host's tagged listing, or none known while it serves none.
    struct ListedPanes
    {
        std::vector<PaneFact> panes;                       ///< The listed panes.
        PaneListing listing = PaneListing::OwnRecordsOnly; ///< Where their tags come from.
    };

    /// @return The panes; std::nullopt if the host failed to list them, which the engine is then not told.
    [[nodiscard]] std::optional<ListedPanes> listedPanes(IGuestHost& host)
    {
        auto listed = host.panes();
        if (!listed)
        {
            if (listed.error() == HostError::NotSupportedYet)
                return ListedPanes { .panes = {}, .listing = PaneListing::OwnRecordsOnly };
            host.log(LogLevel::Warn, std::format("panes: {}", nameOf(listed.error())));
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

std::optional<uint64_t> WakeTimer::unasked() const noexcept
{
    if (!_due || _asked == _due)
        return std::nullopt;
    return _due;
}

uint32_t WakeTimer::delayUntil(uint64_t dueMs, uint64_t nowMs) noexcept
{
    if (dueMs <= nowMs)
        return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(dueMs - nowMs, std::numeric_limits<uint32_t>::max()));
}

// }}}
// {{{ Driver

Driver::Driver(IGuestHost& host, IDriverObserver& observer, std::string worktreeRoot, Engine engine):
    _host { host },
    _observer { observer },
    _worktreeRoot { std::move(worktreeRoot) },
    _engine { std::move(engine) }
{
}

Driver Driver::load(IGuestHost& host, std::string_view configJson, HostInfo hostInfo)
{
    return load(host, configJson, std::move(hostInfo), noObserver());
}

Driver Driver::load(IGuestHost& host,
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

    // Unlisted, the panes are not known: none is reported, and the engine takes the listing as its own
    // records.
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

    auto driver = Driver { host, observer, std::move(worktreeRoot), std::move(started.engine) };
    driver.carryOut(started.effects);
    // The results the start consumed are released once its effects, its store writes among them, are done.
    for (auto const handle: taken)
        driver.release(handle);
    driver.stepReplies();
    driver.askForWake();
    return driver;
}

void Driver::update(Hint hint)
{
    switch (hint)
    {
        case Hint::Wake: wake(); break;
        case Hint::Procs: consumeFinished(); break;
        // Served from phase 2 on: panes with the host's pane-to-tag registry, tabs and actions with the view.
        case Hint::Panes:
        case Hint::Tabs:
        case Hint::Actions: break;
    }
    askForWake();
}

void Driver::wake()
{
    _timer.requestUsed();
    // A lost Procs hint only delays a result: a wake takes it in first.
    consumeFinished();
    auto const clock = _host.now();
    if (!_timer.takeDue(clock.monoMs))
        return;
    step(Wake { .monoMs = clock.monoMs, .wallMs = clock.wallMs });
    stepReplies();
    listPanes();
}

void Driver::listPanes()
{
    // The request time is read before the listing is asked for.
    auto const requestedMs = nowMs();
    auto listed = listedPanes(_host);
    if (!listed)
        return;
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
    auto const effects = _engine.step(input);
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
    auto const due = _timer.unasked();
    if (!due)
        return;
    _host.wake(WakeTimer::delayUntil(*due, nowMs()));
    _timer.asked(*due);
}

uint64_t Driver::nowMs()
{
    return _host.now().monoMs;
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
                    auto content = _host.fileRead(WorktreesRoot, relativeTo(_worktreeRoot, read.path));
                    _replies.emplace_back(FileRead { .tag = read.tag,
                                                     .content = std::move(content).transform_error(
                                                         [](FileError error) { return readErrorOf(error); }),
                                                     .monoMs = nowMs() });
                },
                [&](OpenPane const& open) {
                    auto pane =
                        _host.paneOpen(open.paneTemplate, open.arguments, open.cwd, open.title, open.run);
                    if (pane)
                        _replies.emplace_back(
                            PaneOpenedFor { .run = open.run, .pane = std::move(*pane), .monoMs = nowMs() });
                    else
                        _replies.emplace_back(PaneOpenFailedFor {
                            .run = open.run, .why = std::string(nameOf(pane.error())), .monoMs = nowMs() });
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
                // The dashboard arrives in phase 2.
                [&](Redraw const&) {},
                [&](DispatchRefused const& refused) {
                    _host.log(LogLevel::Info,
                              std::format("dispatch for {} refused: {}",
                                          refused.repo,
                                          DispatchRefusalNames[std::to_underlying(refused.reason)]));
                },
            },
            effect);
    }
}

// }}}

} // namespace orchestrator::guest
