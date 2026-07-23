// SPDX-License-Identifier: Apache-2.0
#include <net/posix/PosixSocket.h>

#ifndef _WIN32

    #include <sys/socket.h>

    #include <algorithm>
    #include <cerrno>
    #include <cstring>

    #include <fcntl.h>
    #include <unistd.h>

    #include <net/posix/FdUtils.h> // MSG_NOSIGNAL fallback, makeNonBlockingCloexec

    // macOS / BSD also lack MSG_CMSG_CLOEXEC (atomic close-on-exec for received
    // descriptors); there readWithFd sets FD_CLOEXEC via fcntl right after
    // receipt instead — a tiny fork race, matching what every portable imsg
    // implementation accepts on those platforms.
    #ifndef MSG_CMSG_CLOEXEC
        #define MSG_CMSG_CLOEXEC 0
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

        auto const n = _plainFd ? ::read(_fd, buffer.data(), buffer.size())
                                : ::recv(_fd, buffer.data(), buffer.size(), 0);
        if (n > 0)
            co_return static_cast<std::size_t>(n);
        if (n == 0)
            co_return std::size_t { 0 }; // clean EOF

        auto const err = errno;
        if (err == ENOTSOCK && !_plainFd)
        {
            // An adopted PTY master or pipe end (net::adoptFd): recv/send do
            // not apply; detect once, serve via plain read/write from now on.
            _plainFd = true;
            continue;
        }
        if (err == EIO && _plainFd)
            co_return std::size_t { 0 }; // a PTY master reports child exit as EIO
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
        co_return std::unexpected(fromErrno(err, _plainFd ? "read" : "recv"));
    }
}

coro::Task<std::expected<ReadWithFd, NetError>> PosixSocket::readWithFd(std::span<std::byte> buffer)
{
    while (true)
    {
        if (_closed || _fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "read on closed socket"));

        if (_plainFd)
        {
            // A PTY/pipe fd cannot carry SCM_RIGHTS; serve as a plain read.
            auto const n = co_await read(buffer);
            if (!n)
                co_return std::unexpected(n.error());
            co_return ReadWithFd { .bytesRead = *n, .fd = -1 };
        }

        auto iov = ::iovec { .iov_base = buffer.data(), .iov_len = buffer.size() };
        alignas(::cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        auto msg = ::msghdr {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        auto const n = ::recvmsg(_fd, &msg, MSG_CMSG_CLOEXEC);
        if (n >= 0)
        {
            // Keep the FIRST received fd; close any extras (mirroring the
            // rewritten-imsg receive semantics: one fd per message).
            auto fd = -1;
            // A peer advertising more fds than the one-fd contract allows must not
            // let cmsg_len drive reads (or closes) past the control buffer: on
            // MSG_CTRUNC the kernel reports the FULL sent length even though only
            // part of it landed. Clamp to capacity, and distrust the whole set on
            // truncation — close what arrived, keep nothing.
            auto const capacity = (sizeof(control) - CMSG_LEN(0)) / sizeof(int);
            auto const truncated = (msg.msg_flags & MSG_CTRUNC) != 0;
            for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg))
            {
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                    continue;
                auto const count = std::min((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int), capacity);
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto received = -1;
                    std::memcpy(&received, CMSG_DATA(cmsg) + (i * sizeof(int)), sizeof(int));
                    if (fd < 0 && !truncated)
                        fd = received;
                    else
                        ::close(received);
                }
                if (fd >= 0 && MSG_CMSG_CLOEXEC == 0) // no atomic close-on-exec on this platform
                    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            if (n == 0 && fd >= 0)
            {
                ::close(fd); // an fd on EOF has no message to belong to
                fd = -1;
            }
            co_return ReadWithFd { .bytesRead = static_cast<std::size_t>(n), .fd = fd };
        }

        auto const err = errno;
        if (err == ENOTSOCK)
        {
            _plainFd = true;
            continue;
        }
        if (isWouldBlock(err))
        {
            co_await _loop.waitReadable(_fd);
            continue;
        }
        if (err == EINTR)
            continue;
        co_return std::unexpected(fromErrno(err, "recvmsg"));
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
        auto const n = _plainFd ? ::write(_fd, remaining.data(), remaining.size())
                                : ::send(_fd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
        if (n > 0)
        {
            total += static_cast<std::size_t>(n);
            continue;
        }

        auto const err = errno;
        if (err == ENOTSOCK && !_plainFd)
        {
            _plainFd = true;
            continue;
        }
        if (isWouldBlock(err))
        {
            co_await _loop.waitWritable(_fd);
            continue;
        }
        if (err == EINTR)
            continue;
        co_return std::unexpected(fromErrno(err, _plainFd ? "write" : "send"));
    }
    co_return total;
}

} // namespace net

#endif // !_WIN32
