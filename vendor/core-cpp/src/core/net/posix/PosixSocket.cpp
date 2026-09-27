// SPDX-License-Identifier: Apache-2.0
#include <core/net/posix/PosixSocket.hpp>

#include <core/net/SocketContract.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/WouldBlock.hpp>
#include <core/net/posix/FdUtils.hpp> // MSG_NOSIGNAL fallback, makeNonBlockingCloexec

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ranges>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

// macOS / BSD also lack MSG_CMSG_CLOEXEC (atomic close-on-exec for received descriptors); there
// readWithFd sets FD_CLOEXEC via fcntl right after receipt instead — a tiny fork race, matching
// what every portable imsg implementation accepts on those platforms.
#ifndef MSG_CMSG_CLOEXEC
    #define MSG_CMSG_CLOEXEC 0
#endif

namespace core::net
{

using detail::isWouldBlock;

namespace
{
    /// How many segments one `sendmsg` carries. A cursor drives the rest, so this bounds the
    /// stack array rather than the write: a caller passing more segments than this simply takes
    /// more syscalls, which is what a partial send would have cost anyway.
    constexpr std::size_t MaxIoVectors = 16;

    /// @param what Which verb is refusing.
    /// @return The error a verb reports on a socket that is already closed.
    [[nodiscard]] NetError closedSocket(char const* what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::string { what } + " on closed socket");
    }

    /// @return The error a verb reports when the loop would not watch the descriptor. Distinct
    ///         from a cancellation: nothing was cancelled, the registration was refused, and a
    ///         caller that cannot tell them apart retries a socket that can never become ready.
    [[nodiscard]] NetError registrationRefused()
    {
        return makeNetError(NetErrorCode::SystemError, 0, "the event loop refused to watch this socket");
    }
} // namespace

PosixSocket::PosixSocket(EventLoop& loop, int fd, std::string peerAddress) noexcept:
    _loop(loop), _fd(fd), _peerAddress(std::move(peerAddress))
{
#ifdef SO_NOSIGPIPE
    // macOS / BSD: suppress SIGPIPE on writes to a peer-closed socket at the socket level (the
    // portable analogue of Linux's MSG_NOSIGNAL send flag).
    int const one = 1;
    if (_fd >= 0)
        ::setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

PosixSocket::~PosixSocket()
{
    contract::assertTeardownIsSerialisedWithDispatch(_loop);
    // Cancel, not Resume: a parked flow resumed on its normal path would read `_fd` and `_closed`
    // through a `this` that is about to stop existing. Abandoning it unwinds the frame through
    // OperationCancelled, which never re-enters the body.
    close(FdWakePolicy::Cancel);
}

void PosixSocket::close() noexcept
{
    close(FdWakePolicy::Resume);
}

void PosixSocket::close(FdWakePolicy policy) noexcept
{
    if (_closed)
        return;
    _closed = true;

    // **Detached FIRST, completed LAST, with no member touched in between.** Completing SETTLES
    // the parked operation and hands its coroutine to the loop, which resumes it after this
    // returns (G2) -- but a destructor abandons into the same slots, and an owner that named no
    // loop is still resumed inline, so the discipline stays: both operations are taken into locals
    // before either is settled, and nothing after the first settle reads `this`.
    auto read = takeRead();
    auto write = takeWrite();

    if (_fd >= 0)
    {
        // Before the close, while the descriptor is still valid: epoll and kqueue cannot report a
        // closed descriptor, so without this a flow parked on it would never be resumed. It also
        // releases the private dup() a duplicate registration holds, and ends the registration this
        // socket keeps for its life -- which is the promise `RegistrationLifetime::UntilClosed`
        // asks of whoever parks with it, and the reason this socket may.
        _loop.notifyHandleClosing(_fd, policy);
        ::close(_fd);
        _fd = -1;
    }

    // Past this point nothing may touch a member. The awaitables live in their own coroutines'
    // frames rather than in this object, so they stay valid once the first resume has taken the
    // socket down.
    if (policy == FdWakePolicy::Cancel)
    {
        abandonRead(read);
        if (write.awaitable != nullptr)
            write.awaitable->abandon();
        return;
    }
    settleRead(read, makeNetError(NetErrorCode::Cancelled, 0, "the socket was closed"));
    if (write.awaitable != nullptr)
        write.awaitable->complete(
            std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "the socket was closed")));
}

void PosixSocket::cancelRead() noexcept
{
    if (_closed || _read.kind == ReadKind::None || _read.awaitable == nullptr)
        return;
    // The detach-then-complete discipline, for the one caller-facing verb that uses it without
    // closing the socket: the slot is free when this returns and the operation is settled with
    // `Cancelled` at once, because a readiness transport consumes nothing and so a retired read can
    // lose nothing. Its flow is resumed by the loop, not here.
    auto read = takeRead();
    settleRead(read, makeNetError(NetErrorCode::Cancelled, 0, "the read was retired by cancelRead"));
}

ResultAwaitable<void> PosixSocket::shutdownWrite()
{
    // Completes INLINE: `::shutdown` is a syscall that either takes or does not, with nothing to
    // flush first. The awaitable is the interface's shape, not a cost this transport pays.
    if (_closed || _fd < 0 || _plainFd)
        return ResultAwaitable<void> { std::expected<void, NetError> {} };
    if (::shutdown(_fd, SHUT_WR) < 0)
    {
        auto const err = errno;
        // ENOTCONN is not a failure to report: the peer is already gone, which is the state the
        // caller was asking for.
        if (err != ENOTCONN)
            return ResultAwaitable<void> { std::unexpected(detail::socketError(err, "shutdown")) };
    }
    return ResultAwaitable<void> { std::expected<void, NetError> {} };
}

void PosixSocket::setReceiveDeadline(std::chrono::milliseconds deadline) noexcept
{
    // Non-positive REMOVES the bound, which is SO_RCVTIMEO's own reading of zero: "If the timeout
    // is set to zero (the default), then the operation will never timeout" (man 7 socket). Storing
    // it is the whole of the removal -- `armRead` arms a timer only for a positive value -- and a
    // read already parked keeps the timer it armed, which is what the interface says.
    _receiveDeadline = std::max(deadline, std::chrono::milliseconds::zero());
}

// ---- The read side ----------------------------------------------------------------------------

std::optional<IoResult> PosixSocket::tryRead(std::span<std::byte> buffer)
{
    while (true)
    {
        auto const n = _plainFd ? ::read(_fd, buffer.data(), buffer.size())
                                : ::recv(_fd, buffer.data(), buffer.size(), 0);
        if (n > 0)
            return IoResult { static_cast<std::size_t>(n) };
        if (n == 0)
        {
            _peerClosed = true; // isClosed() now answers true, as ISocket documents
            return IoResult { std::size_t { 0 } };
        }

        auto const err = errno;
        if (err == ENOTSOCK && !_plainFd)
        {
            // An adopted PTY master or pipe end (net::adoptFd): recv/send do not apply; detect
            // once, serve via plain read/write from now on.
            _plainFd = true;
            continue;
        }
        if (err == EIO && _plainFd)
        {
            _peerClosed = true;                    // the child is gone: an EOF by another name
            return IoResult { std::size_t { 0 } }; // a PTY master reports child exit as EIO
        }
        if (isWouldBlock(err) || err == EINTR)
            return std::nullopt;
        return IoResult { std::unexpected(detail::socketError(err, _plainFd ? "read" : "recv")) };
    }
}

std::optional<std::expected<ReadWithFd, NetError>> PosixSocket::tryReadWithFd(std::span<std::byte> buffer)
{
    while (true)
    {
        if (_plainFd)
        {
            // A PTY/pipe fd cannot carry SCM_RIGHTS; serve it as a plain read.
            auto const plain = tryRead(buffer);
            if (!plain.has_value())
                return std::nullopt;
            if (!plain->has_value())
                return std::unexpected(plain->error());
            return ReadWithFd { .bytesRead = **plain, .fd = -1 };
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
            // Keep the FIRST received fd; close any extras (mirroring the rewritten-imsg receive
            // semantics: one fd per message).
            auto fd = -1;
            // A peer advertising more fds than the one-fd contract allows must not let cmsg_len
            // drive reads (or closes) past the control buffer: on MSG_CTRUNC the kernel reports the
            // FULL sent length even though only part of it landed. Clamp to capacity, and distrust
            // the whole set on truncation — close what arrived, keep nothing.
            auto const capacity = (sizeof(control) - CMSG_LEN(0)) / sizeof(int);
            auto const truncated = (msg.msg_flags & MSG_CTRUNC) != 0;
            auto* next = CMSG_FIRSTHDR(&msg);
            while (next != nullptr)
            {
                // Step to the next header first, so the `continue` below moves on to it.
                auto* cmsg = std::exchange(next, CMSG_NXTHDR(&msg, next));
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                    continue;
                auto const count = std::min((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int), capacity);
                for (auto const i: std::views::iota(std::size_t { 0 }, count))
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
            if (n == 0)
            {
                _peerClosed = true; // the same EOF latch read() keeps, on the fd-passing path
                if (fd >= 0)
                {
                    ::close(fd); // an fd on EOF has no message to belong to
                    fd = -1;
                }
            }
            return ReadWithFd { .bytesRead = static_cast<std::size_t>(n), .fd = fd };
        }

        auto const err = errno;
        if (err == ENOTSOCK)
        {
            _plainFd = true;
            continue;
        }
        if (isWouldBlock(err) || err == EINTR)
            return std::nullopt;
        return std::unexpected(detail::socketError(err, "recvmsg"));
    }
}

std::optional<IoResult> PosixSocket::tryProbe()
{
    // A PTY master or pipe end cannot be peeked, so it is probed rather than peeked: FIONREAD says
    // how many bytes are readable now, and POLLHUP distinguishes "nothing yet" from "the writer is
    // gone". Both are non-destructive, which is what `waitReadable` promises.
    //
    // The version reviewed in round 1 answered a flat `1` here without consulting the descriptor at
    // all, so a `waitReadable` on a PTY never suspended and a watchdog loop holding one became a
    // turn-free spin.
    if (_plainFd)
    {
        auto pending = 0;
        if (::ioctl(_fd, FIONREAD, &pending) < 0)
        {
            auto const err = errno;
            if (isWouldBlock(err) || err == EINTR)
                return std::nullopt;
            if (err == EIO)
                return IoResult { std::size_t { 0 } }; // a PTY master reports child exit as EIO
            return IoResult { std::unexpected(detail::socketError(err, "ioctl(FIONREAD)")) };
        }
        if (pending > 0)
            return IoResult { std::size_t { 1 } };

        // Nothing pending. POLLHUP is the only non-destructive way to tell a writer that has gone
        // from one that has simply not written yet, and answering the second as EOF would report a
        // healthy descriptor dead.
        auto watch = pollfd { .fd = _fd, .events = POLLIN, .revents = 0 };
        if (::poll(&watch, 1, 0) > 0 && (watch.revents & POLLHUP) != 0)
            return IoResult { std::size_t { 0 } };
        return std::nullopt;
    }

    // A one-byte MSG_PEEK: `0` is EOF and `>0` is data pending, which is the whole of
    // `waitReadable`'s contract. It consumes nothing, so a probe can be retried and retired freely.
    auto probe = std::array<std::byte, 1> {};
    auto const got = ::recv(_fd, probe.data(), probe.size(), MSG_PEEK);
    if (got >= 0)
        return IoResult { got == 0 ? std::size_t { 0 } : std::size_t { 1 } };

    auto const err = errno;
    if (err == ENOTSOCK)
    {
        // An adopted PTY master or pipe end, discovered HERE rather than by a preceding read --
        // `waitReadable` is documented to be callable first, so there need not have been one.
        _plainFd = true;
        return tryProbe();
    }
    if (isWouldBlock(err) || err == EINTR)
        return std::nullopt;

    // **A negative peek is TWO things, and answering both with `1` says the opposite of what
    // happened for the second.** EAGAIN/EINTR is a spurious readiness with nothing behind it, and
    // parking again is right. Anything else — ECONNRESET above all — is the peer GONE, and a
    // watcher that never reads would take "one byte is pending" as proof of life
    // ([fastcached#899](https://github.com/LASTRADA-Software/fastcached/issues/899)).
    return IoResult { std::unexpected(detail::socketError(err, "recv")) };
}

IoAwaitable PosixSocket::read(std::span<std::byte> buffer)
{
    contract::requireReadBuffer(buffer);
    if (_closed || _fd < 0)
        return IoAwaitable { std::unexpected(closedSocket("read")) };

    if (auto const done = tryRead(buffer); done.has_value())
        return IoAwaitable { *done };

    contract::claimReadSlot(_read.awaitable, _fd);
    _read.kind = ReadKind::Bytes;
    _read.buffer = buffer;
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<PosixSocket*>(owner);
                            // **The claim is HERE, because this is where the slot is actually taken.** The
                            // verb's guard is the early, friendlier diagnostic; it cannot see two operations
                            // created through `core::async::asTask` and awaited afterwards, because neither
                            // has armed when the second verb runs -- and the second arm then overwrites this
                            // slot and the park id, leaving the first park registered on a socket that will
                            // be freed under it.
                            contract::claimReadSlot(socket->_read.awaitable, socket->_fd);
                            socket->_read.awaitable = &self;
                            if (!socket->armRead(Interest::Read))
                            {
                                socket->_read = {};
                                self.complete(std::unexpected(registrationRefused()));
                                return;
                            }
                            self.cancelThrough(socket->_loop, socket->_read.park);
                        },
                         &PosixSocket::retireRead,
                         this };
}

ResultAwaitable<ReadWithFd> PosixSocket::readWithFd(std::span<std::byte> buffer)
{
    contract::requireReadBuffer(buffer);
    if (_closed || _fd < 0)
        return ResultAwaitable<ReadWithFd> { std::unexpected(closedSocket("read")) };

    if (auto done = tryReadWithFd(buffer); done.has_value())
        return ResultAwaitable<ReadWithFd> { std::move(*done) };

    contract::claimReadSlot(_read.awaitable, _fd);
    _read.kind = ReadKind::WithFd;
    _read.buffer = buffer;
    return ResultAwaitable<ReadWithFd> { [](void* owner, ResultAwaitable<ReadWithFd>& self) {
                                            auto* const socket = static_cast<PosixSocket*>(owner);
                                            contract::claimReadSlot(socket->_read.awaitable, socket->_fd);
                                            socket->_read.awaitable = &self;
                                            if (!socket->armRead(Interest::Read))
                                            {
                                                socket->_read = {};
                                                self.complete(std::unexpected(registrationRefused()));
                                                return;
                                            }
                                            self.cancelThrough(socket->_loop, socket->_read.park);
                                        },
                                         &PosixSocket::retireRead,
                                         this };
}

IoAwaitable PosixSocket::waitReadable()
{
    if (_closed || _fd < 0)
        return IoAwaitable { std::unexpected(closedSocket("waitReadable")) };

    if (auto const done = tryProbe(); done.has_value())
        return IoAwaitable { *done };

    contract::claimReadSlot(_read.awaitable, _fd);
    _read.kind = ReadKind::Probe;
    _read.buffer = {};
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<PosixSocket*>(owner);
                            contract::claimReadSlot(socket->_read.awaitable, socket->_fd);
                            socket->_read.awaitable = &self;
                            if (!socket->armRead(Interest::Read))
                            {
                                socket->_read = {};
                                self.complete(std::unexpected(registrationRefused()));
                                return;
                            }
                            self.cancelThrough(socket->_loop, socket->_read.park);
                        },
                         &PosixSocket::retireRead,
                         this };
}

bool PosixSocket::armRead(Interest interest)
{
    _read.park = _loop.registerPark(ParkEntry::onReadyCallback(
        &PosixSocket::onReadWake, this, _fd, DefaultHandleKind, interest, RegistrationLifetime::UntilClosed));
    if (!_read.park)
        return false;

    // The receive deadline is armed HERE and nowhere else, so it bounds exactly what it says it
    // bounds: one read that had to wait. It is a timer on the loop's ONE deadline heap rather than
    // a wake this socket computes for itself — a second "when should we next wake" is the design
    // fault whose symptom is a wait that is too long, which is a hang rather than a failure.
    if (_receiveDeadline > std::chrono::milliseconds::zero())
        _read.deadline =
            _loop.addTimer(_loop.clock().now() + _receiveDeadline, &PosixSocket::onReadDeadline, this);
    return true;
}

void PosixSocket::pumpRead()
{
    if (_read.awaitable == nullptr)
        return;

    switch (_read.kind)
    {
        case ReadKind::None: return;

        case ReadKind::Bytes: {
            auto const done = tryRead(_read.buffer);
            if (!done.has_value())
                return; // a spurious readiness; stay parked for the next one
            auto operation = takeRead();
            static_cast<IoAwaitable*>(operation.awaitable)->complete(*done);
            return;
        }

        case ReadKind::WithFd: {
            auto done = tryReadWithFd(_read.buffer);
            if (!done.has_value())
                return;
            auto operation = takeRead();
            static_cast<ResultAwaitable<ReadWithFd>*>(operation.awaitable)->complete(std::move(*done));
            return;
        }

        case ReadKind::Probe: {
            auto const done = tryProbe();
            if (!done.has_value())
                return;
            auto operation = takeRead();
            static_cast<IoAwaitable*>(operation.awaitable)->complete(*done);
            return;
        }
    }
}

PosixSocket::ReadOperation PosixSocket::takeRead() noexcept
{
    auto taken = std::exchange(_read, ReadOperation {});
    if (taken.deadline)
        std::ignore = _loop.cancelTimer(taken.deadline);
    if (taken.park)
        _loop.unregisterPark(taken.park);
    return taken;
}

void PosixSocket::settleRead(ReadOperation& operation, NetError error) noexcept
{
    // **A read with no awaitable is a real state, not a defensive `if`.** The verb records the kind
    // and the buffer when it is CALLED, while the awaitable is recorded when it is AWAITED -- and
    // `[[nodiscard]]` makes dropping one in between a warning rather than an impossibility. So a
    // caller that creates a read and never awaits it leaves the slot naming an operation with no
    // frame behind it, and `close()` then arrives here with nothing to complete.
    if (operation.awaitable == nullptr)
        return;

    switch (operation.kind)
    {
        case ReadKind::None: return;
        case ReadKind::Bytes:
        case ReadKind::Probe:
            static_cast<IoAwaitable*>(operation.awaitable)->complete(std::unexpected(std::move(error)));
            return;
        case ReadKind::WithFd:
            static_cast<ResultAwaitable<ReadWithFd>*>(operation.awaitable)
                ->complete(std::unexpected(std::move(error)));
            return;
    }
}

void PosixSocket::abandonRead(ReadOperation& operation) noexcept
{
    if (operation.awaitable == nullptr)
        return; // created and never awaited; see settleRead

    switch (operation.kind)
    {
        case ReadKind::None: return;
        case ReadKind::Bytes:
        case ReadKind::Probe: static_cast<IoAwaitable*>(operation.awaitable)->abandon(); return;
        case ReadKind::WithFd:
            static_cast<ResultAwaitable<ReadWithFd>*>(operation.awaitable)->abandon();
            return;
    }
}

void PosixSocket::onReadWake(void* state, ParkWake wake)
{
    auto* const socket = static_cast<PosixSocket*>(state);
    if (socket->_read.kind == ReadKind::None || socket->_read.awaitable == nullptr)
        return;
    switch (wake)
    {
        case ParkWake::Ready: socket->pumpRead(); return;
        case ParkWake::Cancelled: {
            // The awaiting flow's own token was stopped. The operation is settled with a value and
            // `await_resume` turns it into an OperationCancelled, because the token it reads is
            // the one that was stopped — the value here is what a flow whose token is NOT stopped
            // would have seen, and nothing observes it.
            auto operation = socket->takeRead();
            settleRead(operation,
                       makeNetError(NetErrorCode::Cancelled, 0, "the awaiting flow was cancelled"));
            return;
        }
        case ParkWake::Abandoned: {
            auto operation = socket->takeRead();
            abandonRead(operation);
            return;
        }
    }
}

void PosixSocket::onReadDeadline(void* state)
{
    auto* const socket = static_cast<PosixSocket*>(state);
    if (socket->_read.kind == ReadKind::None || socket->_read.awaitable == nullptr)
        return;
    // Taken first: the timer has fired, so its id is spent, and `takeRead`'s cancelTimer resolves
    // to nothing — which is the generation check doing the work rather than a flag.
    auto operation = socket->takeRead();
    settleRead(operation,
               makeNetError(NetErrorCode::Timeout, 0, "the receive deadline elapsed before any data"));
}

void PosixSocket::retireRead(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<PosixSocket*>(owner);
    // Identity, not merely "something is parked": an operation that was retired and replaced must
    // not be able to retire its successor.
    if (socket->_read.awaitable != awaitable)
        return;
    std::ignore = socket->takeRead();
}

// ---- The write side ---------------------------------------------------------------------------

std::optional<IoResult> PosixSocket::trySend(WriteOperation& operation)
{
    return operation.segments.empty() ? trySendFlat(operation) : trySendSegments(operation);
}

std::optional<IoResult> PosixSocket::trySendFlat(WriteOperation& operation)
{
    while (!operation.remaining.empty())
    {
        // MSG_NOSIGNAL: a write to a peer-closed socket returns EPIPE rather than raising SIGPIPE
        // and killing the process.
        auto const n =
            _plainFd ? ::write(_fd, operation.remaining.data(), operation.remaining.size())
                     : ::send(_fd, operation.remaining.data(), operation.remaining.size(), MSG_NOSIGNAL);
        auto const err = errno; // captured before any branch: everything below reads THIS call
        if (n > 0)
        {
            operation.written += static_cast<std::size_t>(n);
            operation.remaining = operation.remaining.subspan(static_cast<std::size_t>(n));
            continue;
        }
        // A zero return on a non-empty buffer is neither progress nor a named failure, and `errno`
        // does not describe it -- it still holds whatever the previous syscall left. Falling through
        // to the ladder below therefore spun on an already-writable socket, retried for ever, or
        // reported a failure that never happened, depending on that stale value. Report the one
        // thing that is true: the transport took nothing.
        if (n == 0)
            return IoResult { std::unexpected(
                makeNetError(NetErrorCode::SystemError,
                             0,
                             _plainFd ? "write accepted no bytes" : "send accepted no bytes")) };
        if (err == ENOTSOCK && !_plainFd)
        {
            // An adopted PTY master or pipe end: send does not apply; serve via plain write.
            _plainFd = true;
            continue;
        }
        if (err == EINTR)
            continue; // interrupted before anything moved; the same send again
        if (isWouldBlock(err))
            return std::nullopt;
        return IoResult { std::unexpected(detail::socketError(err, _plainFd ? "write" : "send")) };
    }
    return IoResult { operation.written };
}

void PosixSocket::advanceSegmentCursor(WriteOperation& operation, std::size_t sent) noexcept
{
    auto left = sent;
    while (left > 0 && operation.segmentIndex < operation.segments.size())
    {
        auto const inThisSegment =
            operation.segments[operation.segmentIndex].size() - operation.segmentOffset;
        if (left < inThisSegment)
        {
            operation.segmentOffset += left;
            return;
        }
        left -= inThisSegment;
        ++operation.segmentIndex;
        operation.segmentOffset = 0;
    }
}

std::optional<IoResult> PosixSocket::trySendSegments(WriteOperation& operation)
{
    while (operation.segmentIndex < operation.segments.size())
    {
        auto vectors = std::array<::iovec, MaxIoVectors> {};
        auto count = std::size_t { 0 };
        auto offset = operation.segmentOffset;
        for (auto const index: std::views::iota(operation.segmentIndex, operation.segments.size())
                                   | std::views::take(MaxIoVectors))
        {
            auto const segment = operation.segments[index].subspan(offset);
            offset = 0;
            if (segment.empty())
                continue;
            vectors.at(count) =
                ::iovec { .iov_base = const_cast<std::byte*>(segment.data()), .iov_len = segment.size() };
            ++count;
        }
        if (count == 0)
            break; // every remaining segment is empty; there is nothing left to send

        auto msg = ::msghdr {};
        msg.msg_iov = vectors.data();
        msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(count);
        auto const n = ::sendmsg(_fd, &msg, MSG_NOSIGNAL);
        auto const err = errno;
        if (n > 0)
        {
            operation.written += static_cast<std::size_t>(n);
            advanceSegmentCursor(operation, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            return IoResult { std::unexpected(
                makeNetError(NetErrorCode::SystemError, 0, "sendmsg accepted no bytes")) };
        if (err == EINTR)
            continue;
        if (isWouldBlock(err))
            return std::nullopt;
        return IoResult { std::unexpected(detail::socketError(err, "sendmsg")) };
    }

    operation.segmentIndex = operation.segments.size();
    operation.segmentOffset = 0;
    return IoResult { operation.written };
}

IoAwaitable PosixSocket::write(std::span<std::byte const> buffer)
{
    // **The guard comes before `_write` is touched, and the attempt runs on a TEMPORARY.** The
    // first version of this verb wrote the new cursor into `_write`, called `trySend`, and reached
    // the guard only on the parking branch -- so a second write issued while one was parked
    // clobbered the parked operation's cursor, and on the inline-completion branch ran `_write = {}`
    // and returned success having silently dropped the parked awaitable and leaked its park, with
    // no assertion on that path in Debug or Release. Nothing owns `_write` until the operation is
    // known to park.
    contract::claimWriteSlot(_write.awaitable, _fd);

    if (_closed || _fd < 0)
        return IoAwaitable { std::unexpected(closedSocket("write")) };

    auto pending = WriteOperation {};
    pending.remaining = buffer;
    if (auto const done = trySend(pending); done.has_value())
        return IoAwaitable { *done };

    _write = std::move(pending);
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<PosixSocket*>(owner);
                            // **The claim is HERE, because this is where the slot is actually taken.** The
                            // verb's guard above is the early, friendlier diagnostic; it cannot see two
                            // operations created through `core::async::asTask` and awaited afterwards,
                            // because neither has armed when the second verb runs.
                            contract::claimWriteSlot(socket->_write.awaitable, socket->_fd);
                            socket->_write.awaitable = &self;
                            if (!socket->armWrite())
                            {
                                socket->_write = {};
                                self.complete(std::unexpected(registrationRefused()));
                                return;
                            }
                            self.cancelThrough(socket->_loop, socket->_write.park);
                        },
                         &PosixSocket::retireWrite,
                         this };
}

IoAwaitable PosixSocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                       std::shared_ptr<void const> keepAlive)
{
    // Guard first, attempt on a temporary -- see `write` above for what the other order cost.
    contract::claimWriteSlot(_write.awaitable, _fd);

    if (_closed || _fd < 0)
        return IoAwaitable { std::unexpected(closedSocket("write")) };

    auto pending = WriteOperation {};
    pending.segments = segments;
    pending.keepAlive = std::move(keepAlive);
    if (auto const done = trySend(pending); done.has_value())
        return IoAwaitable { *done };

    _write = std::move(pending);
    return IoAwaitable { [](void* owner, IoAwaitable& self) {
                            auto* const socket = static_cast<PosixSocket*>(owner);
                            contract::claimWriteSlot(socket->_write.awaitable, socket->_fd);
                            socket->_write.awaitable = &self;
                            if (!socket->armWrite())
                            {
                                socket->_write = {};
                                self.complete(std::unexpected(registrationRefused()));
                                return;
                            }
                            self.cancelThrough(socket->_loop, socket->_write.park);
                        },
                         &PosixSocket::retireWrite,
                         this };
}

bool PosixSocket::armWrite()
{
    _write.park = _loop.registerPark(ParkEntry::onReadyCallback(&PosixSocket::onWriteWake,
                                                                this,
                                                                _fd,
                                                                DefaultHandleKind,
                                                                Interest::Write,
                                                                RegistrationLifetime::UntilClosed));
    return static_cast<bool>(_write.park);
}

void PosixSocket::pumpWrite()
{
    if (_write.awaitable == nullptr)
        return; // created and never awaited; see settleRead
    auto const done = trySend(_write);
    if (!done.has_value())
        return; // still backed up; stay parked for the next writable edge
    auto operation = takeWrite();
    operation.awaitable->complete(*done);
}

PosixSocket::WriteOperation PosixSocket::takeWrite() noexcept
{
    auto taken = std::exchange(_write, WriteOperation {});
    if (taken.park)
        _loop.unregisterPark(taken.park);
    return taken;
}

void PosixSocket::onWriteWake(void* state, ParkWake wake)
{
    auto* const socket = static_cast<PosixSocket*>(state);
    if (socket->_write.awaitable == nullptr)
        return;
    switch (wake)
    {
        case ParkWake::Ready: socket->pumpWrite(); return;
        case ParkWake::Cancelled: {
            auto operation = socket->takeWrite();
            operation.awaitable->complete(
                std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "the awaiting flow was cancelled")));
            return;
        }
        case ParkWake::Abandoned: {
            auto operation = socket->takeWrite();
            operation.awaitable->abandon();
            return;
        }
    }
}

void PosixSocket::retireWrite(void* owner, void* awaitable) noexcept
{
    auto* const socket = static_cast<PosixSocket*>(owner);
    if (socket->_write.awaitable != awaitable)
        return;
    std::ignore = socket->takeWrite();
}

} // namespace core::net
