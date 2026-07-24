// SPDX-License-Identifier: Apache-2.0
#include <vthost/ConnectionAcceptor.h>

#include <chrono>
#include <utility>

#include <net/AsyncBufferedReader.h>

namespace vthost
{

using namespace std::chrono_literals;

ConnectionAcceptor::ConnectionAcceptor(net::EventLoop& loop,
                                       std::unique_ptr<net::IListener> listener,
                                       ConnectionHandler handler):
    _loop(loop), _listener(std::move(listener)), _handler(std::move(handler))
{
}

coro::Task<void> ConnectionAcceptor::serve()
{
    while (true)
    {
        auto accepted = co_await _listener->accept();
        if (!accepted.has_value())
        {
            if (accepted.error().code == net::NetErrorCode::Cancelled)
                co_return; // listener closed / shutdown requested
            // Persistent failures (EMFILE/ENFILE fd exhaustion) fail synchronously
            // with no suspension point — a bare `continue` would spin this loop and
            // starve every other flow on the event loop. Back off, then keep serving.
            co_await _loop.delay(100ms);
            continue;
        }

        ++_acceptedCount;
        // One flow per connection, socket ownership moved into its frame; the
        // loop reaps the frame when the connection flow finishes.
        _loop.spawn(_handler(std::move(*accepted)));
    }
}

coro::Task<void> drainConnection(std::unique_ptr<net::ISocket> connection)
{
    auto reader = net::AsyncBufferedReader { connection.get() };
    while (true)
    {
        auto const line = co_await reader.readLine();
        if (!line.has_value())
            co_return; // EOF or error: the connection is done
    }
}

} // namespace vthost
