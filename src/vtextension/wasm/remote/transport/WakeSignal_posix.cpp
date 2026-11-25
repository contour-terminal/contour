// SPDX-License-Identifier: Apache-2.0
//
// The wake signal on POSIX: a Unix-domain stream socket pair, one byte per wake. The sender's end is
// non-blocking and its sends carry MSG_NOSIGNAL (macOS: SO_NOSIGPIPE), so a full buffer or a gone receiver
// costs the parent neither a block nor a SIGPIPE -- a pipe would raise SIGPIPE, and the parent installs no
// handler. The receiver polls its end beside a pipe of its own that stop() writes to.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vtextension/wasm/remote/transport/WakeSignal.hpp>
#include <vtextension/wasm/remote/transport/WouldBlock.hpp>

namespace vtextension::wasm::remote::transport
{

namespace
{
#ifdef MSG_NOSIGNAL
    /// Never blocks, and fails a send to a gone peer with EPIPE instead of raising SIGPIPE.
    constexpr auto SendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
    // macOS has no MSG_NOSIGNAL; the sender's socket carries SO_NOSIGPIPE instead.
    constexpr auto SendFlags = MSG_DONTWAIT;
#endif

    /// Closes @p descriptor if it is open.
    void closeDescriptor(int descriptor) noexcept
    {
        if (descriptor != -1)
            ::close(descriptor);
    }

    /// Adds @p flags to @p descriptor's file status flags.
    [[nodiscard]] std::expected<void, ChannelError> addStatusFlags(int descriptor, int flags) noexcept
    {
        auto const current = ::fcntl(descriptor, F_GETFL);
        if (current == -1 || ::fcntl(descriptor, F_SETFL, current | flags) == -1)
            return std::unexpected(ChannelError::Io);
        return {};
    }

    /// Makes @p descriptor close-on-exec.
    [[nodiscard]] std::expected<void, ChannelError> setCloseOnExec(int descriptor) noexcept
    {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
            return std::unexpected(ChannelError::Io);
        return {};
    }

    /// @return A pipe whose ends are close-on-exec and non-blocking, read end first.
    [[nodiscard]] std::expected<std::array<int, 2>, ChannelError> makeStopPipe() noexcept
    {
        auto ends = std::array<int, 2> { -1, -1 };
#ifdef __APPLE__
        if (::pipe(ends.data()) == -1)
            return std::unexpected(ChannelError::Io);
        for (auto const end: ends)
            if (!setCloseOnExec(end) || !addStatusFlags(end, O_NONBLOCK))
            {
                closeDescriptor(ends[0]);
                closeDescriptor(ends[1]);
                return std::unexpected(ChannelError::Io);
            }
#else
        if (::pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) == -1)
            return std::unexpected(ChannelError::Io);
#endif
        return ends;
    }

    /// Makes a receiver's end what wait() needs: close-on-exec, and non-blocking so that it drains.
    [[nodiscard]] std::expected<void, ChannelError> configureReceiver(int descriptor) noexcept
    {
        return setCloseOnExec(descriptor).and_then([&] { return addStatusFlags(descriptor, O_NONBLOCK); });
    }

    /// Makes a sender's end non-blocking, and on macOS SIGPIPE-free.
    [[nodiscard]] std::expected<void, ChannelError> configureSender(int descriptor) noexcept
    {
#ifdef SO_NOSIGPIPE
        auto const enable = 1;
        if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) == -1)
            return std::unexpected(ChannelError::Io);
#endif
        return setCloseOnExec(descriptor).and_then([&] { return addStatusFlags(descriptor, O_NONBLOCK); });
    }

    /// What draining a receiver's socket found.
    enum class Drained : uint8_t
    {
        Nothing,     ///< No byte, and the stream goes on.
        EndOfStream, ///< The sender is gone, and nothing was queued.
        Wakes,       ///< At least one wake was taken.
    };

    /// Takes every byte queued on @p descriptor.
    [[nodiscard]] Drained drain(int descriptor) noexcept
    {
        auto buffer = std::array<uint8_t, 256> {};
        auto drained = Drained::Nothing;
        while (true)
        {
            auto const received = ::recv(descriptor, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received > 0)
                drained = Drained::Wakes;
            else if (received == -1 && errno == EINTR)
                continue;
            else if (received == -1 && wouldBlock(errno))
                return drained;
            else
                // End of stream, or a failure that ends it as surely.
                return drained == Drained::Wakes ? drained : Drained::EndOfStream;
        }
    }
} // namespace

WakeSender::WakeSender(NativeHandle handle) noexcept: _handle { handle }
{
}

WakeSender::WakeSender(WakeSender&& other) noexcept: _handle { std::exchange(other._handle, -1) }
{
}

WakeSender& WakeSender::operator=(WakeSender&& other) noexcept
{
    if (this != &other)
    {
        closeDescriptor(_handle);
        _handle = std::exchange(other._handle, -1);
    }
    return *this;
}

WakeSender::~WakeSender()
{
    closeDescriptor(_handle);
}

WakeResult WakeSender::signal() const noexcept
{
    auto const wake = uint8_t { 1 };
    while (true)
    {
        if (::send(_handle, &wake, sizeof(wake), SendFlags) == 1)
            return WakeResult::Sent;
        if (errno == EINTR)
            continue;
        if (wouldBlock(errno))
            return WakeResult::Pending;
        // The receiver is gone (EPIPE, ECONNRESET): nobody is left to wake, and its exit is seen elsewhere.
        return WakeResult::Sent;
    }
}

NativeHandle WakeSender::nativeHandle() const noexcept
{
    return _handle;
}

WakeReceiver::WakeReceiver(NativeHandle handle, int stopRead, int stopWrite) noexcept:
    _handle { handle }, _stopRead { stopRead }, _stopWrite { stopWrite }
{
}

std::expected<WakeReceiver, ChannelError> WakeReceiver::adopt(NativeHandle handle)
{
    struct stat status {};
    if (::fstat(handle, &status) == -1 || !S_ISSOCK(status.st_mode))
        return std::unexpected(ChannelError::Io);
    return configureReceiver(handle).and_then(makeStopPipe).transform([&](std::array<int, 2> stop) {
        return WakeReceiver { handle, stop[0], stop[1] };
    });
}

WakeReceiver::WakeReceiver(WakeReceiver&& other) noexcept:
    _handle { std::exchange(other._handle, -1) },
    _stopRead { std::exchange(other._stopRead, -1) },
    _stopWrite { std::exchange(other._stopWrite, -1) }
{
}

WakeReceiver& WakeReceiver::operator=(WakeReceiver&& other) noexcept
{
    if (this != &other)
    {
        release();
        _handle = std::exchange(other._handle, -1);
        _stopRead = std::exchange(other._stopRead, -1);
        _stopWrite = std::exchange(other._stopWrite, -1);
    }
    return *this;
}

WakeReceiver::~WakeReceiver()
{
    release();
}

void WakeReceiver::release() noexcept
{
    closeDescriptor(_handle);
    closeDescriptor(_stopRead);
    closeDescriptor(_stopWrite);
    _handle = -1;
    _stopRead = -1;
    _stopWrite = -1;
}

WakeWait WakeReceiver::wait() noexcept
{
    while (true)
    {
        auto descriptors = std::array { pollfd { .fd = _stopRead, .events = POLLIN, .revents = 0 },
                                        pollfd { .fd = _handle, .events = POLLIN, .revents = 0 } };
        if (::poll(descriptors.data(), descriptors.size(), -1) == -1)
        {
            if (errno == EINTR)
                continue;
            // Nothing can be waited on any more: as good as a gone sender.
            return WakeWait::PeerGone;
        }
        // The stop pipe is never drained: once stopped, every wait returns at once.
        if (descriptors[0].revents != 0)
            return WakeWait::Stopped;
        if (descriptors[1].revents == 0)
            continue;
        switch (drain(_handle))
        {
            case Drained::Wakes: return WakeWait::Woken;
            case Drained::EndOfStream: return WakeWait::PeerGone;
            case Drained::Nothing: break;
        }
    }
}

void WakeReceiver::stop() const noexcept
{
    auto const stop = uint8_t { 1 };
    // A full pipe is a stopped receiver already.
    while (::write(_stopWrite, &stop, sizeof(stop)) == -1 && errno == EINTR)
    {
    }
}

NativeHandle WakeReceiver::nativeHandle() const noexcept
{
    return _handle;
}

std::expected<WakePair, ChannelError> makeWakePair()
{
    auto ends = std::array<int, 2> { -1, -1 };
#ifdef SOCK_CLOEXEC
    constexpr auto SocketType = SOCK_STREAM | SOCK_CLOEXEC;
#else
    constexpr auto SocketType = SOCK_STREAM;
#endif
    if (::socketpair(AF_UNIX, SocketType, 0, ends.data()) == -1)
        return std::unexpected(ChannelError::Io);
    // Owned from here on, so that every failure below closes both.
    auto sender = WakeSender { ends[0] };
    auto const stop = makeStopPipe();
    if (!stop)
    {
        closeDescriptor(ends[1]);
        return std::unexpected(stop.error());
    }
    auto receiver = WakeReceiver { ends[1], (*stop)[0], (*stop)[1] };
    return configureSender(sender.nativeHandle())
        .and_then([&] { return configureReceiver(receiver.nativeHandle()); })
        .transform([&] { return WakePair { .parent = std::move(sender), .helper = std::move(receiver) }; });
}

} // namespace vtextension::wasm::remote::transport
