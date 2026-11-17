// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DaemonExtensionRunner.hpp>

#include <vtbackend/screen/LocalTime.hpp>

#include <core/Assert.hpp>
#include <core/async/StopToken.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonInstance.hpp>
#include <vtextension/ExtensionConsent.hpp>

namespace vtextension
{

namespace
{
    using core::platform::SteadyDuration;
    using core::platform::SteadyTimePoint;

    /// The most times a back-off doubles, however many faults faultsBeforeDisable allows.
    constexpr auto MaxBackoffDoublings = std::size_t { 16 };

    /// @return The bit of @p hint in the pending-hint mask.
    [[nodiscard]] constexpr uint8_t bitOf(DaemonHint hint) noexcept
    {
        return static_cast<uint8_t>(1U << std::to_underlying(hint));
    }

    /// Every hint, in the order pending hints are delivered: DaemonHint's.
    constexpr auto HintOrder = [] {
        auto order = std::array<DaemonHint, abi::EnumCaseCount<DaemonHint>> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, order.size()))
            order.at(index) = static_cast<DaemonHint>(index);
        return order;
    }();
    static_assert(HintOrder.size() <= 8, "the pending hints are one byte's bits");

    /// @return @p when as whole milliseconds since its clock's epoch, never negative.
    template <typename TimePoint>
    [[nodiscard]] uint64_t millisecondsOf(TimePoint when) noexcept
    {
        auto const count = std::chrono::floor<std::chrono::milliseconds>(when.time_since_epoch()).count();
        return static_cast<uint64_t>(std::max<decltype(count)>(count, 0));
    }

    /// @return @p size as a WIT `u32`, saturated.
    [[nodiscard]] uint32_t clampedU32(std::size_t size) noexcept
    {
        return static_cast<uint32_t>(std::min<std::size_t>(size, UINT32_MAX));
    }

    /// @return How many characters (code points) @p text holds; it is valid UTF-8.
    [[nodiscard]] std::size_t characterCount(std::string_view text) noexcept
    {
        constexpr auto ContinuationMask = uint8_t { 0b1100'0000 };
        constexpr auto ContinuationBits = uint8_t { 0b1000'0000 };
        return static_cast<std::size_t>(std::ranges::count_if(
            text, [&](char c) { return (static_cast<uint8_t>(c) & ContinuationMask) != ContinuationBits; }));
    }

    /// @return @p text neutralized line by line, its `\n` line breaks kept.
    [[nodiscard]] std::string neutralizedLines(std::string_view text)
    {
        auto shown = std::string {};
        while (true)
        {
            auto const end = text.find('\n');
            shown += neutralizedForDisplay(text.substr(0, end));
            if (end == std::string_view::npos)
                return shown;
            shown += '\n';
            text.remove_prefix(end + 1);
        }
    }

    /// @return What the guest sees of a process the service lists.
    [[nodiscard]] ProcInfo procInfoOf(ProcessInfo const& process)
    {
        auto state = ProcState { ProcRunning {} };
        if (process.result)
            state = ProcResult {
                .completion = process.result->completion,
                .exitCode = process.result->exitCode,
                .stdoutBytes = clampedU32(process.result->stdoutBytes),
                .stdoutExtent = process.result->stdoutExtent,
                .stderrBytes = clampedU32(process.result->stderrBytes),
                .stderrExtent = process.result->stderrExtent,
            };
        return ProcInfo { .handle = std::to_underlying(process.handle),
                          .tag = process.tag,
                          .command = process.command,
                          .state = state };
    }

    /// @return @p error as the guest sees it.
    [[nodiscard]] HostError toHostError(ProcessError error) noexcept
    {
        return hostErrorOf(error);
    }

    /// @copydoc toHostError(ProcessError)
    [[nodiscard]] HostError toHostError(StoreError error) noexcept
    {
        return hostErrorOf(error);
    }

    /// @return @p bytes as the text the store keeps.
    [[nodiscard]] std::string textOf(std::span<uint8_t const> bytes)
    {
        auto text = std::string(bytes.begin(), bytes.end());
        return text;
    }
} // namespace

/// The runner's state, shared with the jobs and timers on its lane so that it outlives the runner until its
/// last job has run. Everything but the atomics, the status and the callback gate is touched on the lane
/// only.
class DaemonExtensionRunner::Core final: public IDaemonHostServices, public std::enable_shared_from_this<Core>
{
  public:
    Core(std::shared_ptr<Engine> engine,
         std::shared_ptr<CompiledExtension const> compiled,
         DaemonRunnerConfig config,
         DaemonServices services):
        _engine { std::move(engine) },
        _compiled { std::move(compiled) },
        _config { std::move(config) },
        _services { std::move(services) },
        _lane { _services.scheduler.assignLane() }
    {
        Require(_engine.get() == &_compiled->engine() && _engine->world() == World::Daemon);
    }

    Core(Core const&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core const&) = delete;
    Core& operator=(Core&&) = delete;
    ~Core() override = default;

    // {{{ any thread
    void hint(DaemonHint hint) noexcept
    {
        _pending.fetch_or(bitOf(hint));
        if (_drain.exchange(DrainState::Scheduled) == DrainState::Scheduled)
            return;
        try
        {
            onLane([](Core& core) { core.drain(); });
        }
        catch (...)
        {
            // The scheduler could not take the job (out of memory): the next hint retries the drain.
            _drain.store(DrainState::Idle);
        }
    }

    /// Tells the presence that this run is up, before any of its jobs runs.
    void reportRunning() noexcept { reportPhase(DaemonExtensionPhase::Running); }

    void restart() noexcept
    {
        {
            auto const lock = std::scoped_lock { _statusMutex };
            if (_status.state != DaemonRunnerState::Disabled)
                return;
            _status.state = DaemonRunnerState::Starting;
        }
        reportPhase(DaemonExtensionPhase::Running);
        try
        {
            onLane([](Core& core) { core.restartDisabled(); });
        }
        catch (...)
        {
            // The scheduler could not take the job: it stays disabled, and may be restarted again.
            {
                auto const lock = std::scoped_lock { _statusMutex };
                _status.state = DaemonRunnerState::Disabled;
            }
            reportPhase(DaemonExtensionPhase::Disabled);
        }
    }

    /// Runs @p job on the lane, keeping the core alive until it has run.
    /// @throws What the scheduler throws when it cannot take the job.
    template <typename Job>
    void onLane(Job job)
    {
        _services.scheduler.post(_lane, [self = shared_from_this(), job = std::move(job)] {
            self->guardedJob([&] { job(*self); });
        });
    }

    [[nodiscard]] DaemonRunnerStatus status() const noexcept
    {
        auto const lock = std::scoped_lock { _statusMutex };
        return _status;
    }

    [[nodiscard]] DaemonRunnerConfig const& config() const noexcept { return _config; }

    /// Leaks the core on purpose: what a runner does when it cannot have its instance destroyed on the lane.
    void keepAliveForever() noexcept { _selfHold = shared_from_this(); }

    /// Closes the callback gate; the lane's later jobs do nothing.
    void close() noexcept
    {
        {
            auto const lock = std::scoped_lock { _gateMutex };
            _gate = Gate::Closed;
        }
        _stop.request_stop();
    }
    // }}}

    // {{{ lane
    /// Creates and loads an instance, then delivers the hints that wait.
    void start()
    {
        _restartTimer.reset();
        if (isClosed())
            return;
        setState(DaemonRunnerState::Starting);
        if (!_engine->backend().isCurrentThreadPrepared())
        {
            fault(ExtensionError::RuntimeUnavailable);
            return;
        }
        // Instantiating runs a reactor's _initialize, a callback of its own.
        beginCallback();
        auto instance = DaemonInstance::create(_compiled, *this, clock(), *_engine, _config.limits);
        if (!instance)
        {
            fault(instance.error());
            return;
        }
        _instance = std::move(*instance);
        beginCallback();
        // The offset as of this load: a daemon runs across daylight-saving changes.
        auto const hostInfo = DaemonHostInfo {
            .roots = _config.roots,
            .home = _config.home,
            .utcOffsetMinutes = static_cast<int32_t>(
                vtbackend::utcOffsetAt(_services.localTime, _services.wallClock.now()).count()),
        };
        if (auto const loaded = _instance->load(_config.configJson, hostInfo); !loaded)
        {
            fault(loaded.error());
            return;
        }
        setState(DaemonRunnerState::Running);
        deliver();
    }

    /// A disabled extension restarted: a fresh fault window, then a start.
    void restartDisabled()
    {
        _faults.clear();
        {
            auto const lock = std::scoped_lock { _statusMutex };
            _status.faultsInWindow = 0;
        }
        start();
    }

    /// Delivers the hints that wait.
    void drain()
    {
        // Cleared first: a hint raised from now on schedules another drain. Sequentially consistent, as the
        // raiser's exchange: this store and the mask's exchange that follows must not be reordered.
        _drain.store(DrainState::Idle);
        if (isClosed())
            return;
        deliver();
    }

    /// Drops the instance and everything else that must die on the lane.
    void teardown() noexcept
    {
        cancelTimer(_wakeTimer);
        cancelTimer(_restartTimer);
        _instance.reset();
        // The engine and module go back to the owner side: their last reference must not die here.
        _retirement->compiled = std::move(_compiled);
        _retirement->engine = std::move(_engine);
        _services.retirement.retire(std::move(_retirement));
    }

    /// Runs one job body: no exception may leave a job, and one that would faults the instance.
    template <typename Body>
    void guardedJob(Body const& body) noexcept
    {
        try
        {
            body();
        }
        catch (...)
        {
            try
            {
                fault(ExtensionError::Trapped);
            }
            catch (...)
            {
                // Not even the fault could be recorded (out of memory): stop the extension as it is.
                _instance.reset();
                setState(DaemonRunnerState::Disabled);
                reportPhase(DaemonExtensionPhase::Disabled);
            }
        }
    }
    // }}}

    // {{{ IDaemonHostServices (lane, from inside a call)
    void log(LogLevel level, std::string_view message) override
    {
        if (_logsThisCall >= _config.limits.maxLogsPerCall)
            return;
        ++_logsThisCall;
        emitLog(level, message);
    }

    void wake(std::chrono::milliseconds after) override
    {
        cancelTimer(_wakeTimer);
        _wakeTimer =
            _services.scheduler.schedule(_lane, std::max(after, MinWakeInterval), [weak = weak_from_this()] {
                if (auto const self = weak.lock())
                    self->guardedJob([&] { self->wakeFired(); });
            });
    }

    [[nodiscard]] ClockReading now() override
    {
        return ClockReading { .monotonicMs = millisecondsOf(clock().now()),
                              .wallMs = millisecondsOf(_services.wallClock.now()) };
    }

    [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                               ConfigMap const& args,
                                                               std::string_view cwd,
                                                               std::string_view tag) override
    {
        return _services.processes.start(command, args, cwd, tag)
            .transform([](ProcessHandle handle) { return std::to_underlying(handle); })
            .transform_error([](ProcessError error) { return toHostError(error); });
    }

    [[nodiscard]] std::expected<std::vector<ProcInfo>, HostError> procs() override
    {
        return std::ranges::to<std::vector<ProcInfo>>(_services.processes.list()
                                                      | std::views::transform(procInfoOf));
    }

    [[nodiscard]] std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                           ProcStream stream,
                                                           uint32_t offset) override
    {
        return _services.processes.take(ProcessHandle { handle }, stream, offset)
            .transform([](OutputPage const& page) {
                return Chunk { .bytes = ByteList(page.bytes.begin(), page.bytes.end()),
                               .remaining = clampedU32(page.remaining) };
            })
            .transform_error([](ProcessError error) { return toHostError(error); });
    }

    [[nodiscard]] std::expected<void, HostError> procRelease(uint32_t handle) override
    {
        return _services.processes.release(ProcessHandle { handle }).transform_error([](ProcessError error) {
            return toHostError(error);
        });
    }

    [[nodiscard]] std::expected<std::optional<ByteList>, HostError> storeGet(std::string_view key) override
    {
        return _services.store.get(key)
            .transform([](std::optional<std::string> const& value) -> std::optional<ByteList> {
                if (!value)
                    return std::nullopt;
                return ByteList(value->begin(), value->end());
            })
            .transform_error([](StoreError error) { return toHostError(error); });
    }

    [[nodiscard]] std::expected<void, HostError> storeSet(std::string_view key,
                                                          std::span<uint8_t const> value) override
    {
        return durably([&] { return _services.store.set(key, textOf(value)); });
    }

    [[nodiscard]] std::expected<void, HostError> storeRemove(std::string_view key) override
    {
        return durably([&] { return _services.store.remove(key); });
    }

    [[nodiscard]] std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view prefix) override
    {
        return _services.store.keys(prefix).transform_error(
            [](StoreError error) { return toHostError(error); });
    }

    [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view shortText,
                                                            std::string_view detail) override
    {
        if (characterCount(shortText) > MaxSummaryShortChars || detail.size() > MaxSummaryDetailBytes)
            return std::unexpected(HostError::Limit);
        _services.presence.publishSummary(
            _config.name, _config.generation, neutralizedForDisplay(shortText), neutralizedLines(detail));
        return {};
    }
    // }}}

  private:
    /// Whether a drain job is on its way.
    enum class DrainState : uint8_t
    {
        Idle,
        Scheduled,
    };

    /// Whether callbacks may still be called.
    enum class Gate : uint8_t
    {
        Open,
        Closed,
    };

    [[nodiscard]] core::platform::IClock& clock() const noexcept { return _services.scheduler.clock(); }

    [[nodiscard]] bool isClosed() const noexcept { return _stop.stop_requested(); }

    /// Tells the presence where the extension now stands, as this run.
    void reportPhase(DaemonExtensionPhase phase) noexcept
    {
        _services.presence.phaseChanged(_config.name, _config.generation, phase);
    }

    void setState(DaemonRunnerState state) noexcept
    {
        auto const lock = std::scoped_lock { _statusMutex };
        _status.state = state;
    }

    /// Calls the log callback unless the runner is gone; the runner's destructor waits for it.
    void emitLog(LogLevel level, std::string_view message)
    {
        auto const lock = std::scoped_lock { _gateMutex };
        if (_gate == Gate::Open && _services.onLog)
            _services.onLog(_config.name, level, message);
    }

    void cancelTimer(std::optional<TimerId>& timer) noexcept
    {
        if (timer)
            _services.scheduler.cancel(*std::exchange(timer, std::nullopt));
    }

    /// Gives the callback about to run its own budgets: its logs and its durable calls.
    void beginCallback() noexcept
    {
        _logsThisCall = 0;
        _durableCalls = 0;
        _durableTime = SteadyDuration::zero();
    }

    /// Runs one durable store call within the callback's DurableBudget, and charges it.
    /// @return What the store answered, as the guest sees it; `limit`, the store untouched, once the budget
    /// is
    ///         spent.
    template <typename Call>
    [[nodiscard]] std::expected<void, HostError> durably(Call const& call)
    {
        auto const& budget = _config.durableBudget;
        if (_durableCalls >= budget.maxCalls || _durableTime >= budget.maxTime)
            return std::unexpected(HostError::Limit);
        ++_durableCalls;
        auto const started = clock().now();
        auto result = call();
        _durableTime += clock().now() - started;
        return result.transform_error([](StoreError error) { return toHostError(error); });
    }

    /// The `wake` timer fired: an `update(wake)`, or one once an instance runs.
    void wakeFired()
    {
        _wakeTimer.reset();
        if (isClosed())
            return;
        _pending.fetch_or(bitOf(DaemonHint::Wake));
        deliver();
    }

    /// Calls `update` once per pending hint, in DaemonHint's order. Without an instance, or for the hints
    /// after one that faulted, the hints stay pending for the next instance; the hint that faulted is
    /// dropped.
    void deliver()
    {
        if (!_instance)
            return;
        auto const pending = _pending.exchange(0);
        for (auto const hint: HintOrder)
        {
            if ((pending & bitOf(hint)) == 0)
                continue;
            if (!_instance)
            {
                _pending.fetch_or(bitOf(hint));
                continue;
            }
            beginCallback();
            if (auto const updated = _instance->update(hint); !updated)
                fault(updated.error());
        }
    }

    /// Drops the faulted instance, then backs off or gives up.
    void fault(ExtensionError error)
    {
        _instance.reset();
        cancelTimer(_wakeTimer);

        auto const now = clock().now();
        _faults.push_back(now);
        while (_faults.front() + _config.limits.faultWindow < now)
            _faults.pop_front();
        auto const disabled = _faults.size() >= _config.limits.faultsBeforeDisable;
        {
            auto const lock = std::scoped_lock { _statusMutex };
            _status.lastFault = error;
            _status.faultsInWindow = _faults.size();
            _status.state = disabled ? DaemonRunnerState::Disabled : DaemonRunnerState::BackingOff;
        }
        if (disabled)
        {
            reportPhase(DaemonExtensionPhase::Disabled);
            emitLog(LogLevel::Error, std::format("{} disabled: {}", _config.name, errorText(error)));
            return;
        }

        // Doubling per recent fault; the shift is capped so that no configured limit can overflow it.
        auto const doublings = std::min<std::size_t>(_faults.size() - 1, MaxBackoffDoublings);
        auto const backoff = _config.limits.initialBackoff * (uint64_t { 1 } << doublings);
        cancelTimer(_restartTimer); // A fault while backing off (a job that threw) re-arms, never doubles.
        _restartTimer = _services.scheduler.schedule(_lane, backoff, [weak = weak_from_this()] {
            if (auto const self = weak.lock())
                self->guardedJob([&] { self->start(); });
        });
    }

    // Fixed at construction.
    std::shared_ptr<Engine> _engine;
    std::shared_ptr<CompiledExtension const> _compiled;
    DaemonRunnerConfig _config;
    DaemonServices _services;
    LaneId _lane;

    // Any thread.
    std::atomic<uint8_t> _pending { 0 }; ///< The hints waiting, one bit per DaemonHint.
    std::atomic<DrainState> _drain { DrainState::Idle };
    mutable std::mutex _statusMutex;
    DaemonRunnerStatus _status; ///< Guarded by _statusMutex.
    std::mutex _gateMutex;
    Gate _gate = Gate::Open; ///< Guarded by _gateMutex.
    core::async::StopSource _stop;

    // The lane only.
    std::unique_ptr<DaemonInstance> _instance;
    std::optional<TimerId> _wakeTimer;
    std::optional<TimerId> _restartTimer;
    std::deque<SteadyTimePoint> _faults; ///< Within faultWindow, oldest first.
    std::size_t _logsThisCall = 0;       ///< How many messages the current callback logged.
    std::size_t _durableCalls = 0;       ///< How many durable calls the current callback made.
    SteadyDuration _durableTime {};      ///< How long the current callback's durable calls took.
    std::shared_ptr<Core> _selfHold;     ///< Set only by keepAliveForever().
    /// Allocated here, on the owner's thread, so that teardown on the lane allocates nothing.
    std::unique_ptr<RetiredExtension> _retirement = std::make_unique<RetiredExtension>();
};

DaemonExtensionRunner::DaemonExtensionRunner(std::shared_ptr<Engine> engine,
                                             std::shared_ptr<CompiledExtension const> compiled,
                                             DaemonRunnerConfig config,
                                             DaemonServices services):
    _core { std::make_shared<Core>(
        std::move(engine), std::move(compiled), std::move(config), std::move(services)) }
{
    // Before its first job: whatever the lane reports of this run comes after it.
    _core->reportRunning();
    _core->onLane([](Core& core) { core.start(); });
}

DaemonExtensionRunner::~DaemonExtensionRunner()
{
    _core->close();
    try
    {
        // The last job: the instance dies on its lane, after whatever was posted before.
        _core->onLane([](Core& core) { core.teardown(); });
    }
    catch (...)
    {
        // The scheduler could not take the job (out of memory). Destroying the instance here, off its lane,
        // could race a job still running there: the core is kept alive for good instead.
        _core->keepAliveForever();
    }
}

void DaemonExtensionRunner::hint(DaemonHint hint) noexcept
{
    _core->hint(hint);
}

void DaemonExtensionRunner::restart() noexcept
{
    _core->restart();
}

DaemonRunnerStatus DaemonExtensionRunner::status() const noexcept
{
    return _core->status();
}

std::string const& DaemonExtensionRunner::name() const noexcept
{
    return _core->config().name;
}

} // namespace vtextension
