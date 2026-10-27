// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <vtpty/ChannelPty.hpp>

#include <algorithm>
#include <chrono>

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
                    bool& stopped,
                    ReactorThread& reactor,
                    std::function<void()> detach,
                    std::chrono::milliseconds detachBound)
{
    {
        auto const lock = std::lock_guard { mutex };
        if (stopped)
            return false;
        stopped = true;
    }
    reactor.post(std::move(detach));
    if (!reactor.waitForExit(detachBound))
        reactor.requestStop();
    reactor.join();
    return true;
}

bool RemoteController::isConnected() const
{
    auto const lock = std::lock_guard { _mutex };
    return _state == State::Ready && !_stopped;
}

bool RemoteController::isStopped() const
{
    auto const lock = std::lock_guard { _mutex };
    return _stopped;
}

bool RemoteController::isConnecting() const
{
    auto const lock = std::lock_guard { _mutex };
    return !_stopped
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
                   [this](std::string const& reason) { failConnect(reason); });
}

void RemoteController::restartConnecting()
{
    {
        auto const lock = std::lock_guard { _mutex };
        _stopped = false;
        _state = State::Connecting;
        _failure.clear();
    }
    _connectStartedAt = _clock.now();
    _reactor.restart([this](core::net::EventLoop* loop) { return runClient(loop); },
                     [this](std::string const& reason) { failConnect(reason); });
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

void RemoteController::stop()
{
    auto const detach = [this] {
        if (!detachOnReactor())
            _reactor.requestStop();
    };
    if (stopMuxReactor(_mutex, _stopped, _reactor, detach))
        closeReactorBindings();
}

} // namespace contour::remote
