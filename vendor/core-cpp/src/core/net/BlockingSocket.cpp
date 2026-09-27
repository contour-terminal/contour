// SPDX-License-Identifier: Apache-2.0
#include <core/net/BlockingSocket.hpp>

#include <core/net/SocketContract.hpp>
#include <core/net/detail/BlockingPrimitives.hpp>

#include <expected>
#include <string>
#include <utility>

namespace core::net
{

namespace
{
    /// @param what Which verb is refusing.
    /// @return What every verb answers on a socket that is already closed.
    [[nodiscard]] NetError closedSocket(char const* what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::string { what } + " on closed socket");
    }
} // namespace

BlockingSocket::BlockingSocket(platform::NativeHandle socket, std::string peerAddress) noexcept:
    _socket { socket }, _peerAddress { std::move(peerAddress) }
{
    // Every route to a connected blocking socket constructs through here, so this is the one place
    // the per-socket SIGPIPE suppression has to be applied.
    detail::armNoSigPipe(_socket);
}

BlockingSocket::~BlockingSocket()
{
    close();
}

IoAwaitable BlockingSocket::read(std::span<std::byte> buffer)
{
    contract::requireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("read")) };

    auto received = detail::receiveSome(_socket, buffer);
    if (received.has_value() && *received == 0)
        _peerClosed = true; // the EOF latch `ISocket::isClosed` documents
    return IoAwaitable { std::move(received) };
}

IoAwaitable BlockingSocket::waitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("waitReadable")) };
    return IoAwaitable { detail::waitReadable(_socket) };
}

IoResult BlockingSocket::sendAll(std::span<std::byte const> buffer) const
{
    auto written = std::size_t { 0 };
    while (written < buffer.size())
    {
        auto const sent = detail::sendSome(_socket, buffer.subspan(written));
        if (!sent.has_value())
            return std::unexpected(sent.error());
        written += *sent;
    }
    return written;
}

IoAwaitable BlockingSocket::write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("write")) };
    return IoAwaitable { sendAll(buffer) };
}

IoAwaitable BlockingSocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                          std::shared_ptr<void const> /*keepAlive*/)
{
    if (_closed)
        return IoAwaitable { std::unexpected(closedSocket("write")) };

    // Every segment is sent before this returns, so the keep-alive has nothing to outlive. One send
    // loop per segment rather than a scattered send: this transport is for threads that may block,
    // and the reactor sockets carry the scatter path.
    auto total = std::size_t { 0 };
    for (auto const segment: segments)
    {
        auto const sent = sendAll(segment);
        if (!sent.has_value())
            return IoAwaitable { std::unexpected(sent.error()) };
        total += *sent;
    }
    return IoAwaitable { IoResult { total } };
}

ResultAwaitable<void> BlockingSocket::shutdownWrite()
{
    // After `close` there is no write side left to shut, and every socket here answers success
    // without acting.
    if (_closed)
        return ResultAwaitable<void> { std::expected<void, NetError> {} };
    return ResultAwaitable<void> { detail::shutdownSend(_socket) };
}

void BlockingSocket::setReceiveDeadline(std::chrono::milliseconds deadline) noexcept
{
    if (!_closed)
        detail::setIoTimeout(_socket, detail::IoDirection::Receive, deadline);
}

void BlockingSocket::setSendDeadline(std::chrono::milliseconds deadline) const noexcept
{
    if (!_closed)
        detail::setIoTimeout(_socket, detail::IoDirection::Send, deadline);
}

void BlockingSocket::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    detail::closeSocket(std::exchange(_socket, platform::InvalidHandle));
}

} // namespace core::net
