// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.h>
#include <contour/remote/RemoteController.h>

#include <vtpty/ChannelPty.h>

namespace contour
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

std::expected<void, std::string> RemoteController::connectAndWait(std::chrono::milliseconds timeout)
{
    _reactor.start([this](net::EventLoop* loop) { return runClient(loop); });

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

} // namespace contour
