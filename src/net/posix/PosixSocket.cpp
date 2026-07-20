// SPDX-License-Identifier: Apache-2.0
#include <net/posix/PosixSocket.h>

#ifndef _WIN32

    #include <sys/socket.h>

    #include <cerrno>

    #include <unistd.h>

    // macOS / BSD lack MSG_NOSIGNAL; they suppress SIGPIPE via the SO_NOSIGPIPE
    // socket option instead (set in the constructor). Fall back to 0 for the send
    // flag there so the call still compiles and behaves.
    #ifndef MSG_NOSIGNAL
        #define MSG_NOSIGNAL 0
    #endif

namespace net
{

namespace
{
    /// @return True if @p err is the transient "retry when ready" errno.
    [[nodiscard]] bool isWouldBlock(int err) noexcept
    {
        return err == EAGAIN || err == EWOULDBLOCK;
    }

    /// Maps an errno from a socket call to a NetError category.
    [[nodiscard]] NetError fromErrno(int err, std::string context)
    {
        auto code = NetErrorCode::Other;
        switch (err)
        {
            case ECONNRESET: code = NetErrorCode::ConnReset; break;
            case EPIPE: code = NetErrorCode::ConnReset; break;
            case EBADF: code = NetErrorCode::BadHandle; break;
            default: break;
        }
        return makeNetError(code, err, std::move(context));
    }
} // namespace

PosixSocket::PosixSocket(EventLoop& loop, int fd, std::string peerAddress) noexcept:
    _loop(loop), _fd(fd), _peerAddress(std::move(peerAddress))
{
    #ifdef SO_NOSIGPIPE
    // macOS / BSD: suppress SIGPIPE on writes to a peer-closed socket at the socket
    // level (the portable analogue of Linux's MSG_NOSIGNAL send flag).
    int const one = 1;
    if (_fd >= 0)
        ::setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    #endif
}

PosixSocket::~PosixSocket()
{
    close();
}

void PosixSocket::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
}

coro::Task<IoResult> PosixSocket::read(std::span<std::byte> buffer)
{
    while (true)
    {
        if (_closed || _fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "read on closed socket"));

        auto const n = ::recv(_fd, buffer.data(), buffer.size(), 0);
        if (n > 0)
            co_return static_cast<std::size_t>(n);
        if (n == 0)
            co_return std::size_t { 0 }; // clean EOF

        auto const err = errno;
        if (isWouldBlock(err))
        {
            // Park until the fd is readable, then retry. A cancelled wait throws
            // OperationCancelled, which unwinds the caller — the right behaviour for
            // a cancelled connection.
            co_await _loop.waitReadable(_fd);
            continue;
        }
        if (err == EINTR)
            continue;
        co_return std::unexpected(fromErrno(err, "recv"));
    }
}

coro::Task<IoResult> PosixSocket::write(std::span<std::byte const> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        if (_closed || _fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "write on closed socket"));

        auto const remaining = buffer.subspan(total);
        // MSG_NOSIGNAL: a write to a peer-closed socket returns EPIPE rather than
        // raising SIGPIPE and killing the process.
        auto const n = ::send(_fd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
        if (n > 0)
        {
            total += static_cast<std::size_t>(n);
            continue;
        }

        auto const err = errno;
        if (isWouldBlock(err))
        {
            co_await _loop.waitWritable(_fd);
            continue;
        }
        if (err == EINTR)
            continue;
        co_return std::unexpected(fromErrno(err, "send"));
    }
    co_return total;
}

} // namespace net

#endif // !_WIN32
