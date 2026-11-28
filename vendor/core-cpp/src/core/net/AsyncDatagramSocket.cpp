// SPDX-License-Identifier: Apache-2.0

/// @file
/// `openAsyncDatagramSocket` and the one `IAsyncDatagramSocket` implementation: the state machine,
/// written once over an `detail::IDatagramChannel` -- the platform's, or the in-memory network's.

#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncDatagramSocket.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/net/detail/DatagramChannel.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/OperationClaim.hpp>
#include <core/net/detail/SocketEndpoint.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace core::net
{

namespace
{
    /// The options a datagram socket has. The keep-alive rows exist too, and some kernels even
    /// accept `SO_KEEPALIVE` on a UDP socket, but a datagram has no connection to keep alive.
    constexpr auto DatagramOptions = std::array {
        SocketOption::ReceiveBufferSize,
        SocketOption::SendBufferSize,
        SocketOption::UnicastHopLimit,
    };

    /// @return The socket's own refusal of a call its state does not allow: never the OS's.
    [[nodiscard]] NetError refused(std::string what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::move(what));
    }

    /// @return A task that answers @p error without touching any socket.
    template <typename T>
    async::Task<std::expected<T, NetError>> refuse(NetError error)
    {
        co_return std::unexpected(std::move(error));
    }

    /// @return @p family's wildcard address at port 0.
    [[nodiscard]] ResolvedEndpoint wildcardOf(AddressFamily family) noexcept
    {
        if (family == AddressFamily::Ipv6)
            return toEndpoint(IpSocketAddress { .address = Ipv6Address {} });
        return toEndpoint(IpSocketAddress { .address = Ipv4Address {} });
    }

    /// Which operations a socket has in flight. Shared with the tasks it hands out, because a
    /// task may outlive its socket. Two predicates rather than one state: a receive and a send are
    /// independent, and all four combinations are legal.
    struct Outstanding
    {
        bool isReceiving = false; ///< A receive is in flight.
        bool isSending = false;   ///< A send is in flight.
    };

    /// One in-flight operation's hold on its slot in @c Outstanding.
    using Claim = detail::OperationClaim<Outstanding>;

    /// The one `IAsyncDatagramSocket`: what each state allows, over a channel that does it.
    class AsyncDatagramSocket final: public IAsyncDatagramSocket
    {
      public:
        /// @param channel The channel, unbound.
        /// @param family The family it was opened with.
        AsyncDatagramSocket(std::shared_ptr<detail::IDatagramChannel> channel, AddressFamily family):
            _channel { std::move(channel) }, _family { family }
        {
        }

        ~AsyncDatagramSocket() override { close(); }
        AsyncDatagramSocket(AsyncDatagramSocket const&) = delete;
        AsyncDatagramSocket& operator=(AsyncDatagramSocket const&) = delete;
        AsyncDatagramSocket(AsyncDatagramSocket&&) = delete;
        AsyncDatagramSocket& operator=(AsyncDatagramSocket&&) = delete;

        [[nodiscard]] std::expected<void, NetError> bind(ResolvedEndpoint const& local) override
        {
            if (_isClosed)
                return std::unexpected(refused("bind on a closed datagram socket"));
            // Refused here rather than by the kernel, whose second bind is an `EINVAL` that reads
            // like a bad address.
            if (_bound.has_value())
                return std::unexpected(refused("bind on a datagram socket that is already bound"));
            return ofFamily(local, "bind").and_then([&](IpSocketAddress const& address) {
                return _channel->bind(local).transform([&] { _bound = address; });
            });
        }

        [[nodiscard]] std::expected<void, NetError> connect(std::optional<ResolvedEndpoint> remote) override
        {
            if (_isClosed)
                return std::unexpected(refused("connect on a closed datagram socket"));
            if (!remote.has_value())
            {
                if (!_remote.has_value())
                    return std::unexpected(refused("disconnect on an unconnected datagram socket"));
                // The port, from before: Linux unbinds a socket whose port the kernel chose when it
                // disconnects (`__udp_disconnect` unhashes it unless the port was named), so a
                // socket that bound port 0 would lose the port its peers know it by.
                return _channel->endpoint(detail::SocketSide::Local)
                    .and_then([&](ResolvedEndpoint const& local) {
                        auto const port = detail::portOfSockaddr(local.storage.data(), local.length);
                        return _channel->connect(std::nullopt).and_then([&] {
                            // Disconnected from here, whether or not the port can be taken back.
                            _remote.reset();
                            return keepPort(port).and_then([&] { return keepAddress(); });
                        });
                    });
            }
            return ofFamily(*remote, "connect").and_then([&](IpSocketAddress address) {
                return bindIfUnbound().and_then([&] { return _channel->connect(remote); }).transform([&] {
                    _remote = address;
                });
            });
        }

        [[nodiscard]] async::Task<std::expected<AsyncDatagram, NetError>> receive(
            std::size_t maxBytes) override
        {
            if (_isClosed)
                return refuse<AsyncDatagram>(refused("receive on a closed datagram socket"));
            // Not left to the kernel, which parks an unbound POSIX socket for ever and fails a
            // Windows one: one answer on every platform.
            if (!_bound.has_value())
                return refuse<AsyncDatagram>(refused("receive on an unbound datagram socket"));
            if (_outstanding->isReceiving)
                return refuse<AsyncDatagram>(refused("receive while another receive is in flight"));
            // Clamped, so a caller asking for "anything" is not taken at its word by the allocator.
            return detail::holding(Claim { _outstanding, &Outstanding::isReceiving },
                                   _channel->receive(std::min(maxBytes, MaxIpv6DatagramPayload)));
        }

        [[nodiscard]] async::Task<std::expected<void, NetError>> send(
            std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) override
        {
            if (_isClosed)
                return refuse<void>(refused("send on a closed datagram socket"));
            if (_outstanding->isSending)
                return refuse<void>(refused("send while another send is in flight"));
            auto destination = destinationOf(to);
            if (!destination.has_value())
                return refuse<void>(std::move(destination.error()));
            if (auto bound = bindIfUnbound(); !bound.has_value())
                return refuse<void>(std::move(bound.error()));
            return detail::holding(Claim { _outstanding, &Outstanding::isSending },
                                   _channel->send(std::move(payload), *destination));
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> localEndpoint() const override
        {
            if (_isClosed)
                return std::unexpected(refused("localEndpoint on a closed datagram socket"));
            // Not asked of the OS: an unbound socket is the wildcard at port 0 on POSIX and an error
            // on Windows.
            if (!_bound.has_value())
                return std::unexpected(refused("localEndpoint on an unbound datagram socket"));
            return _channel->endpoint(detail::SocketSide::Local);
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> remoteEndpoint() const override
        {
            if (_isClosed)
                return std::unexpected(refused("remoteEndpoint on a closed datagram socket"));
            if (!_remote.has_value())
                return std::unexpected(refused("remoteEndpoint on an unconnected datagram socket"));
            return _channel->endpoint(detail::SocketSide::Remote);
        }

        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption option) const override
        {
            return optionAllowed(option, "option").and_then([&] { return _channel->option(option); });
        }

        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption option,
                                                              std::uint64_t value) override
        {
            return optionAllowed(option, "setOption").and_then([&] {
                return _channel->setOption(option, value);
            });
        }

        void close() noexcept override
        {
            if (std::exchange(_isClosed, true))
                return;
            _channel->close();
        }

      private:
        /// @return @p endpoint as an IP address, or why it is not one of this socket's family.
        [[nodiscard]] std::expected<IpSocketAddress, NetError> ofFamily(ResolvedEndpoint const& endpoint,
                                                                        char const* operation) const
        {
            return toIpSocketAddress(endpoint).and_then(
                [&](IpSocketAddress address) -> std::expected<IpSocketAddress, NetError> {
                    if (address.family() != _family)
                        return std::unexpected(
                            makeNetError(NetErrorCode::AddressError,
                                         0,
                                         std::string { operation }
                                             + ": the endpoint is not of the socket's address family"));
                    return address;
                });
        }

        /// Binds what was bound again where a disconnect took its port, and does nothing otherwise --
        /// on every kernel but Linux, and on Linux for a socket bound to a named port.
        ///
        /// The address is the one @c bind was given, wildcard or named, not what `getsockname`
        /// said while connected: that is the route's source address, and binding it would leave a
        /// wildcard socket hearing one interface.
        /// @param port The port the socket held before the disconnect.
        /// @return Nothing, or @c NetErrorCode::AddressInUse where another socket took the port in
        ///         between. The socket is disconnected either way.
        [[nodiscard]] std::expected<void, NetError> keepPort(std::uint16_t port)
        {
            auto const now = _channel->endpoint(detail::SocketSide::Local);
            if (now.has_value() && detail::portOfSockaddr(now->storage.data(), now->length) != 0)
                return {};
            auto again = *_bound;
            again.port = port;
            return _channel->bind(toEndpoint(again));
        }

        /// Refuses a disconnect that left the socket bound wider than it was bound: macOS and the BSDs
        /// reset a socket's local address to the wildcard when it disconnects (`udp_disconnect`), so
        /// one bound to 127.0.0.1 would go on hearing every interface. Its port it keeps, so it cannot
        /// be bound to the address again; it is closed instead, which fails closed.
        /// @return Nothing where the address is the one @c bind was given, or a wildcard was;
        ///         @c NetErrorCode::Unsupported, the socket closed, where it is wider now.
        [[nodiscard]] std::expected<void, NetError> keepAddress()
        {
            auto const isWildcard = std::visit(
                [](auto const& address) {
                    auto const isZero = [](auto part) {
                        return part == 0;
                    };
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(address)>, Ipv4Address>)
                        return std::ranges::all_of(address.octets, isZero);
                    else
                        return std::ranges::all_of(address.segments, isZero);
                },
                _bound->address);
            if (isWildcard)
                return {};
            auto const now =
                _channel->endpoint(detail::SocketSide::Local).and_then([](ResolvedEndpoint const& local) {
                    return toIpSocketAddress(local);
                });
            if (now.has_value() && now->address == _bound->address)
                return {};
            close();
            return std::unexpected(
                makeNetError(NetErrorCode::Unsupported,
                             0,
                             "the system widened the bound address at the disconnect, so the "
                             "socket is closed rather than heard on every interface"));
        }

        /// Binds the family's wildcard where nothing is bound yet, so a socket that sends or
        /// connects has a port to be answered on and a state that says so.
        [[nodiscard]] std::expected<void, NetError> bindIfUnbound()
        {
            if (_bound.has_value())
                return {};
            return bind(wildcardOf(_family));
        }

        /// Where a send goes: @p to on an unconnected socket, nothing (the kernel's remote) on a
        /// connected one. Naming the remote is allowed and sent unaddressed, because macOS refuses
        /// an address on a connected socket with `EISCONN` where Linux and Windows accept it.
        [[nodiscard]] std::expected<std::optional<ResolvedEndpoint>, NetError> destinationOf(
            std::optional<ResolvedEndpoint> to) const
        {
            if (!_remote.has_value())
            {
                if (!to.has_value())
                    return std::unexpected(
                        refused("send with no destination on an unconnected datagram socket"));
                return ofFamily(*to, "send").transform([&](IpSocketAddress const&) { return to; });
            }
            if (!to.has_value())
                return std::optional<ResolvedEndpoint> {};
            return ofFamily(*to, "send")
                .and_then([&](IpSocketAddress const& address)
                              -> std::expected<std::optional<ResolvedEndpoint>, NetError> {
                    if (address != *_remote)
                        return std::unexpected(
                            makeNetError(NetErrorCode::AddressError,
                                         0,
                                         "send: a connected datagram socket sends to its remote only"));
                    return std::optional<ResolvedEndpoint> {};
                });
        }

        /// @return Nothing when @p option may be asked of an open datagram socket.
        [[nodiscard]] std::expected<void, NetError> optionAllowed(SocketOption option,
                                                                  char const* operation) const
        {
            if (_isClosed)
                return std::unexpected(refused(std::string { operation } + " on a closed datagram socket"));
            if (std::ranges::find(DatagramOptions, option) == DatagramOptions.end())
                return std::unexpected(
                    makeNetError(NetErrorCode::Unsupported,
                                 0,
                                 std::string { operation } + ": a datagram socket has no such option"));
            return {};
        }

        std::shared_ptr<detail::IDatagramChannel> _channel;
        AddressFamily _family;
        std::optional<IpSocketAddress> _bound; ///< What @c bind was given; empty while unbound.
        std::shared_ptr<Outstanding> _outstanding = std::make_shared<Outstanding>();
        std::optional<IpSocketAddress> _remote; ///< The connected remote; empty while unconnected.
        bool _isClosed = false;
    };
} // namespace

namespace detail
{
    std::unique_ptr<IAsyncDatagramSocket> makeAsyncDatagramSocket(std::shared_ptr<IDatagramChannel> channel,
                                                                  AddressFamily family)
    {
        return std::make_unique<AsyncDatagramSocket>(std::move(channel), family);
    }
} // namespace detail

std::expected<std::unique_ptr<IAsyncDatagramSocket>, NetError> openAsyncDatagramSocket(EventLoop& loop,
                                                                                       AddressFamily family)
{
    // Before the socket exists, as a dial asks: on Windows, a loop with no completion port could
    // never complete a send or a receive.
    return detail::dialableOn(loop)
        .and_then([&loop, family] { return detail::openDatagramChannel(loop, family); })
        .transform([family](std::shared_ptr<detail::IDatagramChannel> channel) {
            return detail::makeAsyncDatagramSocket(std::move(channel), family);
        });
}

} // namespace core::net
