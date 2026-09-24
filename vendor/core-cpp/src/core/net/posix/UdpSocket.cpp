// SPDX-License-Identifier: Apache-2.0
#include <core/net/UdpSocket.hpp>

#include <core/net/detail/DatagramAddressing.hpp>
#include <core/net/detail/DatagramReceiveBuffer.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/posix/FdUtils.hpp>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

namespace core::net
{

namespace
{
    /// @param context What was being attempted.
    /// @return The error the last socket call reported, through the one classification table --
    ///         where `EMSGSIZE` has its own row, because it is the whole difference between "this path
    ///         cannot carry a message this big" and "something went wrong".
    [[nodiscard]] NetError lastError(std::string context)
    {
        return detail::socketError(detail::lastSocketError(), std::move(context));
    }

    /// A blocking UDP socket over a POSIX stack.
    class PosixUdpSocket final: public IDatagramSocket
    {
      public:
        /// Takes ownership of an already-bound descriptor.
        /// @param fd The descriptor.
        /// @param bound What it bound.
        /// @param receiveBuffer The receive buffer's length; see @c detail::receiveBufferFor.
        PosixUdpSocket(int fd, DatagramAddress bound, std::size_t receiveBuffer):
            _fd { fd },
            _bound { std::move(bound) },
            // One buffer for the socket's life rather than one per receive: a receive loop that
            // allocated 64 KiB per call would spend more time in the allocator than on the wire,
            // and a buffer short enough to make that cheap is the truncation defect
            // `MaxIpv4DatagramPayload` exists to remove.
            _buffer(receiveBuffer)
        {
        }

        ~PosixUdpSocket() override
        {
            if (_fd >= 0)
                ::close(_fd);
        }

        PosixUdpSocket(PosixUdpSocket const&) = delete;
        PosixUdpSocket(PosixUdpSocket&&) = delete;
        PosixUdpSocket& operator=(PosixUdpSocket const&) = delete;
        PosixUdpSocket& operator=(PosixUdpSocket&&) = delete;

        std::expected<void, NetError> send(std::span<std::byte const> payload,
                                           DatagramAddress const& to) override
        {
            // An empty host names nothing, and the two platforms disagree about that rather than
            // both refusing it: `getaddrinfo("", ...)` is `EAI_NONAME` on glibc and SUCCEEDS on
            // Winsock, resolving to the local host. Refused explicitly here so a reply addressed to
            // a sender this process could not render fails the same way on both.
            if (to.host.empty())
                return std::unexpected(makeNetError(NetErrorCode::AddressNotAvail, 0, "no host to send to"));

            auto const resolved =
                detail::resolveDatagramEndpoint(to.host, to.port, detail::EndpointUse::Send);
            if (!resolved)
                return std::unexpected(
                    makeNetError(NetErrorCode::AddressNotAvail,
                                 0,
                                 "cannot resolve " + to.host + " port " + std::to_string(to.port)));

            auto const* const candidate = resolved.get();
            auto const sent =
                ::sendto(_fd, payload.data(), payload.size(), 0, candidate->ai_addr, candidate->ai_addrlen);

            if (sent < 0)
                return std::unexpected(lastError("sendto " + to.host + " port " + std::to_string(to.port)));

            // **A short send on a datagram socket is not a partial write to retry:** the kernel
            // places a datagram whole or not at all, so anything else means the message was too
            // large for the path. A caller that read it as a partial write would resend the tail as
            // a datagram of its own, which is a second message rather than the rest of the first.
            if (static_cast<std::size_t>(sent) != payload.size())
                return std::unexpected(makeNetError(NetErrorCode::MessageTooLarge,
                                                    0,
                                                    "datagram truncated at " + std::to_string(sent) + " of "
                                                        + std::to_string(payload.size()) + " bytes"));
            return {};
        }

        std::expected<ReceivedDatagram, DatagramWait> receive(std::chrono::milliseconds timeout) override
        {
            if (_closed.load(std::memory_order_acquire))
                return std::unexpected(DatagramWait::Closed);

            applyReceiveTimeout(timeout);

            // `recvmsg` rather than `recvfrom`, for the one thing only it reports: `MSG_TRUNC` in
            // the returned flags, which is the difference between a datagram that fit and one the
            // kernel cut to the buffer. `recvfrom` returns the same count for both.
            auto from = sockaddr_storage {};
            auto vector = iovec {};
            vector.iov_base = _buffer.data();
            vector.iov_len = _buffer.size();
            auto message = msghdr {};
            message.msg_name = &from;
            message.msg_namelen = static_cast<socklen_t>(sizeof(from));
            message.msg_iov = &vector;
            message.msg_iovlen = 1;

            auto const received = ::recvmsg(_fd, &message, 0);

            // Checked AFTER the receive as well as before: `close()` may have been called while
            // this call was parked, and the timeout is what let it return at all.
            if (_closed.load(std::memory_order_acquire))
                return std::unexpected(DatagramWait::Closed);

            if (received < 0)
                return std::unexpected(DatagramWait::TimedOut);

            // The rest of the datagram is already gone; what is left is the part that fit, and
            // handing that back as the message is the corruption the buffer's length exists to rule
            // out. Winsock drops it the same way and says `WSAEMSGSIZE`.
            if ((message.msg_flags & MSG_TRUNC) != 0)
                return std::unexpected(DatagramWait::MessageTooLarge);

            auto const count = static_cast<std::size_t>(received);
            return ReceivedDatagram { .payload = { _buffer.begin(),
                                                   _buffer.begin() + static_cast<std::ptrdiff_t>(count) },
                                      .from = detail::datagramAddressOf(from, message.msg_namelen) };
        }

        void close() noexcept override { _closed.store(true, std::memory_order_release); }

        [[nodiscard]] DatagramAddress boundAddress() const override { return _bound; }

      private:
        /// Sets `SO_RCVTIMEO`, so a parked receive returns and the loop can stop.
        ///
        /// **Floored at a millisecond, and that is not a rounding nicety:** `SO_RCVTIMEO` of zero
        /// means *block forever*, so a caller's `0ms` would become the parked receive the whole
        /// timeout argument exists to prevent.
        /// @param timeout How long a receive may park.
        void applyReceiveTimeout(std::chrono::milliseconds timeout) const noexcept
        {
            auto const bounded = std::max(timeout, std::chrono::milliseconds { 1 });
            auto value = timeval {};
            value.tv_sec = static_cast<decltype(value.tv_sec)>(bounded.count() / 1000);
            value.tv_usec = static_cast<decltype(value.tv_usec)>((bounded.count() % 1000) * 1000);
            std::ignore = ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
        }

        int _fd;
        DatagramAddress _bound;
        std::vector<std::byte> _buffer;
        std::atomic<bool> _closed { false };
    };

    /// Applies the sharing policy to @p fd before it binds.
    /// @param fd The unbound descriptor.
    /// @param sharing What the caller asked for.
    void applySharing(int fd, PortSharing sharing) noexcept
    {
        if (sharing != PortSharing::Shared)
            return;

        int const reuse = 1;
        std::ignore = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // **One intent, two spellings, because SO_REUSEADDR does not mean the same thing
        // everywhere.** On Linux it is what lets a second socket bind an address a first one holds.
        // On BSD and Darwin it permits that only for a MULTICAST address; a second bind of the same
        // unicast address needs SO_REUSEPORT, and without it the second node on a macOS host cannot
        // bind the beacon port at all.
        //
        // Set wherever the option exists rather than behind a platform test, and that is measured
        // rather than assumed: on Ubuntu 24.04 all four combinations of the two options across two
        // sockets bind successfully, so a process carrying this still shares a port with one that
        // does not.
#ifdef SO_REUSEPORT
        std::ignore = ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    }
} // namespace

std::expected<std::unique_ptr<IDatagramSocket>, NetError> openUdpSocket(std::string_view bindAddress,
                                                                        std::uint16_t port,
                                                                        BroadcastMode broadcast,
                                                                        PortSharing sharing)
{
    return detail::openUdpSocketWithReceiveBuffer(bindAddress, port, broadcast, sharing, std::nullopt);
}

std::expected<std::unique_ptr<IDatagramSocket>, NetError> detail::openUdpSocketWithReceiveBuffer(
    std::string_view bindAddress,
    std::uint16_t port,
    BroadcastMode broadcast,
    PortSharing sharing,
    std::optional<std::size_t> receiveBuffer)
{
    auto const resolved =
        detail::resolveDatagramEndpoint(std::string { bindAddress }, port, detail::EndpointUse::Bind);
    if (!resolved)
        return std::unexpected(makeNetError(
            NetErrorCode::AddressError, 0, "cannot resolve bind address " + std::string { bindAddress }));

    // The last candidate's failure, not the first: a host that resolves to both an IPv6 and an IPv4
    // address is tried in that order, and reporting the v6 attempt would name a family the caller
    // never asked about.
    auto failure = makeNetError(NetErrorCode::AddressNotAvail, 0, "no candidate address could be bound");

    // A while that steps to the next candidate FIRST, so every `continue` below moves on.
    auto const* next = resolved.get();
    while (next != nullptr)
    {
        auto const* const candidate = std::exchange(next, next->ai_next);
        // makeDatagramSocket rather than a bare ::socket: close-on-exec is armed where every
        // socket passes, and a child this process spawns must not keep a bound port alive.
        auto const fd = makeDatagramSocket(candidate->ai_family, candidate->ai_protocol);
        if (fd < 0)
        {
            failure = lastError("socket");
            continue;
        }

        applySharing(fd, sharing);

        if (broadcast == BroadcastMode::On)
        {
            int const enable = 1;
            std::ignore = ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
        }

        if (::bind(fd, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)) != 0)
        {
            failure = lastError("bind");
            ::close(fd);
            continue;
        }

        // Read back what was actually bound rather than echoing what was asked for — see
        // `IDatagramSocket::boundAddress`. Through the same decoder the receive path uses, so there
        // is one family switch rather than two.
        auto actual = sockaddr_storage {};
        auto actualLength = static_cast<socklen_t>(sizeof(actual));
        auto bound = DatagramAddress {};
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actualLength) == 0)
            bound = detail::datagramAddressOf(actual, actualLength);

        return std::make_unique<PosixUdpSocket>(
            fd, std::move(bound), receiveBuffer.value_or(detail::receiveBufferFor(candidate->ai_family)));
    }

    return std::unexpected(std::move(failure));
}

} // namespace core::net
