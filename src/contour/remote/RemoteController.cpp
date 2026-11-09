// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <vtpty/ChannelPty.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace contour::remote
{

MuxConnectOutcome awaitMuxConnect(std::mutex& mutex,
                                  std::condition_variable& cv,
                                  MuxConnectPhase const& phase,
                                  std::string const& failure,
                                  std::chrono::milliseconds timeout)
{
    auto lock = std::unique_lock { mutex };
    if (!cv.wait_for(lock, timeout, [&phase] { return phase != MuxConnectPhase::Connecting; }))
        return MuxConnectOutcome { .ready = false, .timedOut = true, .failure = {} };
    return MuxConnectOutcome { .ready =
                                   phase == MuxConnectPhase::Ready || phase == MuxConnectPhase::Reconnecting,
                               .timedOut = false,
                               .failure = failure };
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
    }
    _connectStartedAt = _clock.now();
    _reactor.restart([this](core::net::EventLoop* loop) { return runClient(loop); },
                     [this](std::string const& reason) { reactorDied(reason); });
}

std::expected<void, std::string> RemoteController::awaitConnected(std::chrono::milliseconds budget)
{
    startConnecting();
    auto const spent = _clock.now() - *_connectStartedAt;
    auto const remaining = std::max(std::min(budget, MinimumConnectAwait),
                                    std::chrono::ceil<std::chrono::milliseconds>(budget - spent));
    auto const outcome = awaitMuxConnect(_mutex, _connected, _state, _failure, remaining);
    if (outcome.timedOut)
    {
        stop();
        return std::unexpected(connectTimeoutMessage());
    }
    if (!outcome.ready)
        return std::unexpected(outcome.failure.empty() ? connectClosedMessage() : outcome.failure);
    return {};
}

std::expected<void, std::string> RemoteController::connectAndWait(std::chrono::milliseconds timeout)
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
