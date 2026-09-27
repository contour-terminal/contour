// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ResolvedEndpoint` and the blocking name-resolution seam behind it.
///
/// **This header names no platform socket type**, which is why the raw `sockaddr` bytes live in a
/// fixed, suitably-aligned array rather than in a `sockaddr_storage`: a public header that
/// included `<winsock2.h>` would put it into every consumer's translation unit
/// (`.agent/rules/platform.md`). `SocketAddress.cpp` static-asserts the array is big enough.
///
/// Imported from fastcached's `Net/SocketAddress.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`, minus its bind half: core-cpp's listeners already
/// own their bind sequence, so `BindAndListen`, `BoundPortOf` and `ReusePort` are deliberately
/// not here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// One candidate endpoint produced by an @c IAddressResolver, ready to hand to `::connect`.
struct ResolvedEndpoint
{
    /// Capacity of @c storage, equal to `sizeof(sockaddr_storage)` on every supported platform.
    static constexpr std::size_t StorageSize = 128;

    /// Raw `sockaddr_in` / `sockaddr_in6` bytes. Aligned for any sockaddr type.
    alignas(alignof(std::max_align_t)) std::array<std::byte, StorageSize> storage {};

    /// Number of valid bytes in @c storage (a `socklen_t` value).
    std::uint32_t length { 0 };

    /// Address family: `AF_INET` or `AF_INET6`.
    int family { 0 };

    /// Transport protocol (e.g. `IPPROTO_TCP`) reported by the resolver.
    int protocol { 0 };
};

/// Name and address resolution, as an injectable seam.
///
/// **Blocking, and that is the point of it being separate from @c IAsyncAddressResolver.**
/// `getaddrinfo` has no portable asynchronous form and takes no timeout, so somebody's thread
/// waits; this interface is the thing that waits, and `IAsyncAddressResolver` is the thing that
/// decides WHOSE thread. A test substitutes a deterministic fake for either.
class IAddressResolver
{
  public:
    IAddressResolver() = default;
    virtual ~IAddressResolver() = default;

    IAddressResolver(IAddressResolver const&) = delete;
    IAddressResolver& operator=(IAddressResolver const&) = delete;
    IAddressResolver(IAddressResolver&&) = delete;
    IAddressResolver& operator=(IAddressResolver&&) = delete;

    /// Resolves @p host : @p port into candidate endpoints in preference order.
    /// @param host Address or hostname, **unbracketed**: "127.0.0.1", "::1", "localhost". An
    ///        empty host means the wildcard address, which is a BIND target and not a dial one —
    ///        @c detail::runConnectFlow refuses it before it gets here.
    /// @param port TCP port in host byte order.
    /// @return A non-empty list of candidates, or a message saying why there are none.
    [[nodiscard]] virtual std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view host, std::uint16_t port) = 0;
};

/// The `getaddrinfo`-backed resolver: the one place in this library that issues the lookup.
class SystemAddressResolver final: public IAddressResolver
{
  public:
    /// @copydoc IAddressResolver::resolve
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view host, std::uint16_t port) override;
};

/// @return The process-wide @c SystemAddressResolver, for a caller with no resolver of its own.
///
/// An ambient resource with a name, in the shape this tree already uses for
/// @c platform::defaultSteadyClock: a default argument a caller can replace, rather than a
/// hard-coded call nobody can reach past.
[[nodiscard]] IAddressResolver& defaultAddressResolver() noexcept;

/// Formats the address held in @p endpoint as a printable host string — an IPv4 dotted-quad
/// ("203.0.113.7") or an IPv6 textual address ("::1").
///
/// The port is deliberately omitted: this feeds a connection's log prefix, which records the
/// address, and that is how the accept path already formats it.
/// @param endpoint The endpoint whose stored sockaddr is rendered.
/// @return The printable host, or "" for an empty or unknown-family endpoint.
[[nodiscard]] std::string formatPeerAddress(ResolvedEndpoint const& endpoint);

namespace detail
{

    /// Is this host text a literal address rather than a name to be looked up?
    ///
    /// **Load-bearing rather than an optimisation**: it is what keeps a dial to a literal off a
    /// resolver thread, and what lets the whole connect path be tested with no thread existing.
    ///
    /// Answered with `inet_pton` for both families rather than by inspecting the characters, so
    /// there is one definition of "literal" and it is the platform's own. Which means a **scoped**
    /// literal (`fe80::1%eth0`) is platform-dependent, and that is fine: glibc's `inet_pton`
    /// rejects the zone suffix and macOS's accepts it, so the same text is a name on one host and
    /// a literal on the next, and either answer produces a working dial. What must never happen is
    /// the other direction — a NAME reported as a literal would be handed to `::connect` as an
    /// address and could not resolve at all.
    /// @param host Host text, unbracketed.
    /// @return True when @p host parses as an IPv4 or IPv6 literal.
    [[nodiscard]] bool isNumericHost(std::string_view host) noexcept;

    /// Copies a raw sockaddr into a platform-free @c ResolvedEndpoint, reading the family from
    /// the sockaddr itself.
    /// @param sockaddr Pointer to a `sockaddr` / `sockaddr_in` / `sockaddr_in6`.
    /// @param length Valid byte count (a `socklen_t` value).
    /// @return The captured endpoint; an all-zero endpoint when @p length is 0 or exceeds
    ///         @c ResolvedEndpoint::StorageSize.
    [[nodiscard]] ResolvedEndpoint endpointFromSockaddr(void const* sockaddr, std::uint32_t length) noexcept;

    /// Reads the port out of a raw sockaddr.
    ///
    /// Spelled once because the two sockaddr layouts put it at different offsets, and reading the
    /// wrong one yields a plausible-looking number rather than an error — so a second copy of the
    /// family switch would be a second chance to get that silently wrong.
    /// @param sockaddr Pointer to a `sockaddr` / `sockaddr_in` / `sockaddr_in6`.
    /// @param length Valid byte count (a `socklen_t` value).
    /// @return The port in host byte order, or 0 for a family that has none.
    [[nodiscard]] std::uint16_t portOfSockaddr(void const* sockaddr, std::uint32_t length) noexcept;

} // namespace detail

} // namespace core::net
