// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/DescriptorSocket.hpp>

#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstring>
#include <ranges>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>

namespace vthost::keeper
{

namespace
{
    /// The most byte runs one send gathers.
    constexpr auto MaxGather = std::size_t { 64 };

    /// The type of msghdr::msg_iovlen: size_t on Linux, int on macOS.
    using IovecCount = decltype(::msghdr {}.msg_iovlen);
    static_assert(std::in_range<IovecCount>(MaxGather), "a gathered send's run count must fit msg_iovlen");

    /// The types of msghdr::msg_controllen and cmsghdr::cmsg_len: size_t on Linux, socklen_t on macOS.
    using ControlLength = decltype(::msghdr {}.msg_controllen);
    using ControlMessageLength = decltype(::cmsghdr {}.cmsg_len);

    /// The control buffer one frame's descriptors fit in.
    using ControlBuffer = std::array<char, CMSG_SPACE(sizeof(int) * MaxFdsPerFrame)>;

    /// The most descriptors one receive takes in: more than a frame may carry, so that a list over
    /// MaxFdsPerFrame is still received whole, and closed. macOS installs every descriptor a message carries,
    /// those a control buffer has no room for as well, and leaks them; only a list beyond this one can.
    constexpr auto MaxFdsPerReceive = std::size_t { 8 * MaxFdsPerFrame };

    /// The control buffer a receive takes descriptors into.
    using ReceiveControlBuffer = std::array<char, CMSG_SPACE(sizeof(int) * MaxFdsPerReceive)>;

    /// Hands every control message of @p message to @p visit, in order.
    /// @param message A received message.
    /// @param visit Called with each control message.
    void forEachControlMessage(::msghdr& message, std::invocable<::cmsghdr&> auto&& visit)
    {
        auto* header = CMSG_FIRSTHDR(&message);
        while (header != nullptr)
        {
            visit(*header);
            header = CMSG_NXTHDR(&message, header);
        }
    }
} // namespace

std::expected<std::size_t, SendError> sendWithDescriptors(int socket,
                                                          std::span<std::span<uint8_t const> const> runs,
                                                          std::span<int const> fds)
{
    if (fds.size() > MaxFdsPerFrame)
        return std::unexpected { SendError::Broken };
    auto iov = std::array<::iovec, MaxGather> {};
    auto const count = std::min(runs.size(), MaxGather);
    for (auto&& [slot, run]: std::views::zip(iov, runs | std::views::take(count)))
        slot = ::iovec { .iov_base = const_cast<uint8_t*>(run.data()), .iov_len = run.size() };
    alignas(::cmsghdr) auto control = ControlBuffer {};
    auto message = ::msghdr {};
    message.msg_iov = iov.data();
    message.msg_iovlen = static_cast<IovecCount>(count); // count <= MaxGather, which fits
    if (!fds.empty())
    {
        message.msg_control = control.data();
        message.msg_controllen = static_cast<ControlLength>(CMSG_SPACE(sizeof(int) * fds.size()));
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = static_cast<ControlMessageLength>(CMSG_LEN(sizeof(int) * fds.size()));
        std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
    }
#ifdef MSG_NOSIGNAL
    constexpr auto Flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
    constexpr auto Flags = MSG_DONTWAIT; // SO_NOSIGPIPE, set on the socket, keeps SIGPIPE away.
#endif
    while (true)
    {
        auto const sent = ::sendmsg(socket, &message, Flags);
        if (sent >= 0)
            return static_cast<std::size_t>(sent);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::size_t { 0 };
        if (errno == EPIPE || errno == ECONNRESET)
            return std::unexpected { SendError::PeerGone };
        return std::unexpected { SendError::Broken };
    }
}

std::expected<Received, ReceiveError> receiveWithDescriptors(int socket, std::span<uint8_t> buffer)
{
#ifdef MSG_CMSG_CLOEXEC
    constexpr auto Flags = MSG_CMSG_CLOEXEC | MSG_DONTWAIT;
#else
    constexpr auto Flags = MSG_DONTWAIT;
#endif
    auto iov = ::iovec { .iov_base = buffer.data(), .iov_len = buffer.size() };
    alignas(::cmsghdr) auto control = ReceiveControlBuffer {};
    auto message = ::msghdr {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = static_cast<ControlLength>(control.size());
    auto got = ssize_t { 0 };
    do
        got = ::recvmsg(socket, &message, Flags);
    while (got < 0 && errno == EINTR);
    if (got < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return std::unexpected { ReceiveError::WouldBlock };
        return std::unexpected { errno == ECONNRESET ? ReceiveError::PeerGone : ReceiveError::Broken };
    }

    // On MSG_CTRUNC the kernel reports the full length although only part landed: clamp to what fits.
    auto received = Received { .bytes = static_cast<std::size_t>(got), .fds = {} };
    auto const capacity = (control.size() - CMSG_LEN(0)) / sizeof(int);
    forEachControlMessage(message, [&](::cmsghdr& header) {
        if (header.cmsg_level != SOL_SOCKET || header.cmsg_type != SCM_RIGHTS)
            return;
        auto const count = std::min((header.cmsg_len - CMSG_LEN(0)) / sizeof(int), capacity);
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
        {
            auto fd = -1;
            std::memcpy(&fd, CMSG_DATA(&header) + (index * sizeof(int)), sizeof(int));
#ifndef MSG_CMSG_CLOEXEC
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
            received.fds.push_back(fd);
        }
    });
    if ((message.msg_flags & MSG_CTRUNC) != 0 || received.fds.size() > MaxFdsPerFrame)
    {
        // Some descriptors were lost, or more came than a frame carries: no frame can be trusted to have its
        // own.
        closeAll(received.fds);
        return std::unexpected { ReceiveError::Truncated };
    }
    return received;
}

} // namespace vthost::keeper
