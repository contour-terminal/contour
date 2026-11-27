// SPDX-License-Identifier: Apache-2.0
#include <core/net/AcceptLoop.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/Diagnostics.hpp>
#include <core/net/InterruptibleSleep.hpp>

#include <chrono>
#include <utility>

namespace core::net
{

namespace
{

    /// How a backoff ended.
    enum class Backoff : std::uint8_t
    {
        Elapsed, ///< It ran its course: accept again.
        Stopped, ///< The listener was closed (or destroyed) or the flow cancelled while it waited.
    };

    /// Waits out an accept backoff, unless the listener is closed or the flow is cancelled first.
    /// @param loop The loop whose timer waits.
    /// @param closed The listener's close token.
    /// @param delay How long.
    /// @return How the wait ended.
    async::Task<Backoff> backOff(EventLoop* loop, async::StopToken closed, std::chrono::milliseconds delay)
    {
        try
        {
            auto const woke = co_await interruptibleSleepUntil(loop, closed, loop->clock().now() + delay);
            co_return woke == WakeReason::Deadline ? Backoff::Elapsed : Backoff::Stopped;
        }
        catch (async::OperationCancelled const&)
        {
            // The flow's own stop, met in a backoff: the loop returns, as it does when the same stop
            // meets it parked in an accept, which the listener answers `Cancelled`.
            co_return Backoff::Stopped;
        }
    }

} // namespace

void reportAcceptLoopEvent(AcceptLoopReporting const& reporting, AcceptLoopEvent const& event)
{
    if (reporting.onEvent)
        reporting.onEvent(event);
    else
        reportDiagnostic(event.line);
}

async::Task<void> acceptLoop(EventLoop* loop,
                             IListener* listener,
                             AcceptLoopReporting reporting,
                             ConnectionFlow flow,
                             OnAccepted onAccepted)
{
    // Taken while the listener is alive -- the caller's contract -- and kept: the token outlives the
    // listener, so asking it before every accept is how this loop never calls into a listener its
    // owner closed and destroyed while the loop was backing off or serving a connection.
    auto const closed = listener->closeToken();
    auto policy = AcceptErrorPolicy {};
    // Set when the listener was closed or the flow cancelled where the loop could see it; the end
    // that follows is the owner's, and only the way out below says anything about it.
    auto stopping = false;
    while (!stopping && !closed.stop_requested())
    {
        auto accepted = AcceptResult {};
        try
        {
            accepted = co_await listener->accept();
        }
        catch (async::OperationCancelled const&)
        {
            // The flow's own stop, met in an accept that answers it by THROWING `OperationCancelled`
            // -- one parked on a loop timer, say, or any `ResultAwaitable` resumed without a value --
            // where core-cpp's socket listeners answer `Cancelled`. Either way the loop returns, and
            // the way out still says whether a degraded loop stopped.
            stopping = true;
            continue;
        }
        if (!accepted.has_value())
        {
            // One failed accept is almost never a failed listener: only a closed or dead one ends
            // the loop, and everything else is accepted past (`AcceptPolicy.hpp`).
            auto const& error = accepted.error();
            auto const verdict = policy.onError(error.code, loop->clock().now());
            switch (verdict.action)
            {
                case AcceptAction::Stop: stopping = true; continue;
                case AcceptAction::GiveUp:
                    // A dead listener left open would go on queueing handshakes nobody accepts: the
                    // port is closed, so it refuses them, and then the end is reported.
                    if (!closed.stop_requested())
                        listener->close();
                    reportAcceptLoopEvent(
                        reporting,
                        AcceptLoopEvent { .surface = reporting.surface,
                                          .line = describeAcceptLoopEnded(reporting.surface, error),
                                          .error = error,
                                          .kind = AcceptLoopEventKind::GaveUp });
                    co_return;
                case AcceptAction::AcceptAgain: break;
            }
            if (verdict.change == AcceptConditionChange::Degraded)
                reportAcceptLoopEvent(reporting,
                                      AcceptLoopEvent { .surface = reporting.surface,
                                                        .line = describeAcceptDegraded(
                                                            reporting.surface, error, verdict.streak),
                                                        .error = error,
                                                        .kind = AcceptLoopEventKind::Degraded });
            else if (verdict.change == AcceptConditionChange::Recovered)
                reportAcceptLoopEvent(
                    reporting,
                    AcceptLoopEvent { .surface = reporting.surface,
                                      .line = describeAcceptRecovered(reporting.surface, verdict.streak),
                                      .error = {},
                                      .kind = AcceptLoopEventKind::Recovered });
            if (verdict.warning.has_value())
                reportAcceptLoopEvent(
                    reporting,
                    AcceptLoopEvent { .surface = reporting.surface,
                                      .line = describeAcceptFailure(reporting.surface, error, verdict),
                                      .error = error,
                                      .kind = AcceptLoopEventKind::Warning });
            if (verdict.delay > std::chrono::milliseconds {})
                stopping = co_await backOff(loop, closed, verdict.delay) == Backoff::Stopped;
            continue;
        }
        if (auto const recovered = policy.onAccepted(loop->clock().now());
            recovered.change == AcceptConditionChange::Recovered)
            reportAcceptLoopEvent(
                reporting,
                AcceptLoopEvent { .surface = reporting.surface,
                                  .line = describeAcceptRecovered(reporting.surface, recovered.streak),
                                  .error = {},
                                  .kind = AcceptLoopEventKind::Recovered });
        auto connection = onAccepted(std::move(*accepted));
        switch (flow)
        {
            case ConnectionFlow::Awaited: co_await std::move(connection); break;
            case ConnectionFlow::Spawned: loop->spawn(std::move(connection)); break;
        }
    }
    // Every way out but giving up comes here: a closed listener, a destroyed one, a cancelled flow.
    // A loop that was degraded says it has stopped, or a liveness registry goes on showing a surface
    // that was shut down as degraded -- and, restarted under the same name, the new loop has a
    // fresh policy that never reports the recovery that would clear it.
    if (policy.degraded())
        reportAcceptLoopEvent(reporting,
                              AcceptLoopEvent { .surface = reporting.surface,
                                                .line = describeAcceptLoopStopped(reporting.surface),
                                                .error = {},
                                                .kind = AcceptLoopEventKind::Stopped });
}

} // namespace core::net
