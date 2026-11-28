// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede any windows.h a later include pulls in, so this block leads the file.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
#endif
// clang-format on

#include <core/net/SocketAddress.hpp>

#include <core/net/detail/PeerAddress.hpp>
#include <core/platform/WinsockInit.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <utility>

namespace core::net
{

namespace
{
    static_assert(sizeof(sockaddr_storage) <= ResolvedEndpoint::StorageSize,
                  "ResolvedEndpoint::storage must hold any sockaddr this platform produces");

    /// @param code A `getaddrinfo` return code.
    /// @return A human-readable message for it.
    [[nodiscard]] std::string resolverMessage(int code)
    {
#ifdef _WIN32
        return std::string { gai_strerrorA(code) };
#else
        return std::string { ::gai_strerror(code) };
#endif
    }

} // namespace

std::expected<std::vector<ResolvedEndpoint>, std::string> SystemAddressResolver::resolve(
    std::string_view host, std::uint16_t port)
{
    return resolveClassified(host, port).transform_error([](NetError error) {
        return std::move(error.context);
    });
}

std::expected<std::vector<ResolvedEndpoint>, NetError> SystemAddressResolver::resolveClassified(
    std::string_view host, std::uint16_t port)
{
    platform::ensureWinsockInitialized();

    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;     // IPv4 and IPv6 both acceptable.
    hints.ai_socktype = SOCK_STREAM; // TCP.
    // AI_PASSIVE so an empty host yields a bind-able wildcard — the BIND path's requirement, and
    // ignored outright once a host is given, which is every dial. AI_NUMERICSERV because the
    // service is always our own numeric port and never a name out of /etc/services.
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    auto const service = std::to_string(port);
    auto const hostText = std::string { host };
    // An empty host must be passed as nullptr, or AI_PASSIVE has nothing to act on.
    auto const* const node = hostText.empty() ? nullptr : hostText.c_str();

    addrinfo* head = nullptr;
    if (auto const rc = ::getaddrinfo(node, service.c_str(), &hints, &head); rc != 0)
    {
        // The system code first: on POSIX it is errno for EAI_SYSTEM, which formatting may change.
        auto const systemCode = detail::resolverSystemCode(rc);
        return std::unexpected(
            makeNetError(detail::classifyResolverError(rc),
                         systemCode,
                         std::format("cannot resolve '{}': {}", host, resolverMessage(rc))));
    }

    auto endpoints = std::vector<ResolvedEndpoint> {};
    auto const* next = head;
    while (next != nullptr)
    {
        // Step to the next candidate first, so every `continue` below moves on to it — the same
        // shape the listeners' own getaddrinfo walks use, and a range-`for` has no linked list to
        // range over.
        auto const* const candidate = std::exchange(next, next->ai_next);
        if (candidate->ai_addrlen == 0 || candidate->ai_addrlen > ResolvedEndpoint::StorageSize)
            continue;
        auto endpoint = ResolvedEndpoint {};
        std::memcpy(endpoint.storage.data(), candidate->ai_addr, candidate->ai_addrlen);
        endpoint.length = static_cast<std::uint32_t>(candidate->ai_addrlen);
        endpoint.family = candidate->ai_family;
        endpoint.protocol = candidate->ai_protocol;
        endpoints.push_back(endpoint);
    }
    ::freeaddrinfo(head);

    if (endpoints.empty())
        return std::unexpected(makeNetError(
            NetErrorCode::AddressError, 0, std::format("cannot resolve '{}': no usable address", host)));
    return endpoints;
}

IAddressResolver& defaultAddressResolver() noexcept
{
    static SystemAddressResolver resolver;
    return resolver;
}

std::string formatPeerAddress(ResolvedEndpoint const& endpoint)
{
    if (endpoint.length == 0 || endpoint.length > ResolvedEndpoint::StorageSize)
        return {};

    // Through the one formatter this module already has, rather than a second `inet_ntop` call
    // site: adding the port or handling an IPv6 scope should land once.
    auto storage = sockaddr_storage {};
    std::memcpy(&storage, endpoint.storage.data(), endpoint.length);
    return formatPeer(storage);
}

namespace detail
{

    std::expected<void, NetError> checkLookupHost(std::string_view host)
    {
        if (host.empty())
            return std::unexpected(
                makeNetError(NetErrorCode::AddressError, 0, "an empty host names no host to look up"));
        if (host.contains('\0'))
            return std::unexpected(
                makeNetError(NetErrorCode::AddressError, 0, "a host with an embedded NUL is no name"));
        if (host.size() > MaxLookupHostLength)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressError,
                0,
                std::format(
                    "a host of {} bytes is longer than a DNS name's {}", host.size(), MaxLookupHostLength)));
        return {};
    }

    bool isNumericHost(std::string_view host) noexcept
    {
        return endpointOfLiteral(host, 0).has_value();
    }

    std::optional<ResolvedEndpoint> endpointOfLiteral(std::string_view host, std::uint16_t port) noexcept
    {
        // An embedded NUL would end the text inet_pton reads early, and "127.0.0.1\0evil" would
        // parse as the address before it.
        if (host.empty() || host.contains('\0'))
            return std::nullopt;

        // A scoped literal (`fe80::1%eth0`) is a name here on every platform, never a literal: macOS's
        // inet_pton accepts the zone and drops it, so the endpoint would lose the scope a link-local
        // dial needs. getaddrinfo keeps it, so such a host goes to the resolver everywhere.
        if (host.contains('%'))
            return std::nullopt;

        // inet_pton needs a NUL-terminated string, and a host is short.
        auto const text = std::string { host };

        auto v4 = sockaddr_in {};
        if (::inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1)
        {
            v4.sin_family = AF_INET;
            v4.sin_port = htons(port);
            auto endpoint = endpointFromSockaddr(&v4, sizeof(v4));
            endpoint.protocol = IPPROTO_TCP;
            return endpoint;
        }

        auto v6 = sockaddr_in6 {};
        if (::inet_pton(AF_INET6, text.c_str(), &v6.sin6_addr) == 1)
        {
            v6.sin6_family = AF_INET6;
            v6.sin6_port = htons(port);
            auto endpoint = endpointFromSockaddr(&v6, sizeof(v6));
            endpoint.protocol = IPPROTO_TCP;
            return endpoint;
        }
        return std::nullopt;
    }

    NetErrorCode classifyResolverError(int gaiCode) noexcept
    {
        auto const table = resolverErrorTable();
        auto const row = std::ranges::find(table, gaiCode, &ResolverErrorRow::gaiCode);
        return row != table.end() ? row->code : NetErrorCode::AddressError;
    }

    ResolvedEndpoint endpointFromSockaddr(void const* sockaddr, std::uint32_t length) noexcept
    {
        auto endpoint = ResolvedEndpoint {};
        if (sockaddr == nullptr || length == 0 || length > ResolvedEndpoint::StorageSize)
            return endpoint;
        std::memcpy(endpoint.storage.data(), sockaddr, length);
        endpoint.length = length;
        // The address family is the first field of every sockaddr variant.
        endpoint.family = reinterpret_cast<struct sockaddr const*>(sockaddr)->sa_family;
        return endpoint;
    }

    std::uint16_t portOfSockaddr(void const* sockaddr, std::uint32_t length) noexcept
    {
        if (sockaddr == nullptr || length < sizeof(struct sockaddr))
            return 0;

        auto const family = reinterpret_cast<struct sockaddr const*>(sockaddr)->sa_family;
        if (family == AF_INET && length >= sizeof(sockaddr_in))
            return ntohs(reinterpret_cast<sockaddr_in const*>(sockaddr)->sin_port);
        if (family == AF_INET6 && length >= sizeof(sockaddr_in6))
            return ntohs(reinterpret_cast<sockaddr_in6 const*>(sockaddr)->sin6_port);
        return 0;
    }

} // namespace detail

} // namespace core::net
