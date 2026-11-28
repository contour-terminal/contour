// SPDX-License-Identifier: Apache-2.0

/// @file
/// `InMemoryAsyncDatagramNetwork`: an in-memory `detail::IDatagramChannel` per socket, behind the
/// real socket's state machine.

#include <core/net/testing/InMemoryAsyncDatagram.hpp>

#include <core/async/AsyncQueue.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/net/detail/DatagramChannel.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace core::net::testing
{

namespace
{
    class InMemoryChannel;

    /// The first port a bind to port 0 is given, and the range it cycles through: IANA's dynamic
    /// ports.
    constexpr std::uint16_t FirstEphemeralPort = 49152;

    /// @return Whether @p address is its family's wildcard.
    [[nodiscard]] bool isWildcard(IpSocketAddress const& address) noexcept
    {
        return std::visit([](auto const& host) { return host == std::remove_cvref_t<decltype(host)> {}; },
                          address.address);
    }

    /// @return The largest payload one datagram of @p family carries.
    [[nodiscard]] std::size_t largestPayload(AddressFamily family) noexcept
    {
        return family == AddressFamily::Ipv6 ? MaxIpv6DatagramPayload : MaxIpv4DatagramPayload;
    }

    /// @return @p endpoint as an IP address; one the socket has already checked always is.
    [[nodiscard]] IpSocketAddress ipOf(ResolvedEndpoint const& endpoint)
    {
        return toIpSocketAddress(endpoint).value_or(IpSocketAddress { .address = Ipv4Address {} });
    }
} // namespace

/// Which sockets hold which address. Every field is guarded by @c mutex, and so is every bound
/// and connected address a channel holds, because a sender on another thread reads them.
struct InMemoryAsyncDatagramNetwork::State
{
    /// One bound socket.
    struct Holder
    {
        InMemoryChannel* key = nullptr;       ///< Who, for the socket's own delisting.
        std::weak_ptr<InMemoryChannel> owner; ///< The channel, while anything holds it.
    };

    std::mutex mutex;
    std::size_t inboxCapacity = DefaultInboxCapacity;
    std::uint16_t nextPort = FirstEphemeralPort;
    std::vector<Holder> bound;
};

namespace
{
    using State = InMemoryAsyncDatagramNetwork::State;

    /// One socket on the network: a queue of datagrams that have reached it, and its addresses.
    class InMemoryChannel final: public detail::IDatagramChannel
    {
      public:
        /// @param network The network it lives on.
        /// @param loop The loop a parked receive resumes on.
        /// @param family Its family.
        InMemoryChannel(std::shared_ptr<State> network, EventLoop& loop, AddressFamily family):
            _network { std::move(network) },
            _family { family },
            _inbox { loop,
                     async::AsyncQueueOptions { .capacity = _network->inboxCapacity,
                                                .overflow = async::AsyncQueueOverflow::DropNewest } }
        {
        }

        ~InMemoryChannel() override { close(); }
        InMemoryChannel(InMemoryChannel const&) = delete;
        InMemoryChannel& operator=(InMemoryChannel const&) = delete;
        InMemoryChannel(InMemoryChannel&&) = delete;
        InMemoryChannel& operator=(InMemoryChannel&&) = delete;

        [[nodiscard]] std::expected<void, NetError> bind(ResolvedEndpoint const& local) override
        {
            auto address = ipOf(local);
            auto const lock = std::scoped_lock { _network->mutex };
            if (address.port == 0)
            {
                auto const port = freePort(address);
                if (!port.has_value())
                    return std::unexpected(
                        makeNetError(NetErrorCode::AddressInUse, 0, "bind: no ephemeral port is free"));
                address.port = *port;
            }
            else if (!isFree(address))
                return std::unexpected(
                    makeNetError(NetErrorCode::AddressInUse, 0, "bind: the address is taken"));
            _local = address;
            _network->bound.push_back(State::Holder {
                .key = this, .owner = std::static_pointer_cast<InMemoryChannel>(shared_from_this()) });
            return {};
        }

        [[nodiscard]] std::expected<void, NetError> connect(
            std::optional<ResolvedEndpoint> const& remote) override
        {
            auto const lock = std::scoped_lock { _network->mutex };
            _remote = remote.transform(ipOf);
            return {};
        }

        [[nodiscard]] std::expected<ResolvedEndpoint, NetError> endpoint(
            detail::SocketSide side) const override
        {
            auto const lock = std::scoped_lock { _network->mutex };
            auto const& address = side == detail::SocketSide::Local ? _local : _remote;
            if (!address.has_value())
                return std::unexpected(makeNetError(NetErrorCode::BadHandle, 0, "the endpoint is not set"));
            return toEndpoint(*address);
        }

        [[nodiscard]] std::expected<std::uint64_t, NetError> option(SocketOption) const override
        {
            return std::unexpected(unmodelled());
        }

        [[nodiscard]] std::expected<void, NetError> setOption(SocketOption, std::uint64_t) override
        {
            return std::unexpected(unmodelled());
        }

        [[nodiscard]] async::Task<std::expected<AsyncDatagram, NetError>> receive(
            std::size_t maxBytes) override
        {
            return receiveOn(std::static_pointer_cast<InMemoryChannel>(shared_from_this()), maxBytes);
        }

        [[nodiscard]] async::Task<std::expected<void, NetError>> send(
            std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) override
        {
            return sendOn(
                std::static_pointer_cast<InMemoryChannel>(shared_from_this()), std::move(payload), to);
        }

        void close() noexcept override
        {
            _inbox.close();
            auto const lock = std::scoped_lock { _network->mutex };
            std::erase_if(_network->bound,
                          [this](State::Holder const& holder) { return holder.key == this; });
        }

      private:
        /// @return The refusal of an option, which this double does not model.
        [[nodiscard]] static NetError unmodelled()
        {
            return makeNetError(
                NetErrorCode::Unsupported, 0, "InMemoryAsyncDatagramNetwork does not model socket options");
        }

        /// @return Whether a socket may bind @p address. The network's mutex is held.
        [[nodiscard]] bool isFree(IpSocketAddress const& address) const
        {
            return std::ranges::none_of(_network->bound, [&](State::Holder const& holder) {
                auto const other = holder.owner.lock();
                if (other == nullptr || !other->_local.has_value())
                    return false;
                auto const& held = *other->_local;
                return held.family() == address.family() && held.port == address.port
                       && (held.address == address.address || isWildcard(held) || isWildcard(address));
            });
        }

        /// @return A port @p address is free at, or nothing when every dynamic port is taken. The
        ///         network's mutex is held.
        [[nodiscard]] std::optional<std::uint16_t> freePort(IpSocketAddress address) const
        {
            constexpr auto EphemeralPortCount = 65536U - FirstEphemeralPort;
            for ([[maybe_unused]] auto const attempt: std::views::iota(0U, EphemeralPortCount))
            {
                address.port = _network->nextPort;
                _network->nextPort = _network->nextPort == 65535
                                         ? FirstEphemeralPort
                                         : static_cast<std::uint16_t>(_network->nextPort + 1);
                if (isFree(address))
                    return address.port;
            }
            return std::nullopt;
        }

        /// @return The socket @p destination reaches: the one bound to it exactly, else the one bound
        ///         to the wildcard at its port. The network's mutex is held.
        [[nodiscard]] std::shared_ptr<InMemoryChannel> receiverOf(IpSocketAddress const& destination) const
        {
            auto wildcard = std::shared_ptr<InMemoryChannel> {};
            for (auto const& holder: _network->bound)
            {
                auto other = holder.owner.lock();
                if (other == nullptr || !other->_local.has_value())
                    continue;
                auto const& held = *other->_local;
                if (held.family() != destination.family() || held.port != destination.port)
                    continue;
                if (held.address == destination.address)
                    return other;
                if (isWildcard(held))
                    wildcard = std::move(other);
            }
            return wildcard;
        }

        static async::Task<std::expected<AsyncDatagram, NetError>> receiveOn(
            std::shared_ptr<InMemoryChannel> self, std::size_t maxBytes)
        {
            auto datagram = co_await self->_inbox.pop();
            if (!datagram.has_value())
                co_return std::unexpected(detail::closedUnder("receive"));
            if (datagram->payload.size() > maxBytes)
                co_return std::unexpected(makeNetError(
                    NetErrorCode::MessageTooLarge, 0, "receive: the datagram is longer than maxBytes"));
            co_return std::move(*datagram);
        }

        static async::Task<std::expected<void, NetError>> sendOn(std::shared_ptr<InMemoryChannel> self,
                                                                 std::vector<std::byte> payload,
                                                                 std::optional<ResolvedEndpoint> to)
        {
            if (payload.size() > largestPayload(self->_family))
                co_return std::unexpected(
                    makeNetError(NetErrorCode::MessageTooLarge,
                                 0,
                                 "send: the payload is longer than the family's largest datagram"));

            auto receiver = std::shared_ptr<InMemoryChannel> {};
            auto from = IpSocketAddress { .address = Ipv4Address {} };
            {
                auto const lock = std::scoped_lock { self->_network->mutex };
                if (!self->_local.has_value())
                    co_return std::unexpected(detail::closedUnder("send"));
                auto const destination = to.has_value() ? ipOf(*to) : self->_remote.value_or(*self->_local);
                from = *self->_local;
                // What a loopback route picks for a sender bound to the wildcard.
                if (isWildcard(from))
                    from.address = destination.address;
                receiver = self->receiverOf(destination);
                // A connected receiver hears its remote alone.
                if (receiver != nullptr && receiver->_remote.has_value() && *receiver->_remote != from)
                    receiver.reset();
            }
            // Outside the network's lock, for `AsyncQueue::push`'s reason: it posts the woken
            // receiver to its executor, whose own lock must never nest inside a producer's.
            if (receiver != nullptr)
                std::ignore = receiver->_inbox.push(
                    AsyncDatagram { .payload = std::move(payload), .from = toEndpoint(from) });
            co_return {};
        }

        std::shared_ptr<State> _network;
        AddressFamily _family;
        std::optional<IpSocketAddress> _local;  ///< Guarded by the network's mutex.
        std::optional<IpSocketAddress> _remote; ///< Guarded by the network's mutex.
        async::AsyncQueue<AsyncDatagram> _inbox;
    };
} // namespace

InMemoryAsyncDatagramNetwork::InMemoryAsyncDatagramNetwork(std::size_t inboxCapacity):
    _state { std::make_shared<State>() }
{
    _state->inboxCapacity = inboxCapacity;
}

InMemoryAsyncDatagramNetwork::~InMemoryAsyncDatagramNetwork() = default;

std::expected<std::unique_ptr<IAsyncDatagramSocket>, NetError> InMemoryAsyncDatagramNetwork::open(
    EventLoop& loop, AddressFamily family)
{
    return detail::makeAsyncDatagramSocket(std::make_shared<InMemoryChannel>(_state, loop, family), family);
}

} // namespace core::net::testing
