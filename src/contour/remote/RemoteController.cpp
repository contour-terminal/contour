// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <vtpty/ChannelPty.hpp>

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
    return MuxConnectOutcome { .ready = phase == MuxConnectPhase::Ready,
                               .timedOut = false,
                               .failure = failure };
}

std::unique_ptr<vtpty::Pty> makeUnboundFallbackPty(std::optional<vtbackend::PageSize> pageSize)
{
    auto const fallback =
        pageSize.value_or(vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
    return std::make_unique<vtpty::ChannelPty>(fallback);
}

bool stopMuxReactor(std::mutex& mutex, bool& stopped, ReactorThread& reactor, std::function<void()> detach)
{
    {
        auto const lock = std::lock_guard { mutex };
        if (stopped)
            return false;
        stopped = true;
    }
    reactor.post(std::move(detach));
    reactor.requestStop();
    reactor.join();
    return true;
}

void RemoteController::failConnect(std::string reason)
{
    {
        auto const lock = std::lock_guard { _mutex };
        if (_state != State::Connecting && _state != State::Ready)
            return; // already settled; the first reason is the interesting one
        _state = State::Failed;
        _failure = std::move(reason);
    }
    _connected.notify_all();
}

std::expected<void, std::string> RemoteController::connectAndWait(std::chrono::milliseconds timeout)
{
    // The reactor no longer dies silently: an exception that unwinds past runClient's own epilogue
    // leaves the phase stuck in Connecting, and without this the GUI would sit out the full timeout
    // and then blame it on a slow daemon.
    _reactor.start([this](net::EventLoop* loop) { return runClient(loop); },
                   [this](std::string const& reason) { failConnect(reason); });

    auto const outcome = awaitMuxConnect(_mutex, _connected, _state, _failure, timeout);
    if (outcome.timedOut)
    {
        stop();
        return std::unexpected(connectTimeoutMessage());
    }
    if (!outcome.ready)
        return std::unexpected(outcome.failure.empty() ? connectClosedMessage() : outcome.failure);
    return {};
}

void RemoteController::stop()
{
    if (stopMuxReactor(_mutex, _stopped, _reactor, [this] { detachOnReactor(); }))
        closeReactorBindings();
}

} // namespace contour::remote
