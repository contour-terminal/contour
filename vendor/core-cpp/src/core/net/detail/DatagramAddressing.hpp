// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The address half of a UDP socket, written once for both platforms.
///
/// `getaddrinfo`, `getnameinfo` and the two sockaddr families are portable — POSIX and
/// Windows/ws2tcpip alike — so the same body serves `posix/UdpSocket.cpp` and
/// `windows/UdpSocket.cpp`, the way `detail/PeerAddress.hpp` already serves the two listeners. What
/// is NOT portable is which socket options exist and how a receive deadline is spelled, and none of
/// that is here: this header holds only what both sides compute identically. Private to
/// `core::net`, so no platform header reaches a consumer through it.

// winsock2.h MUST precede any windows.h a later include pulls in.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <netdb.h>
    #include <netinet/in.h>
#endif
// clang-format on

#include <core/net/IDatagramSocket.hpp>
#include <core/net/SocketAddress.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace core::net::detail
{

/// Reads a sockaddr back as a host and a port.
///
/// The two halves stay apart all the way out, so nothing here joins them — see
/// @c core::net::DatagramAddress for why.
///
/// **The host comes from `getnameinfo` rather than from the `inet_ntop` behind
/// `detail::formatPeer`, and that is deliberate:** `getnameinfo` appends the `%scope` suffix for a
/// link-local IPv6 address and `inet_ntop` does not. This address is handed straight back to
/// `send`, so an address that lost its zone would resolve to something unroutable — which is the
/// one thing a reply-to-the-sender must not do.
/// @param storage The address.
/// @param length Its length.
/// @return The address, with an empty host when it cannot be rendered.
[[nodiscard]] inline DatagramAddress datagramAddressOf(sockaddr_storage const& storage, socklen_t length)
{
    auto host = std::array<char, NI_MAXHOST> {};

    if (::getnameinfo(reinterpret_cast<sockaddr const*>(&storage),
                      length,
                      host.data(),
                      static_cast<unsigned>(host.size()),
                      nullptr,
                      0,
                      NI_NUMERICHOST)
        != 0)
        return {};

    // The port through the one family switch `SocketAddress.hpp` keeps, rather than a second copy
    // of it here: it is a 16-bit field in the sockaddr, so no text round trip is needed either.
    return DatagramAddress { .host = std::string { host.data() },
                             .port = portOfSockaddr(&storage, static_cast<std::uint32_t>(length)) };
}

/// An owning `addrinfo*`, so no resolution path can leak one.
///
/// A raw `addrinfo*` with a `freeaddrinfo` at each exit is the shape upstream had, and it has one
/// exit per candidate plus one per early return — the rulebook's "no raw owning pointer" is exactly
/// this case.
class AddressList
{
  public:
    AddressList() = default;

    /// Takes ownership of what `getaddrinfo` produced.
    /// @param head The list, or nullptr.
    explicit AddressList(addrinfo* head) noexcept: _head { head } {}

    AddressList(AddressList const&) = delete;
    AddressList& operator=(AddressList const&) = delete;

    AddressList(AddressList&& other) noexcept: _head { std::exchange(other._head, nullptr) } {}

    AddressList& operator=(AddressList&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _head = std::exchange(other._head, nullptr);
        }
        return *this;
    }

    ~AddressList() { reset(); }

    /// @return The first candidate, or nullptr when the list is empty.
    [[nodiscard]] addrinfo const* get() const noexcept { return _head; }

    /// @return Whether the list holds at least one candidate.
    [[nodiscard]] explicit operator bool() const noexcept { return _head != nullptr; }

  private:
    void reset() noexcept
    {
        if (_head != nullptr)
            ::freeaddrinfo(std::exchange(_head, nullptr));
    }

    addrinfo* _head { nullptr };
};

/// What a resolved endpoint is for.
///
/// An `enum class` rather than the `bool` this would otherwise be, because `AI_PASSIVE` changes
/// what an empty host resolves to and `true` at a call site says neither which nor why.
enum class EndpointUse : std::uint8_t
{
    Bind, ///< The address will be bound (`AI_PASSIVE`).
    Send, ///< The address will be sent to.
};

/// Resolves @p host : @p port for a datagram socket.
///
/// `AI_NUMERICSERV` because the service is `std::to_string` of a `std::uint16_t` and can never be a
/// name, which skips the `/etc/services` lookup the resolver would otherwise have to rule out
/// first.
/// @param host The host to resolve; must not be empty (see @c core::net::openUdpSocket's callers,
///        which refuse that before reaching here).
/// @param port The port, in host byte order.
/// @param use Whether the result is to be bound or sent to.
/// @return The candidates, empty when nothing resolved.
[[nodiscard]] inline AddressList resolveDatagramEndpoint(std::string const& host,
                                                         std::uint16_t port,
                                                         EndpointUse use)
{
    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV | (use == EndpointUse::Bind ? AI_PASSIVE : 0);

    auto const service = std::to_string(port);
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0)
        return AddressList {};
    return AddressList { resolved };
}

} // namespace core::net::detail
