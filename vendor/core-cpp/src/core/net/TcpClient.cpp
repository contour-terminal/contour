// SPDX-License-Identifier: Apache-2.0
#include <core/net/TcpClient.hpp>

#include <core/net/BlockingConnector.hpp>

#include <utility>

namespace core::net
{

async::Task<SocketResult> connectTcp(std::string host,
                                     std::uint16_t port,
                                     std::chrono::milliseconds connectTimeout,
                                     std::chrono::milliseconds ioTimeout)
{
    // The connector outlives the await because it is a local of THIS coroutine's frame, not of the
    // call expression -- the whole reason this is a coroutine rather than a function returning the
    // connector's task.
    auto connector =
        BlockingConnector { defaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = ioTimeout } };
    co_return co_await connector.connect(
        std::move(host), port, DialOptions { .connectTimeout = connectTimeout, .keepAlive = KeepAlive::No });
}

async::Task<bool> sendAll(ISocket* socket, std::span<std::byte const> bytes)
{
    auto sent = std::size_t { 0 };
    while (sent < bytes.size())
    {
        // A peer that closed mid-transfer surfaces here as an error rather than as a fatal signal,
        // because every socket this library hands out suppresses SIGPIPE per socket.
        auto const wrote = co_await socket->write(bytes.subspan(sent));
        if (!wrote.has_value() || *wrote == 0)
            co_return false;
        sent += *wrote;
    }
    co_return true;
}

async::Task<std::optional<std::vector<std::byte>>> receiveExactly(ISocket* socket, std::size_t count)
{
    if (count == 0)
        co_return std::vector<std::byte> {};

    auto out = std::vector<std::byte>(count);
    auto got = std::size_t { 0 };
    while (got < count)
    {
        auto const read = co_await socket->read(std::span { out }.subspan(got));
        // Zero is EOF, which here means the peer closed before it sent everything it declared --
        // a short frame, not a short read to retry.
        if (!read.has_value() || *read == 0)
            co_return std::nullopt;
        got += *read;
    }
    co_return std::move(out);
}

} // namespace core::net
