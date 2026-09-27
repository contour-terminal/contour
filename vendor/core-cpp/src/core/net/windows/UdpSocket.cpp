// SPDX-License-Identifier: Apache-2.0
#include <core/net/UdpSocket.hpp>

#include <core/net/detail/DatagramAddressing.hpp>
#include <core/net/detail/DatagramReceiveBuffer.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/platform/WinsockInit.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

    /// A blocking UDP socket over Winsock.
    class WindowsUdpSocket final: public IDatagramSocket
    {
      public:
        /// Takes ownership of an already-bound socket.
        /// @param socket The native handle.
        /// @param bound What it bound.
        /// @param receiveBuffer The receive buffer's length; see @c detail::receiveBufferFor.
        WindowsUdpSocket(SOCKET socket, DatagramAddress bound, std::size_t receiveBuffer):
            _socket { socket },
            _bound { std::move(bound) },
            // One buffer for the socket's life rather than one per receive: a receive loop that
            // allocated 64 KiB per call would spend more time in the allocator than on the wire,
            // and a buffer short enough to make that cheap is the truncation defect
            // `MaxIpv4DatagramPayload` exists to remove.
            _buffer(receiveBuffer)
        {
        }

        ~WindowsUdpSocket() override
        {
            if (_socket != detail::InvalidSocket)
                ::closesocket(_socket);
        }

        WindowsUdpSocket(WindowsUdpSocket const&) = delete;
        WindowsUdpSocket(WindowsUdpSocket&&) = delete;
        WindowsUdpSocket& operator=(WindowsUdpSocket const&) = delete;
        WindowsUdpSocket& operator=(WindowsUdpSocket&&) = delete;

        std::expected<void, NetError> send(std::span<std::byte const> payload,
                                           DatagramAddress const& to) override
        {
            // An empty host names nothing, and the two platforms disagree about that rather than
            // both refusing it: `getaddrinfo("", ...)` is `EAI_NONAME` on glibc and SUCCEEDS here,
            // resolving to the local host. Refused explicitly so a reply addressed to a sender this
            // process could not render fails the same way on both rather than going quietly to
            // loopback on this one.
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
            auto const sent = ::sendto(_socket,
                                       reinterpret_cast<char const*>(payload.data()),
                                       static_cast<int>(payload.size()),
                                       0,
                                       candidate->ai_addr,
                                       static_cast<int>(candidate->ai_addrlen));

            if (sent == SOCKET_ERROR)
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

            auto from = sockaddr_storage {};
            auto fromLength = static_cast<int>(sizeof(from));

            auto const received = ::recvfrom(_socket,
                                             reinterpret_cast<char*>(_buffer.data()),
                                             static_cast<int>(_buffer.size()),
                                             0,
                                             reinterpret_cast<sockaddr*>(&from),
                                             &fromLength);

            // Checked AFTER the receive as well as before: `close()` may have been called while
            // this call was parked, and the timeout is what let it return at all.
            if (_closed.load(std::memory_order_acquire))
                return std::unexpected(DatagramWait::Closed);

            // `WSAEMSGSIZE` is a datagram longer than the buffer: Winsock has filled the buffer and
            // dropped the rest, and it is reported rather than read as a timeout, because a loop
            // that saw only `TimedOut` would never learn that something arrived. POSIX reports the
            // same drop as `MSG_TRUNC`.
            if (received == SOCKET_ERROR)
                return std::unexpected(::WSAGetLastError() == WSAEMSGSIZE ? DatagramWait::MessageTooLarge
                                                                          : DatagramWait::TimedOut);

            auto const count = static_cast<std::size_t>(received);
            return ReceivedDatagram {
                .payload = { _buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(count) },
                .from = detail::datagramAddressOf(from, static_cast<socklen_t>(fromLength))
            };
        }

        void close() noexcept override { _closed.store(true, std::memory_order_release); }

        [[nodiscard]] DatagramAddress boundAddress() const override { return _bound; }

      private:
        /// Sets `SO_RCVTIMEO`, so a parked receive returns and the loop can stop.
        ///
        /// **Floored at a millisecond, and that is not a rounding nicety:** `SO_RCVTIMEO` of zero
        /// means *block forever*, so a caller's `0ms` would become the parked receive the whole
        /// timeout argument exists to prevent. Winsock rounds up to a system timer tick — about
        /// 15ms — which is why a caller that wants a real poll asks for one rather than for zero.
        /// @param timeout How long a receive may park.
        void applyReceiveTimeout(std::chrono::milliseconds timeout) const noexcept
        {
            auto const bounded = std::max(timeout, std::chrono::milliseconds { 1 });
            auto const millis = static_cast<DWORD>(bounded.count());
            std::ignore = ::setsockopt(
                _socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&millis), sizeof(millis));
        }

        SOCKET _socket;
        DatagramAddress _bound;
        std::vector<std::byte> _buffer;
        std::atomic<bool> _closed { false };
    };

    /// Applies the sharing policy to @p socket before it binds.
    ///
    /// **Exclusive is the default on POSIX and has to be asked for here**, which is the same
    /// asymmetry the stream side records: SO_REUSEADDR on a *later* bind takes an address a live
    /// socket already holds, and `SO_EXCLUSIVEADDRUSE` is the documented way to refuse that.
    ///
    /// It is not hypothetical just because most of these sockets ask the kernel to choose a port. A
    /// caller that binds a NAMED one goes through this branch, and a socket answering a protocol is
    /// precisely the one whose datagrams must not be handed to a second process.
    /// @param socket The unbound socket.
    /// @param sharing What the caller asked for.
    /// @return Whether the socket is still usable; false means this candidate must be abandoned.
    [[nodiscard]] bool applySharing(SOCKET socket, PortSharing sharing) noexcept
    {
        if (sharing == PortSharing::Shared)
        {
            int const reuse = 1;
            std::ignore = ::setsockopt(
                socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&reuse), sizeof(reuse));
            return true;
        }

        // Failed rather than ignored the way TCP_NODELAY is: a `setsockopt` carrying a security
        // property is not best-effort.
        int const exclusive = 1;
        return ::setsockopt(socket,
                            SOL_SOCKET,
                            SO_EXCLUSIVEADDRUSE,
                            reinterpret_cast<char const*>(&exclusive),
                            sizeof(exclusive))
               == 0;
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
    // Winsock refuses every call until WSAStartup has run, and there is no diagnostic to
    // distinguish "the stack is not up" from "this address will not bind". The TCP side already has
    // this; a second socket family reaching the network without it is how a Windows-only failure
    // with no message happens.
    platform::ensureWinsockInitialized();

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
        auto const socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == detail::InvalidSocket)
        {
            failure = lastError("socket");
            continue;
        }

        if (!applySharing(socket, sharing))
        {
            failure = lastError("setsockopt SO_EXCLUSIVEADDRUSE");
            ::closesocket(socket);
            continue;
        }

        if (broadcast == BroadcastMode::On)
        {
            int const enable = 1;
            std::ignore = ::setsockopt(
                socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char const*>(&enable), sizeof(enable));
        }

        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0)
        {
            failure = lastError("bind");
            ::closesocket(socket);
            continue;
        }

        // Read back what was actually bound rather than echoing what was asked for — see
        // `IDatagramSocket::boundAddress`. Through the same decoder the receive path uses, so there
        // is one family switch rather than two.
        auto actual = sockaddr_storage {};
        auto actualLength = static_cast<int>(sizeof(actual));
        auto bound = DatagramAddress {};
        if (::getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &actualLength) == 0)
            bound = detail::datagramAddressOf(actual, static_cast<socklen_t>(actualLength));

        return std::make_unique<WindowsUdpSocket>(
            socket, std::move(bound), receiveBuffer.value_or(detail::receiveBufferFor(candidate->ai_family)));
    }

    return std::unexpected(std::move(failure));
}

} // namespace core::net
