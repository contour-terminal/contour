// SPDX-License-Identifier: Apache-2.0
//
// The channel on POSIX: a connected pair of Unix-domain stream sockets, close-on-exec, whose writes never
// raise SIGPIPE. Shutting an end down is ::shutdown(), which wakes a reader blocked on it; the descriptor is
// closed only when the channel is destroyed, so that no thread can find its number reused.
//
// A descriptor travels as SCM_RIGHTS on a message's first byte. Only receiveSomeWithHandles() keeps what
// arrives. A plain recv() leaves the system to discard it -- but on macOS, which installs it all the same,
// receiveSome() takes it in and closes it. Contour's end of a pair refuses descriptors where Linux lets it
// (SO_PASSRIGHTS), so that none is even discarded there (UpwardDescriptors).
//
// trySend() never waits. Where the system honours MSG_DONTWAIT on a send, that is all it takes. macOS does
// not: XNU's sosendcheck() waits for room unless the socket itself is non-blocking or the caller passed
// its private MSG_NBIO, and making the descriptor non-blocking would make a receive another thread blocks
// in fail. There, a send goes out only once poll() says the socket is writable -- room for at least its
// send low-water mark -- and is no larger than that mark, which XNU then takes without waiting. Only one
// thread sends on an end at a time, so the room only grows in between.
//
// A send to a peer that shut its end down fails with Closed. macOS takes such a send and drops the bytes
// (XNU's uipc_send() never asks whether the peer still reads), so there every send first asks poll()
// whether this end's receiving side saw the peer's stream end.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <atomic>
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

#include <vtextension/wasm/remote/WarningLog.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/WouldBlock.hpp>
#include <vtextension/wasm/remote/transport/testing/ClosingReceive.hpp>

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

    /// What the system does with descriptors attached to bytes that a receive takes without asking for them.
    enum class UnclaimedDescriptors : uint8_t
    {
        Disposed,  ///< It closes them, and none is ever installed (Linux, FreeBSD).
        Installed, ///< It installs them all the same, unnamed: XNU's soreceive_ctl() externalizes SCM_RIGHTS
                   ///< whether or not the caller takes the control message, then frees the message. Every
                   ///< receive must then take them in, to close them.
    };

    /// Whether a send honours MSG_DONTWAIT.
    enum class DontWaitSend : uint8_t
    {
        Honoured, ///< It fails with EAGAIN rather than wait for room.
        Ignored,  ///< It waits for room all the same (macOS: XNU's sosendcheck() tests only the socket's
                  ///< non-blocking state and MSG_NBIO).
    };

    /// What a send to a peer that shut its end down comes to.
    enum class SendToEndedPeer : uint8_t
    {
        Fails,   ///< It fails with EPIPE (Linux, FreeBSD).
        Dropped, ///< It is taken, and the bytes dropped (macOS: XNU's uipc_send() asks only whether the
                 ///< sender may still send, and appends to the peer's receive buffer, which shutdown() set to
                 ///< drop what arrives). The send must ask first whether the peer's stream ended.
    };

#ifdef __APPLE__
    constexpr auto ThisSystemUnclaimedDescriptors = UnclaimedDescriptors::Installed;
    constexpr auto ThisSystemDontWaitSend = DontWaitSend::Ignored;
    constexpr auto ThisSystemSendToEndedPeer = SendToEndedPeer::Dropped;
#else
    constexpr auto ThisSystemUnclaimedDescriptors = UnclaimedDescriptors::Disposed;
    constexpr auto ThisSystemDontWaitSend = DontWaitSend::Honoured;
    constexpr auto ThisSystemSendToEndedPeer = SendToEndedPeer::Fails;
#endif

    /// How many descriptors a receive takes in at least: one more than a message may carry, so that a peer
    /// that attached two is told from one that attached one.
    constexpr auto MaxReceivedHandles = std::size_t { 2 };

    /// The control buffer one attached descriptor is sent in.
    using SendControlBuffer = std::array<std::byte, CMSG_SPACE(sizeof(int))>;

    /// Room for all the control data a message can carry: XNU takes at most MCLBYTES (2048) of it in, once
    /// each descriptor in it is widened to a pointer, so no message carries more than that out.
    constexpr auto WholeMessageControlBytes = std::size_t { 2048 };

    /// How many bytes of control data a receive takes in. Where the system disposes of what does not fit
    /// (Linux, FreeBSD), room for MaxReceivedHandles descriptors. Where it installs every descriptor before
    /// it truncates what does not fit (macOS), room for all a message can carry, so that each one installed
    /// is seen, and closed.
    constexpr auto ReceiveControlBytes = ThisSystemUnclaimedDescriptors == UnclaimedDescriptors::Installed
                                             ? WholeMessageControlBytes
                                             : std::size_t { CMSG_SPACE(sizeof(int) * MaxReceivedHandles) };

    /// The control buffer a receive takes descriptors into.
    using ReceiveControlBuffer = std::array<std::byte, ReceiveControlBytes>;

    /// The control buffer a receive that closes whatever came takes it into.
    using WholeMessageControlBuffer = std::array<std::byte, WholeMessageControlBytes>;

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

namespace
{
    /// Refuses a send on @p descriptor whose peer's stream ended, where the system would take it and drop it
    /// (SendToEndedPeer::Dropped). The peer's shutdown() or close ends this end's receiving side: its read
    /// filter reports end of stream, which poll() says as POLLHUP, even with bytes still to be read.
    ///
    /// It asks whether this end's receiving side ended, which says the peer's whole stream ended only because
    /// no end is ever shut one way alone: Channel::shutdown() is the only shutdown, and shuts both ways
    /// (SHUT_RDWR). A peer that shut only its sending side would make this refuse sends it still reads.
    /// @return Nothing where the send may go ahead; `Closed` where the peer's stream ended; `Io` where asking
    ///         failed.
    [[nodiscard]] std::expected<void, ChannelError> refuseSendToEndedPeer(int descriptor) noexcept
    {
        if constexpr (ThisSystemSendToEndedPeer == SendToEndedPeer::Fails)
            return {};
        else
        {
            auto ready = ::pollfd { .fd = descriptor, .events = POLLIN, .revents = 0 };
            auto polled = 0;
            do
                polled = ::poll(&ready, 1, 0);
            while (polled == -1 && errno == EINTR);
            if (polled == -1)
                return std::unexpected(ChannelError::Io);
            if ((ready.revents & POLLHUP) != 0)
                return std::unexpected(ChannelError::Closed);
            return {};
        }
    }
} // namespace

std::expected<void, ChannelError> Channel::send(std::span<uint8_t const> bytes)
{
    while (!bytes.empty())
    {
        if (auto const refused = refuseSendToEndedPeer(_native->descriptor); !refused)
            return refused;
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
    ///         `Closed` where the peer's stream ended and the system would drop the send
    ///         (refuseSendToEndedPeer()); `Io` where asking failed.
    [[nodiscard]] std::expected<std::size_t, ChannelError> sendableWithoutWaiting(
        int descriptor, std::span<uint8_t const> bytes) noexcept
    {
        if constexpr (ThisSystemDontWaitSend == DontWaitSend::Honoured)
            return refuseSendToEndedPeer(descriptor).transform([&] { return bytes.size(); });
        else
        {
            // One poll asks both: whether there is room (POLLOUT), and, where the system would drop a send to
            // a peer whose stream ended, whether it did (the receiving side's end of stream, POLLHUP).
            constexpr auto AsksForEndedPeer = ThisSystemSendToEndedPeer == SendToEndedPeer::Dropped;
            constexpr auto Events = static_cast<short>(AsksForEndedPeer ? POLLOUT | POLLIN : POLLOUT);
            auto ready = ::pollfd { .fd = descriptor, .events = Events, .revents = 0 };
            auto polled = 0;
            do
                polled = ::poll(&ready, 1, 0);
            while (polled == -1 && errno == EINTR);
            if (polled == -1)
                return std::unexpected(ChannelError::Io);
            if constexpr (AsksForEndedPeer)
                if ((ready.revents & POLLHUP) != 0)
                    return std::unexpected(ChannelError::Closed);
            // No room, and nothing for the send to report: none now. An error pending, or this end's own
            // shutdown, is what the send reports, with the errno it fails with: let it.
            if (polled == 0 || (ready.revents & (POLLOUT | POLLHUP | POLLERR)) == 0)
                return 0;
            auto lowWater = 0;
            auto length = static_cast<socklen_t>(sizeof(lowWater));
            if (::getsockopt(descriptor, SOL_SOCKET, SO_SNDLOWAT, &lowWater, &length) == -1)
                return std::unexpected(ChannelError::Io);
            return std::min(bytes.size(), static_cast<std::size_t>(std::max(lowWater, 1)));
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
        if (!sendable)
            return std::unexpected(sendable.error());
        if (*sendable == 0)
            break;
        auto const count = ::send(_native->descriptor, rest.data(), *sendable, SendFlags | MSG_DONTWAIT);
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

namespace
{
    /// What came beside the bytes of one receive.
    enum class Ancillary : uint8_t
    {
        Descriptors,  ///< Descriptors only, if anything, every one taken in close-on-exec.
        Foreign,      ///< Something other than descriptors, or descriptors the buffer had no room for.
        Unconfigured, ///< Descriptors, one of which could not be made close-on-exec.
    };

    /// What one receive took in.
    struct Receipt
    {
        std::size_t bytes = 0; ///< How many bytes; 0 at the peer's end of stream.
        Ancillary ancillary = Ancillary::Descriptors;
    };

    /// @return Whether warnOfForeignAncillaryData() told of it already, in this process.
    [[nodiscard]] std::atomic<bool>& toldForeignAncillaryData() noexcept
    {
        static auto told = std::atomic<bool> { false };
        return told;
    }

    /// Tells once, in the warning log, that a channel took in what no peer of it sends beside its bytes, and
    /// closed or dropped it.
    void warnOfForeignAncillaryData()
    {
        if (!toldForeignAncillaryData().exchange(true))
            warningLog()(
                "An extension host channel received data beside its bytes that no peer sends, or more "
                "than a message can carry; it was discarded.");
    }

    /// Receives at least one byte from @p descriptor into @p buffer, blocking until one arrives, with what
    /// came beside them.
    /// @param control Where the control data goes, aligned for a cmsghdr: its size is how much is taken in.
    /// @param arrived Where every descriptor installed is appended, owned at once, so that each is closed
    ///                however the receive ends.
    /// @return What arrived; why nothing did otherwise.
    [[nodiscard]] std::expected<Receipt, ChannelError> receiveMessage(int descriptor,
                                                                      std::span<uint8_t> buffer,
                                                                      std::span<std::byte> control,
                                                                      std::vector<OwnedHandle>& arrived)
    {
        auto iov = ::iovec { .iov_base = buffer.data(), .iov_len = buffer.size() };
        auto message = ::msghdr {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = static_cast<ControlLength>(control.size());
        auto received = ssize_t { 0 };
        do
            received = ::recvmsg(descriptor, &message, ReceiveFlags);
        while (received == -1 && errno == EINTR);
        if (received == -1)
            return std::unexpected(channelErrorOf(errno));

        auto foreign = false;
        auto unconfigured = false;
        auto const* const controlEnd = control.data() + message.msg_controllen;
        for (auto* header = CMSG_FIRSTHDR(&message); header != nullptr;
             header = CMSG_NXTHDR(&message, header))
        {
            // Defence in depth: on Linux the 2-descriptor buffer is too small for any other cmsg, which ends
            // as MSG_CTRUNC instead, so this is unreachable there.
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS
                || header->cmsg_len < CMSG_LEN(0))
            {
                foreign = true;
                continue;
            }
            // Every one the buffer holds, never past its end, whatever the header claims.
            auto const* const data = reinterpret_cast<std::byte const*>(CMSG_DATA(header));
            auto const held = static_cast<std::size_t>(std::max(controlEnd - data, std::ptrdiff_t { 0 }));
            auto const count = std::min((header->cmsg_len - CMSG_LEN(0)) / sizeof(int), held / sizeof(int));
            for (auto const index: std::views::iota(std::size_t { 0 }, count))
            {
                auto installed = -1;
                std::memcpy(&installed, data + (index * sizeof(int)), sizeof(int));
                arrived.emplace_back(installed);
#ifndef MSG_CMSG_CLOEXEC
                if (::fcntl(installed, F_SETFD, FD_CLOEXEC) == -1)
                    unconfigured = true;
#endif
            }
        }
        // Something other than descriptors, or descriptors the buffer had no room for: nothing of the message
        // can be trusted to carry what its sender meant.
        if (foreign || (message.msg_flags & MSG_CTRUNC) != 0)
            return Receipt { .bytes = static_cast<std::size_t>(received), .ancillary = Ancillary::Foreign };
        return Receipt { .bytes = static_cast<std::size_t>(received),
                         .ancillary = unconfigured ? Ancillary::Unconfigured : Ancillary::Descriptors };
    }
} // namespace

namespace
{
    /// Receives at least one byte from @p descriptor as macOS needs it (testing/ClosingReceive.hpp).
    [[nodiscard]] std::expected<std::size_t, ChannelError> receiveClosingAttachments(
        int descriptor, std::span<uint8_t> buffer)
    {
        // Whatever came beside the bytes is closed as this ends: the bytes are what was asked for. As where
        // the system discards it, the stream goes on; what no peer sends -- ancillary data other than
        // descriptors, or more than any message carries -- is told of once.
        auto discarded = std::vector<OwnedHandle> {};
        alignas(::cmsghdr) auto control = WholeMessageControlBuffer {};
        return receiveMessage(descriptor, buffer, control, discarded)
            .and_then([](Receipt receipt) -> std::expected<std::size_t, ChannelError> {
                if (receipt.ancillary == Ancillary::Foreign)
                    warnOfForeignAncillaryData();
                if (receipt.bytes == 0)
                    return std::unexpected(ChannelError::Closed);
                return receipt.bytes;
            });
    }
} // namespace

std::expected<std::size_t, ChannelError> testing::receiveSomeClosingAttachments(Channel& channel,
                                                                                std::span<uint8_t> buffer)
{
    return receiveClosingAttachments(channel.nativeHandle(), buffer);
}

void testing::forgetToldForeignAncillaryData() noexcept
{
    toldForeignAncillaryData().store(false);
}

std::expected<std::size_t, ChannelError> Channel::receiveSome(std::span<uint8_t> buffer)
{
    if constexpr (ThisSystemUnclaimedDescriptors == UnclaimedDescriptors::Installed)
        return receiveClosingAttachments(_native->descriptor, buffer);
    else
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
        if (auto const refused = refuseSendToEndedPeer(_native->descriptor); !refused)
            return refused;
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
    // Owned at once, so that every one is closed should any part of the message be refused.
    auto arrived = std::vector<OwnedHandle> {};
    alignas(::cmsghdr) auto control = ReceiveControlBuffer {};
    return receiveMessage(_native->descriptor, buffer, control, arrived)
        .and_then([&](Receipt receipt) -> std::expected<std::size_t, ChannelError> {
            switch (receipt.ancillary)
            {
                case Ancillary::Foreign: return std::unexpected(ChannelError::Refused);
                case Ancillary::Unconfigured: return std::unexpected(ChannelError::Io);
                case Ancillary::Descriptors: break;
            }
            if (receipt.bytes == 0)
                return std::unexpected(ChannelError::Closed);
            for (auto& handle: arrived)
                handles.push_back(std::move(handle));
            return receipt.bytes;
        });
}

void Channel::shutdown() noexcept
{
    // Both ways, always: the peer's refuseSendToEndedPeer() reads its receiving side ending as this end's
    // whole stream ending.
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
