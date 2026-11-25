// SPDX-License-Identifier: Apache-2.0
//
// The channel on POSIX: a connected pair of Unix-domain stream sockets, close-on-exec, whose writes never
// raise SIGPIPE. Shutting an end down is ::shutdown(), which wakes a reader blocked on it; the descriptor is
// closed only when the channel is destroyed, so that no thread can find its number reused.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

namespace
{
#ifdef MSG_NOSIGNAL
    /// Fails a write to a closed peer with EPIPE instead of raising SIGPIPE.
    constexpr auto SendFlags = MSG_NOSIGNAL;
#else
    // macOS has no MSG_NOSIGNAL; its sockets carry SO_NOSIGPIPE instead (configureEnd).
    constexpr auto SendFlags = 0;
#endif

    /// @return Why a call that set @p error failed, for the channel.
    [[nodiscard]] ChannelError channelErrorOf(int error) noexcept
    {
        switch (error)
        {
            case EPIPE:
            case ECONNRESET:
            case ENOTCONN:
            case ESHUTDOWN: return ChannelError::Closed;
            default: return ChannelError::Io;
        }
    }

    /// Whether an end is known to be close-on-exec already.
    enum class CloseOnExec : uint8_t
    {
        Unknown, ///< It may not be: set it.
        Set,     ///< socketpair() set it.
    };

    /// Makes @p descriptor close-on-exec and SIGPIPE-free where it is not yet.
    /// @return Whether it succeeded.
    [[nodiscard]] std::expected<void, ChannelError> configureEnd(int descriptor,
                                                                 CloseOnExec closeOnExec) noexcept
    {
        if (closeOnExec == CloseOnExec::Unknown && ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
            return std::unexpected(ChannelError::Io);
#ifdef SO_NOSIGPIPE
        auto const enable = 1;
        if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) == -1)
            return std::unexpected(ChannelError::Io);
#endif
        return {};
    }
} // namespace

/// One socket of the pair, closed when it is destroyed.
struct Channel::Native
{
    int descriptor = -1;

    explicit Native(int descriptor) noexcept: descriptor { descriptor } {}
    ~Native()
    {
        if (descriptor != -1)
            ::close(descriptor);
    }
    Native(Native const&) = delete;
    Native& operator=(Native const&) = delete;
    Native(Native&&) = delete;
    Native& operator=(Native&&) = delete;
};

Channel::Channel(std::unique_ptr<Native> native) noexcept: _native { std::move(native) }
{
}

std::expected<Channel, ChannelError> Channel::adopt(NativeHandle handle)
{
    struct stat status {};
    if (::fstat(handle, &status) == -1 || !S_ISSOCK(status.st_mode))
        return std::unexpected(ChannelError::Io);
    return configureEnd(handle, CloseOnExec::Unknown).transform([&] {
        return Channel { std::make_unique<Native>(handle) };
    });
}

Channel::~Channel() = default;
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;

std::expected<void, ChannelError> Channel::send(std::span<uint8_t const> bytes)
{
    while (!bytes.empty())
    {
        auto const sent = ::send(_native->descriptor, bytes.data(), bytes.size(), SendFlags);
        if (sent == -1)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(channelErrorOf(errno));
        }
        bytes = bytes.subspan(static_cast<std::size_t>(sent));
    }
    return {};
}

std::expected<std::size_t, ChannelError> Channel::receiveSome(std::span<uint8_t> buffer)
{
    while (true)
    {
        auto const received = ::recv(_native->descriptor, buffer.data(), buffer.size(), 0);
        if (received > 0)
            return static_cast<std::size_t>(received);
        if (received == 0)
            return std::unexpected(ChannelError::Closed);
        if (errno != EINTR)
            return std::unexpected(channelErrorOf(errno));
    }
}

void Channel::shutdown() noexcept
{
    if (_native)
        ::shutdown(_native->descriptor, SHUT_RDWR);
}

NativeHandle Channel::nativeHandle() const noexcept
{
    return _native ? _native->descriptor : -1;
}

std::expected<ChannelPair, ChannelError> makeChannelPair()
{
    auto descriptors = std::array<int, 2> { -1, -1 };
#ifdef SOCK_CLOEXEC
    constexpr auto SocketType = SOCK_STREAM | SOCK_CLOEXEC;
    constexpr auto PairCloseOnExec = CloseOnExec::Set;
#else
    constexpr auto SocketType = SOCK_STREAM;
    constexpr auto PairCloseOnExec = CloseOnExec::Unknown;
#endif
    if (::socketpair(AF_UNIX, SocketType, 0, descriptors.data()) == -1)
        return std::unexpected(ChannelError::Io);
    auto parent = std::make_unique<Channel::Native>(descriptors[0]);
    auto helper = std::make_unique<Channel::Native>(descriptors[1]);
    return configureEnd(parent->descriptor, PairCloseOnExec)
        .and_then([&] { return configureEnd(helper->descriptor, PairCloseOnExec); })
        .transform([&] {
            return ChannelPair { .parent = Channel { std::move(parent) },
                                 .helper = Channel { std::move(helper) } };
        });
}

} // namespace vtextension::wasm::remote::transport
