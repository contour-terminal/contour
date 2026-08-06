// SPDX-License-Identifier: Apache-2.0
#include <vthost/ConnectionAcceptor.hpp>

#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <coro/Cancellation.hpp>
#include <vthost/Logging.hpp>

namespace vthost
{

using namespace std::chrono_literals;

namespace
{
    /// Runs one connection's flow and reports anything that escapes it.
    ///
    /// Without this, a throwing handler disappears without a trace. `coro::Task`'s promise
    /// captures an escaping exception into an `exception_ptr` and only rethrows it when someone
    /// awaits the task or asks for its result — but a connection flow is spawned as a ROOT task,
    /// and the loop reaps finished roots by destroying their frames. The `exception_ptr` is
    /// destroyed unread, so the connection dies silently while the daemon carries on.
    ///
    /// Surviving the throw is the RIGHT behaviour — one malformed peer must not take the daemon
    /// down with it — and that behaviour is unchanged here. What changes is that the failure is
    /// now visible, on the always-enabled `error` category, with the connection's identity
    /// attached. A bug that reaches this line is one nobody would otherwise learn about.
    ///
    /// @param handler The connection flow to run.
    /// @param id The connection's identity, for the report.
    /// @param connection The accepted transport, moved into the flow.
    coro::Task<void> superviseConnection(ConnectionHandler handler,
                                         ConnectionId id,
                                         std::unique_ptr<net::ISocket> connection)
    {
        // Formatted BEFORE the move: the flow takes ownership of the id.
        auto const identity = std::format("{}", id);
        try
        {
            co_await handler(std::move(id), std::move(connection));
        }
        catch (coro::OperationCancelled const&)
        {
            // Not a failure: this is how shutdown reaches a flow parked on a read. Reported on
            // the connection tier rather than as an error, so a trace still shows the whole
            // connection's story ending.
            connectionLog()("{}: connection flow cancelled", identity);
        }
        catch (std::exception const& e)
        {
            errorLog()("{}: connection flow aborted: {}", identity, e.what());
        }
        catch (...)
        {
            errorLog()("{}: connection flow aborted with a non-standard exception", identity);
        }
    }
} // namespace

bool AcceptFailureThrottle::shouldLog(net::NetError const& error) noexcept
{
    if (error.code != _lastCode || error.systemCode != _lastSystemCode)
    {
        // A different failure is a different story, however often the old one repeated.
        _lastCode = error.code;
        _lastSystemCode = error.systemCode;
        _consecutive = 1;
        return true;
    }

    ++_consecutive;
    // The first failure already reported at _consecutive == 1, so the next report is due at
    // 1 + N, then 1 + 2N — hence the offset. Without it the very next repeat would be logged
    // for everyNth == 1 and the "one per N" contract would be off by one everywhere.
    return (_consecutive - 1) % _everyNth == 0;
}

ConnectionAcceptor::ConnectionAcceptor(net::EventLoop& loop,
                                       std::string name,
                                       std::unique_ptr<net::IListener> listener,
                                       ConnectionHandler handler,
                                       AcceptFailureThrottle failureThrottle):
    _loop(loop),
    _name(std::move(name)),
    _listener(std::move(listener)),
    _handler(std::move(handler)),
    _failureThrottle(failureThrottle)
{
}

coro::Task<void> ConnectionAcceptor::serve()
{
    // Copied into the coroutine frame at entry, and used in place of _name below.
    //
    // This is load-bearing, not a micro-optimisation: in runDaemon the acceptors are declared
    // AFTER the event loop, so they are destroyed BEFORE ~EventLoop drains its ready queue and
    // resumes this parked coroutine one last time. Touching any member on that final resume is
    // a use-after-free — which is why the cancellation path below reports `name`, and why it
    // does not report _acceptedCount at all.
    auto const name = _name;

    while (true)
    {
        auto accepted = co_await _listener->accept();
        if (!accepted.has_value())
        {
            if (accepted.error().code == net::NetErrorCode::Cancelled)
            {
                daemonLog()("{}: listener closed", name);
                co_return; // listener closed / shutdown requested
            }
            // Persistent failures (EMFILE/ENFILE fd exhaustion) fail synchronously
            // with no suspension point — a bare `continue` would spin this loop and
            // starve every other flow on the event loop. Back off, then keep serving.
            //
            // Throttled, because that back-off never gives up: an unthrottled line here would
            // fill the log at ten a second for as long as the descriptors stay exhausted.
            if (_failureThrottle.shouldLog(accepted.error()))
                errorLog()("{}: accept failed: {} ({} consecutive); retrying",
                           name,
                           accepted.error().toString(),
                           _failureThrottle.consecutive());
            co_await _loop.delay(100ms);
            continue;
        }

        ++_acceptedCount;
        auto id =
            ConnectionId { .endpoint = name, .index = _acceptedCount, .peer = (*accepted)->peerAddress() };
        if (connectionLog)
            connectionLog()("{}: accepted", id);
        // One flow per connection, socket ownership moved into its frame; the
        // loop reaps the frame when the connection flow finishes. Supervised, so an exception
        // escaping the flow is reported rather than reaped in silence.
        _loop.spawn(superviseConnection(_handler, std::move(id), std::move(*accepted)));
    }
}

} // namespace vthost
