// SPDX-License-Identifier: Apache-2.0
//
// The channel on POSIX: a connected pair of Unix-domain stream sockets, close-on-exec, whose writes never
// raise SIGPIPE. Shutting an end down is ::shutdown(), which wakes a reader blocked on it; the descriptor is
// closed only when the channel is destroyed, so that no thread can find its number reused.
//
// A descriptor travels as SCM_RIGHTS on a message's first byte. Only receiveSomeWithHandles() installs what
// arrives; a plain recv() leaves the system to discard it. Contour's end of a pair refuses descriptors where
// Linux lets it (SO_PASSRIGHTS), so that none is even discarded there (UpwardDescriptors).
//
// trySend() never waits. Where the system honours MSG_DONTWAIT on a send, that is all it takes. macOS does
// not: XNU's sosendcheck() waits for room unless the socket itself is non-blocking or the caller passed
// its private MSG_NBIO, and making the descriptor non-blocking would make a receive another thread blocks
// in fail. There, a send goes out only once poll() says the socket is writable -- room for at least its
// send low-water mark -- and is no larger than that mark, which XNU then takes without waiting. Only one
// thread sends on an end at a time, so the room only grows in between.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/WouldBlock.hpp>

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

#ifdef __linux__
    #ifdef SO_PASSRIGHTS
    constexpr auto PassRightsOption = SO_PASSRIGHTS;
    #else
    /// `SO_PASSRIGHTS` (Linux 6.16, asm-generic/socket.h), for C libraries whose headers predate it.
    constexpr auto PassRightsOption = 83;
    #endif
#endif

    /// Makes Contour's end @p descriptor refuse descriptors its peer sends, where the system can: a
    /// descriptor the system took in only to discard it would be closed in the receiving thread, and a last
    /// close can block without bound (UpwardDescriptors).
    /// @return What becomes of descriptors sent to it; `Io` if the system failed otherwise than by lacking
    /// it.
    [[nodiscard]] std::expected<UpwardDescriptors, ChannelError> refuseDescriptors(
        [[maybe_unused]] int descriptor) noexcept
    {
#ifdef __linux__
        auto const disable = 0;
        if (::setsockopt(descriptor, SOL_SOCKET, PassRightsOption, &disable, sizeof(disable)) == 0)
            return UpwardDescriptors::Refused;
        return upwardDescriptorsWhenUnrefused(errno);
#else
        return UpwardDescriptors::Discarded;
#endif
    }

    /// The types of msghdr::msg_controllen and cmsghdr::cmsg_len: size_t on Linux, socklen_t on macOS.
    using ControlLength = decltype(::msghdr {}.msg_controllen);
    using ControlMessageLength = decltype(::cmsghdr {}.cmsg_len);

    /// Whether a send honours MSG_DONTWAIT.
    enum class DontWaitSend : uint8_t
    {
        Honoured, ///< It fails with EAGAIN rather than wait for room.
        Ignored,  ///< It waits for room all the same (macOS: XNU's sosendcheck() tests only the socket's
                  ///< non-blocking state and MSG_NBIO).
    };

#ifdef __APPLE__
    constexpr auto ThisSystemDontWaitSend = DontWaitSend::Ignored;
#else
    constexpr auto ThisSystemDontWaitSend = DontWaitSend::Honoured;
#endif

    /// The most descriptors one receive takes in: one more than a message may carry, so that a peer that
    /// attached two is told from one that attached one.
    constexpr auto MaxReceivedHandles = std::size_t { 2 };

    /// The control buffer one attached descriptor is sent in.
    using SendControlBuffer = std::array<std::byte, CMSG_SPACE(sizeof(int))>;

    /// The control buffer a receive takes descriptors into.
    using ReceiveControlBuffer = std::array<std::byte, CMSG_SPACE(sizeof(int) * MaxReceivedHandles)>;

#ifdef MSG_CMSG_CLOEXEC
    /// Installs received descriptors close-on-exec, atomically.
    constexpr auto ReceiveFlags = MSG_CMSG_CLOEXEC;
#else
    // macOS has no MSG_CMSG_CLOEXEC: each descriptor is made close-on-exec right after recvmsg().
    constexpr auto ReceiveFlags = 0;
#endif
} // namespace

namespace
{
    /// An errno setting SO_PASSRIGHTS may fail with that means the system cannot refuse descriptors here.
    struct UnrefusableRow
    {
        int error;
        std::string_view why;
    };

    constexpr auto UnrefusableRows = std::array {
        UnrefusableRow { .error = ENOPROTOOPT, .why = "a kernel older than 6.16 does not know the option" },
        UnrefusableRow { .error = EINVAL, .why = "nor do some of those that backport parts of it" },
        UnrefusableRow { .error = EPERM, .why = "an outer sandbox's filter (Flatpak) refuses the call" },
        UnrefusableRow { .error = EACCES, .why = "a security module refuses it" },
    };
} // namespace

std::expected<UpwardDescriptors, ChannelError> upwardDescriptorsWhenUnrefused(int error) noexcept
{
    if (std::ranges::contains(UnrefusableRows, error, &UnrefusableRow::error))
        return UpwardDescriptors::Discarded;
    return std::unexpected(ChannelError::Io);
}

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

namespace
{
    /// @return How many bytes of @p bytes, from the front, one send on @p descriptor may be given without
    ///         waiting for room: all of them where the send honours MSG_DONTWAIT; elsewhere at most the
    ///         socket's send low-water mark, once poll() says it is writable, and none (0) while it is not.
    ///         -1, with errno set, where asking failed.
    [[nodiscard]] ssize_t sendableWithoutWaiting(int descriptor, std::span<uint8_t const> bytes) noexcept
    {
        if constexpr (ThisSystemDontWaitSend == DontWaitSend::Honoured)
            return static_cast<ssize_t>(bytes.size());
        else
        {
            auto ready = ::pollfd { .fd = descriptor, .events = POLLOUT, .revents = 0 };
            auto const polled = ::poll(&ready, 1, 0);
            if (polled == -1)
                return -1;
            // A peer that is gone, or an error pending, is what the send reports: let it.
            if (polled == 0 || (ready.revents & (POLLOUT | POLLHUP | POLLERR)) == 0)
                return 0;
            auto lowWater = 0;
            auto length = static_cast<socklen_t>(sizeof(lowWater));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_SNDLOWAT, &lowWater, &length) == -1)
                return -1;
            return static_cast<ssize_t>(
                std::min(bytes.size(), static_cast<std::size_t>(std::max(lowWater, 1))));
        }
    }
} // namespace

std::expected<std::size_t, ChannelError> Channel::trySend(std::span<uint8_t const> bytes)
{
    auto sent = std::size_t { 0 };
    while (sent < bytes.size())
    {
        auto const rest = bytes.subspan(sent);
        auto const sendable = sendableWithoutWaiting(_native->descriptor, rest);
        if (sendable == -1)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(channelErrorOf(errno));
        }
        if (sendable == 0)
            break;
        auto const count = ::send(
            _native->descriptor, rest.data(), static_cast<std::size_t>(sendable), SendFlags | MSG_DONTWAIT);
        if (count == -1)
        {
            if (errno == EINTR)
                continue;
            if (wouldBlock(errno))
                break;
            return std::unexpected(channelErrorOf(errno));
        }
        sent += static_cast<std::size_t>(count);
    }
    return sent;
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

std::expected<void, ChannelError> Channel::sendWithHandle(std::span<uint8_t const> bytes, NativeHandle handle)
{
    if (bytes.empty())
        return std::unexpected(ChannelError::Io);
    auto iov = ::iovec { .iov_base = const_cast<uint8_t*>(bytes.data()), .iov_len = bytes.size() };
    alignas(::cmsghdr) auto control = SendControlBuffer {};
    auto message = ::msghdr {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = static_cast<ControlLength>(control.size());
    auto* const header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = static_cast<ControlMessageLength>(CMSG_LEN(sizeof(int)));
    std::memcpy(CMSG_DATA(header), &handle, sizeof(int));
    while (true)
    {
        auto const sent = ::sendmsg(_native->descriptor, &message, SendFlags);
        if (sent == -1)
        {
            if (errno == EINTR)
                continue;
            // The peer's end refuses descriptors (SO_PASSRIGHTS off): nothing was sent.
            if (errno == EPERM)
                return std::unexpected(ChannelError::Refused);
            return std::unexpected(channelErrorOf(errno));
        }
        // The descriptor went with the first byte; the rest are plain bytes.
        return send(bytes.subspan(static_cast<std::size_t>(sent)));
    }
}

std::expected<std::size_t, ChannelError> Channel::receiveSomeWithHandles(std::span<uint8_t> buffer,
                                                                         std::vector<OwnedHandle>& handles)
{
    auto iov = ::iovec { .iov_base = buffer.data(), .iov_len = buffer.size() };
    alignas(::cmsghdr) auto control = ReceiveControlBuffer {};
    auto message = ::msghdr {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = static_cast<ControlLength>(control.size());
    auto received = ssize_t { 0 };
    do
        received = ::recvmsg(_native->descriptor, &message, ReceiveFlags);
    while (received == -1 && errno == EINTR);
    if (received == -1)
        return std::unexpected(channelErrorOf(errno));

    // Owned at once, so that every one is closed should any part of the message be refused.
    auto arrived = std::vector<OwnedHandle> {};
    auto foreign = false;
    auto unconfigured = false;
    for (auto* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
    {
        // Defence in depth: on Linux the 2-descriptor buffer is too small for any other cmsg, which ends as
        // MSG_CTRUNC instead, so this is unreachable there.
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS
            || header->cmsg_len < CMSG_LEN(0))
        {
            foreign = true;
            continue;
        }
        auto const count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (auto const index: std::views::iota(std::size_t { 0 }, std::min(count, MaxReceivedHandles)))
        {
            auto descriptor = -1;
            std::memcpy(&descriptor, CMSG_DATA(header) + (index * sizeof(int)), sizeof(int));
            arrived.emplace_back(descriptor);
#ifndef MSG_CMSG_CLOEXEC
            if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
                unconfigured = true;
#endif
        }
    }
    // Something other than descriptors, or descriptors the buffer had no room for: nothing of the message
    // can be trusted to carry what its sender meant.
    if (foreign || (message.msg_flags & MSG_CTRUNC) != 0)
        return std::unexpected(ChannelError::Refused);
    if (unconfigured)
        return std::unexpected(ChannelError::Io);
    if (received == 0)
        return std::unexpected(ChannelError::Closed);
    for (auto& handle: arrived)
        handles.push_back(std::move(handle));
    return static_cast<std::size_t>(received);
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
        .and_then([&] { return refuseDescriptors(parent->descriptor); })
        .transform([&](UpwardDescriptors upward) {
            return ChannelPair { .parent = Channel { std::move(parent) },
                                 .helper = Channel { std::move(helper) },
                                 .upward = upward };
        });
}

void closeNativeHandle(NativeHandle handle) noexcept
{
    ::close(handle);
}

} // namespace vtextension::wasm::remote::transport
