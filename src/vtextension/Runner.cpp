// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Runner.hpp>

#include <vtbackend/screen/StatusLineCommandCache.hpp>

#include <core/Assert.hpp>
#include <core/async/StopToken.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/AtomicSharedPtr.hpp>
#include <vtextension/CommandLine.hpp>
#include <vtextension/EventQueue.hpp>
#include <vtextension/HostState.hpp>
#include <vtextension/Surface.hpp>

namespace vtextension
{

namespace
{

    /// How far local civil time is ahead of UTC at an instant, as @p localTime reads it.
    /// @param localTime The time zone.
    /// @param when The instant; its offset may differ from another instant's, across daylight saving.
    /// @return The offset, to the minute; negative west of Greenwich.
    [[nodiscard]] std::chrono::minutes utcOffsetAt(vtbackend::LocalTimeConverter localTime,
                                                   std::chrono::system_clock::time_point when) noexcept
    {
        auto const utc = std::chrono::floor<std::chrono::seconds>(when).time_since_epoch();
        return std::chrono::round<std::chrono::minutes>(localTime(when).time_since_epoch() - utc);
    }

    using core::platform::SteadyDuration;
    using core::platform::SteadyTimePoint;

    /// @return @p size as the page size the instance renders for and the surface is.
    [[nodiscard]] vtbackend::PageSize pageSizeOf(SurfaceSize size) noexcept
    {
        return vtbackend::PageSize { .lines = vtbackend::LineCount(static_cast<int>(size.rows)),
                                     .columns = vtbackend::ColumnCount(static_cast<int>(size.cols)) };
    }

    /// @return @p size, but at least one cell each way: a private terminal cannot be empty.
    [[nodiscard]] vtbackend::PageSize surfacePageSize(SurfaceSize size) noexcept
    {
        return pageSizeOf(SurfaceSize { .cols = std::max(size.cols, 1U), .rows = std::max(size.rows, 1U) });
    }

    /// @return Whether a surface of @p size has anything to show.
    [[nodiscard]] bool isDrawable(SurfaceSize size) noexcept
    {
        return size.cols > 0 && size.rows > 0;
    }

    /// @return @p duration as whole milliseconds, rounded up, and never negative.
    [[nodiscard]] std::chrono::milliseconds delayUntil(SteadyDuration duration) noexcept
    {
        return std::max(std::chrono::ceil<std::chrono::milliseconds>(duration),
                        std::chrono::milliseconds::zero());
    }

    /// The most times a back-off doubles, however many faults faultsBeforeDisable allows.
    constexpr auto MaxBackoffDoublings = std::size_t { 16 };

    /// @return What a host error from capturing a command is, for the guest.
    [[nodiscard]] ExtensionError commandError(vtpty::CommandCaptureError const& error) noexcept
    {
        return error.kind == vtpty::CommandCaptureError::Kind::TimedOut ? ExtensionError::DeadlineExceeded
                                                                        : ExtensionError::CommandFailed;
    }
} // namespace

std::string disabledNotice(std::string_view name, ExtensionError error)
{
    return std::format("\u26A0 {} disabled: {}", name, errorText(error));
}

StatusLineSnapshot drawnNotice(std::string const& notice, Surface& surface, uint64_t generation)
{
    surface.feed(notice);
    auto snapshot = surface.snapshot(generation);
    snapshot.sgrText = notice;
    return snapshot;
}

/// The runner's state, shared with the jobs and timers on its lane so that it outlives the Runner
/// until its last job has run. Everything but the atomics, the queue and the callback gate is touched
/// on the lane only.
class Runner::Core final: public IHostServices, public std::enable_shared_from_this<Core>
{
  public:
    Core(std::shared_ptr<Engine> engine,
         std::shared_ptr<CompiledExtension> compiled,
         RunnerConfig config,
         RunnerServices services):
        _engine { std::move(engine) },
        _compiled { std::move(compiled) },
        _config { std::move(config) },
        _services { std::move(services) },
        _lane { _services.scheduler.assignLane() },
        _queue { EventQueueLimits { .capacity = _config.limits.queueCapacity,
                                    .userVarNames = _config.limits.maxUserVars } },
        _policy { policyFor(*_compiled, _config, _services) }
    {
        Require(_engine.get() == &_compiled->engine());
    }

    Core(Core const&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core const&) = delete;
    Core& operator=(Core&&) = delete;
    ~Core() override = default;

    // {{{ any thread
    template <typename E>
    void post(E&& event) noexcept
    {
        if (_queue.tryPush(std::forward<E>(event)) == PushResult::Dropped)
            return;
        if (_drain.exchange(DrainState::Scheduled) == DrainState::Scheduled)
            return;
        try
        {
            onLane([](Core& core) { core.drain(); });
        }
        catch (...)
        {
            // The scheduler could not take the job (out of memory): the next post retries the drain.
            _drain.store(DrainState::Idle);
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

    [[nodiscard]] std::shared_ptr<StatusLineSnapshot const> latest() const noexcept { return _latest.load(); }
    [[nodiscard]] RunnerState runnerState() const noexcept { return _state.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t dropCount() const noexcept { return _queue.dropCount(); }
    [[nodiscard]] EventKinds subscriptions() const noexcept
    {
        return EventKinds::fromValue(_subscriptions.load(std::memory_order_acquire));
    }
    [[nodiscard]] RunnerConfig const& config() const noexcept { return _config; }

    /// Leaks the core on purpose: what a Runner does when it cannot have its instance destroyed on the
    /// lane. Allocates nothing.
    void keepAliveForever() noexcept { _selfHold = shared_from_this(); }

    /// Closes the callback gate and stops a command in flight; the lane's later jobs do nothing.
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
    /// Creates and loads an instance.
    void start()
    {
        _restartTimer.reset();
        if (isClosed())
            return;
        setState(RunnerState::Starting);
        if (!Engine::ThreadEnv::isCurrentThreadPrepared())
        {
            fault(ExtensionError::RuntimeUnavailable);
            return;
        }
        if (!_surface)
            _surface = std::make_unique<Surface>(
                SurfaceConfig { .pageSize = surfacePageSize(_hostState.size),
                                .cellPixelSize = cellPixelSize(),
                                .replyCapacity = _config.limits.maxReplyBytes },
                SurfaceServices { .environment = _services.environment, .fileSystem = _services.fileSystem });

        // Instantiating runs the module's _initialize, if it exports one, which may log as load may.
        _logsThisCall = 0;
        auto instance = Instance::create(*_engine, *_compiled, *this, clock(), _config.limits);
        if (!instance)
        {
            fault(instance.error());
            return;
        }
        _instance = std::move(*instance);
        _logsThisCall = 0;
        auto const kinds = _instance->load(_config.config);
        if (!kinds)
        {
            fault(kinds.error());
            return;
        }

        _kinds = *kinds;
        _subscriptions.store(_kinds.value(), std::memory_order_release);
        setState(RunnerState::Running);
        _render = RenderDemand::Stale;
        notify();
        settle();
    }

    /// Delivers what the queue holds.
    void drain()
    {
        // Cleared first: an event pushed from now on schedules another drain. Sequentially consistent,
        // like the queue's own operations and the producer's exchange: this store and the queue's load
        // that follows must not be reordered, or a producer's push and test of the flag could both be
        // missed (store buffering), leaving its event undrained.
        _drain.store(DrainState::Idle);
        if (isClosed())
            return;
        process(_queue.drain());
    }

    /// The frame timer fired.
    void wake()
    {
        _wakeTimer.reset();
        if (isClosed())
            return;
        if (_frameDue && *_frameDue <= clock().now())
        {
            _frameDue.reset();
            process({ Event { TimerFired {} } });
            return;
        }
        settle();
    }

    /// Drops the instance and everything else that must die on the lane.
    void teardown() noexcept
    {
        cancelTimer(_wakeTimer);
        cancelTimer(_restartTimer);
        _instance.reset();
        _surface.reset();
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
                _subscriptions.store(0, std::memory_order_release);
                setState(RunnerState::Disabled);
            }
        }
    }
    // }}}

    // {{{ IHostServices (lane, from inside a call)
    void requestFrame(std::chrono::milliseconds after) override
    {
        auto const due = clock().now() + std::max(after, _config.limits.minFrameInterval);
        _frameDue = _frameDue ? std::min(*_frameDue, due) : due;
    }

    void log(LogLevel level, std::string_view message) override
    {
        if (_logsThisCall >= _config.limits.maxLogsPerCall)
            return;
        ++_logsThisCall;
        throughGate([&] {
            if (_services.onLog)
                _services.onLog(_config.name, level, message);
        });
    }

    [[nodiscard]] StateSnapshot state() const override
    {
        auto snapshot = _hostState;
        auto const now = _services.wallClock.now();
        snapshot.now = std::chrono::floor<std::chrono::milliseconds>(now);
        snapshot.utcOffset = utcOffsetAt(_services.localTime, now);
        return snapshot;
    }

    [[nodiscard]] std::expected<std::string, ExtensionError> runCommand(
        std::string_view name, std::span<std::pair<std::string, std::string> const> args) override
    {
        if (!_policy)
            return std::unexpected(ExtensionError::CommandNotDeclared);
        auto const argv = _policy->resolve(name, args);
        if (!argv)
        {
            if (argv.error() == ExtensionError::PermissionPending)
                askPermissionOnce();
            return std::unexpected(argv.error());
        }
        return shellCommandLine(*argv, _services.shellDialect).and_then([&](std::string const& commandLine) {
            return _services.capture.run(commandLine, vtbackend::MaxCommandRuntime, _stop.get_token())
                .transform_error(commandError);
        });
    }
    // }}}

  private:
    /// Whether a drain job is on its way.
    enum class DrainState : uint8_t
    {
        Idle,
        Scheduled,
    };

    /// Whether the extension's output is out of date.
    enum class RenderDemand : uint8_t
    {
        Clean,
        Stale,
    };

    /// Whether callbacks may still be called.
    enum class Gate : uint8_t
    {
        Open,
        Closed,
    };

    /// Whether the user has been asked about this runner's commands.
    enum class PermissionPrompt : uint8_t
    {
        NotAsked,
        Asked,
    };

    [[nodiscard]] static std::optional<CommandPolicy> policyFor(CompiledExtension const& compiled,
                                                                RunnerConfig const& config,
                                                                RunnerServices const& services)
    {
        if (!compiled.manifest())
            return std::nullopt;
        return CommandPolicy(*compiled.manifest(), config.hostId, services.grants, config.runCommand);
    }

    [[nodiscard]] core::platform::IClock& clock() const noexcept { return _services.scheduler.clock(); }

    [[nodiscard]] bool isClosed() const noexcept { return _stop.stop_requested(); }

    void setState(RunnerState state) noexcept { _state.store(state, std::memory_order_release); }

    /// Calls @p callback unless the Runner is gone; the Runner's destructor waits for it.
    template <typename Callback>
    void throughGate(Callback const& callback)
    {
        auto const lock = std::scoped_lock { _gateMutex };
        if (_gate == Gate::Open)
            callback();
    }

    void notify()
    {
        throughGate([&] {
            if (_services.onSnapshotPublished)
                _services.onSnapshotPublished();
        });
    }

    void askPermissionOnce()
    {
        if (_permissionPrompt == PermissionPrompt::Asked)
            return;
        _permissionPrompt = PermissionPrompt::Asked;
        throughGate([&] {
            if (_services.onPermissionNeeded)
                _services.onPermissionNeeded(_config.hostId, *_compiled->manifest());
        });
    }

    void cancelTimer(std::optional<TimerId>& timer) noexcept
    {
        if (timer)
            _services.scheduler.cancel(*std::exchange(timer, std::nullopt));
    }

    /// @return One cell of the surface, in pixels: the latest the terminal told, else the configured one.
    [[nodiscard]] vtbackend::ImageSize cellPixelSize() const noexcept
    {
        auto const& font = _hostState.font;
        if (font.cellWidth == 0 || font.cellHeight == 0)
            return _config.cellPixelSize;
        return vtbackend::ImageSize { vtbackend::Width::cast_from(font.cellWidth),
                                      vtbackend::Height::cast_from(font.cellHeight) };
    }

    /// Records @p event for `state`, and applies a resize or a font change to the surface.
    void record(Event const& event)
    {
        auto const cellBefore = cellPixelSize();
        recordEvent(_hostState, event, _config.limits.maxUserVars);
        if (auto const* const resized = std::get_if<Resized>(&event))
        {
            if (_surface && isDrawable(resized->size))
                _surface->resize(pageSizeOf(resized->size));
            // The surface changed: whatever the extension answers, its frame is stale.
            _render = RenderDemand::Stale;
        }
        else if (std::holds_alternative<FontChanged>(event) && cellPixelSize() != cellBefore)
        {
            // Only a new cell size matters to the surface: a change of point size alone, or metrics told
            // again (a replay, the extensions replaced), redraws nothing.
            if (_surface)
                _surface->setCellPixelSize(cellPixelSize());
            // Its images were cut into cells of another size: whatever the extension answers, it is stale.
            _render = RenderDemand::Stale;
        }
    }

    /// Records a batch, delivers what the extension subscribed to, then renders at most once.
    void process(std::vector<Event> const& events)
    {
        for (auto const& event: events)
        {
            record(event);
            if (!_instance || !_kinds.test(kindOf(event)))
                continue;
            _logsThisCall = 0;
            auto const request = _instance->update(event);
            if (!request)
            {
                // The rest of the batch is still recorded, for the next instance's `state`.
                fault(request.error());
                continue;
            }
            if (*request == RenderRequest::Render)
                _render = RenderDemand::Stale;
        }
        settle();
    }

    /// Renders if a render is due and allowed now, then waits for whatever comes due next.
    void settle()
    {
        if (_instance && _render == RenderDemand::Stale && isDrawable(_hostState.size))
        {
            auto const now = clock().now();
            if (!_lastRender || now >= *_lastRender + _config.limits.minFrameInterval)
                render(now);
            if (!_instance)
                return; // It faulted; the back-off has its own timer.
        }
        armWake();
    }

    /// Renders one frame and publishes it; or faults, which drops the instance.
    void render(SteadyTimePoint now)
    {
        _logsThisCall = 0;
        auto const output = _instance->render(pageSizeOf(_hostState.size));
        if (!output)
        {
            fault(output.error());
            return;
        }
        _lastRender = now;
        _render = RenderDemand::Clean;
        _surface->feed(*output);
        publish(std::make_shared<StatusLineSnapshot const>(_surface->snapshot(++_generation)));

        if (auto replies = _surface->drainReplies(); !replies.empty())
            post(ReplyBytes { .bytes = ByteList(replies.begin(), replies.end()) });
    }

    /// Makes the frame timer fire when the next requested frame or throttled render is due.
    void armWake()
    {
        auto due = _frameDue;
        if (_instance && _render == RenderDemand::Stale && isDrawable(_hostState.size) && _lastRender)
        {
            auto const renderDue = *_lastRender + _config.limits.minFrameInterval;
            due = due ? std::min(*due, renderDue) : renderDue;
        }
        if (!due)
        {
            cancelTimer(_wakeTimer);
            return;
        }
        if (_wakeTimer && _wakeDue <= *due)
            return; // It fires earlier, and arms again for what is left.
        cancelTimer(_wakeTimer);
        _wakeDue = *due;
        _wakeTimer =
            _services.scheduler.schedule(_lane, delayUntil(*due - clock().now()), [weak = weak_from_this()] {
                if (auto const self = weak.lock())
                    self->guardedJob([&] { self->wake(); });
            });
    }

    void publish(std::shared_ptr<StatusLineSnapshot const> snapshot)
    {
        _latest.store(std::move(snapshot));
        notify();
    }

    /// Drops the faulted instance, then backs off or gives up.
    void fault(ExtensionError error)
    {
        _instance.reset();
        cancelTimer(_wakeTimer);
        _frameDue.reset();
        _render = RenderDemand::Clean;

        auto const now = clock().now();
        _faults.push_back(now);
        while (_faults.front() + _config.limits.faultWindow < now)
            _faults.pop_front();

        if (_faults.size() >= _config.limits.faultsBeforeDisable)
        {
            _kinds = {};
            _subscriptions.store(0, std::memory_order_release);
            // The notice first: whoever sees the state Disabled sees the notice too.
            _latest.store(noticeFor(error));
            setState(RunnerState::Disabled);
            notify();
            return;
        }

        setState(RunnerState::BackingOff);
        // Doubling per recent fault; the shift is capped so that no configured limit can overflow it.
        auto const doublings = std::min<std::size_t>(_faults.size() - 1, MaxBackoffDoublings);
        auto const backoff = _config.limits.initialBackoff * (uint64_t { 1 } << doublings);
        cancelTimer(_restartTimer); // A fault while backing off (a job that threw) re-arms, never doubles.
        _restartTimer = _services.scheduler.schedule(_lane, backoff, [weak = weak_from_this()] {
            if (auto const self = weak.lock())
                self->guardedJob([&] { self->start(); });
        });
    }

    /// @return The frame a disabled extension shows: the notice, drawn on its surface.
    [[nodiscard]] std::shared_ptr<StatusLineSnapshot const> noticeFor(ExtensionError error)
    {
        auto const notice = disabledNotice(_config.name, error);
        if (_surface)
            return std::make_shared<StatusLineSnapshot const>(drawnNotice(notice, *_surface, ++_generation));
        auto snapshot = StatusLineSnapshot {};
        snapshot.generation = ++_generation;
        snapshot.sgrText = notice;
        return std::make_shared<StatusLineSnapshot const>(std::move(snapshot));
    }

    // Fixed at construction.
    std::shared_ptr<Engine> _engine;
    std::shared_ptr<CompiledExtension> _compiled;
    RunnerConfig _config;
    RunnerServices _services;
    LaneId _lane;

    // Any thread.
    EventQueue _queue;
    AtomicSharedPtr<StatusLineSnapshot const> _latest;
    std::atomic<RunnerState> _state { RunnerState::Starting };
    std::atomic<uint16_t> _subscriptions { 0 };
    std::atomic<DrainState> _drain { DrainState::Idle };
    std::mutex _gateMutex;
    Gate _gate = Gate::Open; ///< Guarded by _gateMutex.
    core::async::StopSource _stop;

    // The lane only.
    std::optional<CommandPolicy> _policy;
    PermissionPrompt _permissionPrompt = PermissionPrompt::NotAsked;
    std::unique_ptr<Surface> _surface;
    std::unique_ptr<Instance> _instance; ///< After the surface: destroyed first.
    EventKinds _kinds;
    StateSnapshot _hostState;
    RenderDemand _render = RenderDemand::Clean;
    std::optional<SteadyTimePoint> _lastRender;
    std::optional<SteadyTimePoint> _frameDue;
    std::optional<TimerId> _wakeTimer;
    SteadyTimePoint _wakeDue;
    std::optional<TimerId> _restartTimer;
    std::deque<SteadyTimePoint> _faults; ///< Within faultWindow, oldest first.
    uint64_t _generation = 0;
    std::size_t _logsThisCall = 0;   ///< How many messages the current call logged.
    std::shared_ptr<Core> _selfHold; ///< Set only by keepAliveForever().
    /// Allocated here, on the owner's thread, so that teardown on the lane allocates nothing.
    std::unique_ptr<RetiredExtension> _retirement = std::make_unique<RetiredExtension>();
};

Runner::Runner(std::shared_ptr<Engine> engine,
               std::shared_ptr<CompiledExtension> compiled,
               RunnerConfig config,
               RunnerServices services):
    _core { std::make_shared<Core>(
        std::move(engine), std::move(compiled), std::move(config), std::move(services)) }
{
    _core->onLane([](Core& core) { core.start(); });
}

Runner::~Runner()
{
    _core->close();
    try
    {
        // The last job: the instance dies on its lane, after whatever was posted before.
        _core->onLane([](Core& core) { core.teardown(); });
    }
    catch (...)
    {
        // The scheduler could not take the job (out of memory). Destroying the instance here, off its
        // lane, could race a job still running there: the core is kept alive for good instead.
        _core->keepAliveForever();
    }
}

void Runner::post(Event const& event) noexcept
{
    _core->post(event);
}

void Runner::post(Event&& event) noexcept
{
    _core->post(std::move(event));
}

void Runner::resize(vtbackend::PageSize size) noexcept
{
    auto const extent = [](int value) {
        return static_cast<uint32_t>(std::max(value, 0));
    };
    _core->post(Resized {
        .size = SurfaceSize { .cols = extent(unbox(size.columns)), .rows = extent(unbox(size.lines)) } });
}

std::shared_ptr<StatusLineSnapshot const> Runner::latest() const noexcept
{
    return _core->latest();
}

RunnerState Runner::state() const noexcept
{
    return _core->runnerState();
}

uint64_t Runner::dropCount() const noexcept
{
    return _core->dropCount();
}

EventKinds Runner::subscriptions() const noexcept
{
    return _core->subscriptions();
}

std::string const& Runner::name() const noexcept
{
    return _core->config().name;
}

Placement Runner::placement() const noexcept
{
    return _core->config().placement;
}

} // namespace vtextension
