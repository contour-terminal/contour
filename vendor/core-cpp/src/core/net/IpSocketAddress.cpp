// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede any windows.h a later include pulls in, so this block leads the file.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif
// clang-format on

#include <core/net/IpSocketAddress.hpp>

#include <core/Overloaded.hpp>
#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <variant>

namespace core::net
{

namespace
{
    /// @param v4 The typed address.
    /// @param port The port, in host byte order.
    /// @return The `sockaddr_in` they spell.
    [[nodiscard]] sockaddr_in sockaddrOf(Ipv4Address const& v4, std::uint16_t port) noexcept
    {
        auto raw = sockaddr_in {};
        raw.sin_family = AF_INET;
        raw.sin_port = htons(port);
        static_assert(sizeof(raw.sin_addr) == sizeof(v4.octets));
        // The octets are already in the order they travel: network order is the written order.
        std::memcpy(&raw.sin_addr, v4.octets.data(), v4.octets.size());
        return raw;
    }

    /// @param v6 The typed IPv6 address.
    /// @param address The socket address it came from, for its port, flow label and scope.
    /// @return The `sockaddr_in6` they spell.
    [[nodiscard]] sockaddr_in6 sockaddrOf(Ipv6Address const& v6, IpSocketAddress const& address) noexcept
    {
        auto raw = sockaddr_in6 {};
        raw.sin6_family = AF_INET6;
        raw.sin6_port = htons(address.port);
        raw.sin6_flowinfo = htonl(address.flowInfo);
        raw.sin6_scope_id = address.scopeId;
        auto bytes = std::array<std::uint8_t, 16> {};
        for (auto const [index, segment]: core::views::enumerate(v6.segments))
        {
            bytes.at(2 * index) = static_cast<std::uint8_t>(segment >> 8U);
            bytes.at((2 * index) + 1) = static_cast<std::uint8_t>(segment & 0xffU);
        }
        static_assert(sizeof(raw.sin6_addr) == bytes.size());
        std::memcpy(&raw.sin6_addr, bytes.data(), bytes.size());
        return raw;
    }

    /// @param raw A `sockaddr_in` read out of an endpoint.
    /// @return Its typed form.
    [[nodiscard]] IpSocketAddress addressOf(sockaddr_in const& raw) noexcept
    {
        auto v4 = Ipv4Address {};
        std::memcpy(v4.octets.data(), &raw.sin_addr, v4.octets.size());
        return IpSocketAddress { .address = v4, .port = ntohs(raw.sin_port) };
    }

    /// @param raw A `sockaddr_in6` read out of an endpoint.
    /// @return Its typed form.
    [[nodiscard]] IpSocketAddress addressOf(sockaddr_in6 const& raw) noexcept
    {
        auto bytes = std::array<std::uint8_t, 16> {};
        std::memcpy(bytes.data(), &raw.sin6_addr, bytes.size());
        auto v6 = Ipv6Address {};
        for (auto [index, segment]: core::views::enumerate(v6.segments))
            segment = static_cast<std::uint16_t>((bytes.at(2 * index) << 8U) | bytes.at((2 * index) + 1));
        return IpSocketAddress { .address = v6,
                                 .port = ntohs(raw.sin6_port),
                                 .flowInfo = ntohl(raw.sin6_flowinfo),
                                 .scopeId = raw.sin6_scope_id };
    }

    /// Reads a @p Sockaddr out of @p endpoint, refusing one too short to hold it.
    /// @param endpoint An endpoint whose family is @p Sockaddr's.
    /// @return The typed address, or @c NetErrorCode::AddressError.
    template <typename Sockaddr>
    [[nodiscard]] std::expected<IpSocketAddress, NetError> readSockaddr(ResolvedEndpoint const& endpoint)
    {
        if (endpoint.length < sizeof(Sockaddr))
            return std::unexpected(makeNetError(
                NetErrorCode::AddressError,
                0,
                std::format("an endpoint of family {} holds {} bytes, short of its sockaddr's {}",
                            endpoint.family,
                            endpoint.length,
                            sizeof(Sockaddr))));
        auto raw = Sockaddr {};
        std::memcpy(&raw, endpoint.storage.data(), sizeof(raw));
        return addressOf(raw);
    }

} // namespace

ResolvedEndpoint toEndpoint(IpSocketAddress const& address) noexcept
{
    return std::visit(core::Overloaded {
                          [&](Ipv4Address const& v4) {
                              auto const raw = sockaddrOf(v4, address.port);
                              return detail::endpointFromSockaddr(&raw, sizeof(raw));
                          },
                          [&](Ipv6Address const& v6) {
                              auto const raw = sockaddrOf(v6, address);
                              return detail::endpointFromSockaddr(&raw, sizeof(raw));
                          },
                      },
                      address.address);
}

std::expected<IpSocketAddress, NetError> toIpSocketAddress(ResolvedEndpoint const& endpoint)
{
    if (endpoint.length > 0 && endpoint.family == AF_INET)
        return readSockaddr<sockaddr_in>(endpoint);
    if (endpoint.length > 0 && endpoint.family == AF_INET6)
        return readSockaddr<sockaddr_in6>(endpoint);
    return std::unexpected(
        makeNetError(NetErrorCode::Unsupported,
                     0,
                     std::format("an endpoint of family {} is no IP address", endpoint.family)));
}

} // namespace core::net
