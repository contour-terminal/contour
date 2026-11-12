// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <vtpty/ChannelPty.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace contour::remote
{

namespace
{
    /// When an attach bound runs out: @p bound counted from @p since, but never less than MinimumConnectAwait
    /// (or the whole bound, if that is shorter) from @p firstLook, so a caller that only got to look once
    /// most of the bound was spent still gets the floor.
    /// @param bound How long the attach may take.
    /// @param since When the bound started to run.
    /// @param firstLook When the caller first looked at the attach.
    /// @return The deadline.
    [[nodiscard]] core::platform::SteadyTimePoint attachBoundDeadline(
        std::chrono::milliseconds bound,
        core::platform::SteadyTimePoint since,
        core::platform::SteadyTimePoint firstLook)
    {
        return std::max(since + bound, firstLook + std::min(bound, MinimumConnectAwait));
    }
} // namespace

MuxConnectOutcome awaitMuxConnect(std::mutex& mutex,
                                  std::condition_variable& cv,
                                  MuxConnectPhase const& phase,
                                  std::string const& failure,
                                  std::chrono::milliseconds timeout)
{
    auto lock = std::unique_lock { mutex };
    if (cv.wait_for(lock, timeout, [&phase] { return phase != MuxConnectPhase::Connecting; }))
        return MuxConnectOutcome { .ready = phase == MuxConnectPhase::Ready
                                            || phase == MuxConnectPhase::Reconnecting,
                                   .timedOut = false,
                                   .failure = failure };
    return MuxConnectOutcome { .ready = false, .timedOut = true, .failure = {} };
}

std::unique_ptr<vtpty::Pty> makeUnboundFallbackPty(std::optional<vtbackend::PageSize> pageSize)
{
    auto const fallback =
        pageSize.value_or(vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
    return std::make_unique<vtpty::ChannelPty>(fallback);
}

bool stopMuxReactor(std::mutex& mutex,
                    StopPhase& phase,
                    ReactorThread& reactor,
                    std::function<void()> detach,
                    std::chrono::milliseconds detachBound)
{
    {
        auto const lock = std::lock_guard { mutex };
        if (phase != StopPhase::Running)
            return false;
        phase = StopPhase::Stopped;
    }
    reactor.post(std::move(detach));
    joinDetachedReactor(reactor, detachBound);
    return true;
}

void joinDetachedReactor(ReactorThread& reactor, std::chrono::milliseconds detachBound)
{
    if (!reactor.waitForExit(detachBound))
        reactor.requestStop();
    reactor.join();
}

bool RemoteController::isConnected() const
{
    auto const lock = std::lock_guard { _mutex };
    return _state == State::Ready && _stopPhase == StopPhase::Running;
}

bool RemoteController::isStopped() const
{
    auto const lock = std::lock_guard { _mutex };
    return _stopPhase != StopPhase::Running;
}

bool RemoteController::isWindingDown() const
{
    auto const lock = std::lock_guard { _mutex };
    return _stopPhase == StopPhase::WindingDown;
}

bool RemoteController::isConnecting() const
{
    auto const lock = std::lock_guard { _mutex };
    return _stopPhase == StopPhase::Running
           && (_state == State::Connecting || _state == State::Ready || _state == State::Reconnecting);
}

void RemoteController::failConnect(std::string reason)
{
    {
        auto const lock = std::lock_guard { _mutex };
        if (_state != State::Connecting && _state != State::Ready && _state != State::Reconnecting)
            return; // already settled; the first reason is the interesting one
        _state = State::Failed;
        _failure = std::move(reason);
    }
    _connected.notify_all();
    notePhaseChange();
}

void RemoteController::startConnecting()
{
    if (_connectStartedAt)
        return;
    _connectStartedAt = _clock.now();
    // The reactor no longer dies silently: an exception that unwinds past runClient's own epilogue
    // leaves the phase stuck in Connecting, and without this the GUI would sit out the full budget
    // and then blame it on a slow daemon.
    _reactor.start([this](core::net::EventLoop* loop) { return runClient(loop); },
                   [this](std::string const& reason) { reactorDied(reason); });
}

void RemoteController::restartConnecting()
{
    awaitBackgroundStop();
    {
        auto const lock = std::lock_guard { _mutex };
        _stopPhase = StopPhase::Running;
        _state = State::Connecting;
        _failure.clear();
        _transportReturnedAt.reset();
    }
    _connectStartedAt = _clock.now();
    _reactor.restart([this](core::net::EventLoop* loop) { return runClient(loop); },
                     [this](std::string const& reason) { reactorDied(reason); });
}

void RemoteController::noteTransportReturned()
{
    {
        auto const lock = std::lock_guard { _mutex };
        _transportReturnedAt = _clock.now();
    }
    notePhaseChange();
}

std::expected<void, std::string> RemoteController::awaitConnected(vthost::AttachBudget budget)
{
    startConnecting();
    // How the wait ends: the phase left Connecting, or the bound ran out.
    auto const settled = [this](MuxConnectOutcome const& outcome) -> std::expected<void, std::string> {
        if (!outcome.ready)
            return std::unexpected(outcome.failure.empty() ? connectClosedMessage() : outcome.failure);
        return {};
    };
    auto const timedOut = [this]() -> std::expected<void, std::string> {
        stop();
        return std::unexpected(connectTimeoutMessage());
    };
    auto const returnedAt = [this] {
        auto const lock = std::lock_guard { _mutex };
        return _transportReturnedAt;
    };

    // The connect bound, where there is one, bounds the whole attach; without one the handshake bound does,
    // from when the transport returned -- recomputed whenever that changes -- which is waited for in slices
    // so that its return is seen. The time left is read off the clock at every slice rather than counted down
    // by the slices: a wait oversleeps its timeout on a loaded machine, and counting would add every
    // overshoot to the budget.
    auto deadline =
        budget.connect
            ? std::optional { attachBoundDeadline(*budget.connect, *_connectStartedAt, _clock.now()) }
            : std::nullopt;
    auto handshakeFrom = std::optional<core::platform::SteadyTimePoint> {};
    while (true)
    {
        if (!budget.connect)
        {
            if (auto const at = returnedAt(); at != handshakeFrom)
            {
                handshakeFrom = at;
                deadline = at ? std::optional { attachBoundDeadline(budget.handshake, *at, _clock.now()) }
                              : std::nullopt;
            }
        }
        auto slice = vthost::TransportReturnPollInterval;
        if (deadline)
        {
            auto const left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - _clock.now());
            // Past the deadline, one last look without waiting: an attach that settled while the last slice
            // overslept has settled, and is not to be stopped for the scheduler's delay.
            slice = std::clamp(left, std::chrono::milliseconds::zero(), slice);
        }
        auto const outcome = awaitMuxConnect(_mutex, _connected, _state, _failure, slice);
        if (!outcome.timedOut)
            return settled(outcome);
        if (slice == std::chrono::milliseconds::zero())
            return timedOut();
    }
}

std::optional<core::platform::SteadyTimePoint> RemoteController::attachDeadline(vthost::AttachBudget budget)
{
    if (!_pollStartedAt)
        _pollStartedAt = _clock.now();
    auto returnedAt = std::optional<core::platform::SteadyTimePoint> {};
    {
        auto const lock = std::lock_guard { _mutex };
        returnedAt = _transportReturnedAt;
    }
    // The deadline as awaitConnected() has it: the connect bound from startConnecting() -- never less than
    // MinimumConnectAwait from the first look -- or, without one, the handshake bound from when the transport
    // returned, from the later of that and the first look: a handshake that stalls fails once the bound has
    // passed, whatever number of looks it took.
    if (budget.connect)
        return attachBoundDeadline(*budget.connect, *_connectStartedAt, *_pollStartedAt);
    if (returnedAt)
        return std::max(*returnedAt, *_pollStartedAt) + budget.handshake;
    return std::nullopt;
}

std::optional<std::chrono::milliseconds> RemoteController::untilAttachDeadline(vthost::AttachBudget budget)
{
    auto const deadline = attachDeadline(budget);
    if (!deadline)
        return std::nullopt;
    return std::max(std::chrono::milliseconds::zero(),
                    std::chrono::ceil<std::chrono::milliseconds>(*deadline - _clock.now()));
}

std::optional<std::expected<void, std::string>> RemoteController::pollConnected(vthost::AttachBudget budget)
{
    startConnecting();
    {
        auto const lock = std::lock_guard { _mutex };
        if (_state != State::Connecting)
        {
            if (_state == State::Ready || _state == State::Reconnecting)
                return std::expected<void, std::string> {};
            return std::unexpected(_failure.empty() ? connectClosedMessage() : _failure);
        }
    }
    if (auto const deadline = attachDeadline(budget); deadline && _clock.now() >= *deadline)
    {
        stop();
        return std::unexpected(connectTimeoutMessage());
    }
    return std::nullopt;
}

std::expected<void, std::string> RemoteController::connectAndWait(vthost::AttachBudget timeout)
{
    startConnecting();
    return awaitConnected(timeout);
}

std::function<void()> RemoteController::detachOrCancel()
{
    return [this] {
        if (!detachOnReactor())
            _reactor.requestStop();
    };
}

void RemoteController::awaitBackgroundStop()
{
    if (_stopper.joinable())
        _stopper.join();
}

void RemoteController::stop()
{
    awaitBackgroundStop();
    if (stopMuxReactor(_mutex, _stopPhase, _reactor, detachOrCancel(), _detachBound))
        closeReactorBindings();
}

void RemoteController::stopInBackground(Stopped stopped)
{
    auto phase = StopPhase::Running;
    {
        auto const lock = std::lock_guard { _mutex };
        phase = _stopPhase;
        if (phase == StopPhase::Running)
            _stopPhase = StopPhase::WindingDown;
    }
    switch (phase)
    {
        case StopPhase::WindingDown: return;
        case StopPhase::Stopped:
            // A stop in the background that ended may still be telling its caller: joined first, so that
            // this caller is told after it.
            awaitBackgroundStop();
            stopped();
            return;
        case StopPhase::Running: break;
    }
    // Stopped from here on, as stop() would leave it; only the wait for the detach, and what follows it,
    // move to the thread, which alone joins the reactor until it is joined itself.
    _reactor.post(detachOrCancel());
    _stopper = std::jthread { [this, stopped = std::move(stopped)] {
        joinDetachedReactor(_reactor, _detachBound);
        closeReactorBindings();
        {
            auto const lock = std::lock_guard { _mutex };
            _stopPhase = StopPhase::Stopped;
        }
        stopped();
    } };
}

} // namespace contour::remote
