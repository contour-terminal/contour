// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/MuxController.h>
#include <contour/mux/MuxLoopThread.h>

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

bool stopMuxReactor(std::mutex& mutex, bool& stopped, MuxLoopThread& reactor, std::function<void()> detach)
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

} // namespace contour
