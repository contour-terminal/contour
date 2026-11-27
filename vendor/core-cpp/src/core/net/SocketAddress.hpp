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

#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

    /// Resolves @p host : @p port as @c resolve does, and says WHICH failure a failure is.
    ///
    /// What the asynchronous resolvers call, so a name that does not exist
    /// (@c NetErrorCode::NameNotFound) reaches their caller apart from a resolver that could not
    /// answer now (@c NetErrorCode::ResolverTemporaryFailure).
    ///
    /// **The default knows only words**, and classifies nothing: it wraps @c resolve's message into
    /// @c NetErrorCode::AddressError, the code that claims nothing about whether asking again could
    /// help, with the message as the error's context. A resolver that knows the cause overrides it,
    /// as @c SystemAddressResolver does through @c resolverErrorTable.
    /// @param host As for @c resolve.
    /// @param port As for @c resolve.
    /// @return A non-empty list of candidates, or the classified failure.
    [[nodiscard]] virtual std::expected<std::vector<ResolvedEndpoint>, NetError> resolveClassified(
        std::string_view host, std::uint16_t port)
    {
        return resolve(host, port).transform_error([](std::string message) {
            return makeNetError(NetErrorCode::AddressError, 0, std::move(message));
        });
    }
};

/// The `getaddrinfo`-backed resolver: the one place in this library that issues the lookup.
class CORE_NET_API SystemAddressResolver final: public IAddressResolver
{
  public:
    /// @copydoc IAddressResolver::resolve
    ///
    /// The message is the context of what @c resolveClassified reports.
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view host, std::uint16_t port) override;

    /// @copydoc IAddressResolver::resolveClassified
    ///
    /// Classified by @c resolverErrorTable. A lookup that succeeded with no address this library
    /// can carry is @c NetErrorCode::AddressError.
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, NetError> resolveClassified(
        std::string_view host, std::uint16_t port) override;
};

/// One row of @c resolverErrorTable: a code `getaddrinfo` returns, and its category.
struct ResolverErrorRow
{
    int gaiCode;       ///< The platform's code: an `EAI_*` value on POSIX, a `WSA*` value on Windows.
    NetErrorCode code; ///< What a caller is told.
};

/// Every code this platform's `getaddrinfo` can return, classified: one table per OS, in
/// `posix/SocketsPosix.cpp` and `windows/SocketsWin32.cpp`.
///
/// A code no row names is @c NetErrorCode::AddressError (@c detail::classifyResolverError). Each
/// platform's test asserts the table complete -- POSIX by asking `gai_strerror` which codes its libc
/// knows -- so a new code is a failing case until its row is a decision.
/// @return The table; each code appears once.
[[nodiscard]] CORE_NET_API std::span<ResolverErrorRow const> resolverErrorTable() noexcept;

/// @return The process-wide @c SystemAddressResolver, for a caller with no resolver of its own.
///
/// An ambient resource with a name, in the shape this tree already uses for
/// @c platform::defaultSteadyClock: a default argument a caller can replace, rather than a
/// hard-coded call nobody can reach past.
[[nodiscard]] CORE_NET_API IAddressResolver& defaultAddressResolver() noexcept;

/// Formats the address held in @p endpoint as a printable host string — an IPv4 dotted-quad
/// ("203.0.113.7") or an IPv6 textual address ("::1").
///
/// The port is deliberately omitted: this feeds a connection's log prefix, which records the
/// address, and that is how the accept path already formats it.
/// @param endpoint The endpoint whose stored sockaddr is rendered.
/// @return The printable host, or "" for an empty or unknown-family endpoint.
[[nodiscard]] CORE_NET_API std::string formatPeerAddress(ResolvedEndpoint const& endpoint);

namespace detail
{

    /// Is this host text a literal address rather than a name to be looked up?
    ///
    /// **Load-bearing rather than an optimisation**: it is what keeps a dial to a literal off a
    /// resolver thread, and what lets the whole connect path be tested with no thread existing.
    ///
    /// Answered with `inet_pton` for both families rather than by inspecting the characters, so
    /// there is one definition of "literal" and it is the platform's own -- with one exception: a
    /// **scoped** literal (`fe80::1%eth0`) is a name on every platform. macOS's `inet_pton` accepts
    /// the zone and drops it, so treating it as a literal there would lose the scope a link-local
    /// dial needs; `getaddrinfo` keeps it. What must never happen is a NAME reported as a literal:
    /// it would be handed to `::connect` as an address and could not resolve at all.
    /// @param host Host text, unbracketed.
    /// @return True when @p host parses as an IPv4 or IPv6 literal.
    [[nodiscard]] CORE_NET_API bool isNumericHost(std::string_view host) noexcept;

    /// Parses a literal address into the endpoint a lookup of it would produce, with no lookup.
    ///
    /// The same parse as @c isNumericHost, so the two never disagree about what a literal is. Text
    /// with an IPv6 zone suffix (`%eth0`) is no literal on any platform, so its scope reaches
    /// `getaddrinfo` instead of being dropped. Only the strict forms are literals: a dotted quad, or IPv6
    /// text (an IPv4-mapped `::ffff:1.2.3.4` included). The shorthand `127.1` that `getaddrinfo` also accepts
    /// is not one; it is a name, and a resolver looks it up -- which `getaddrinfo` then answers as
    /// `127.0.0.1`. Text with a port, brackets or an embedded NUL is no literal.
    /// @param host Host text, unbracketed.
    /// @param port The port to carry, in host byte order.
    /// @return A `sockaddr_in` or `sockaddr_in6` endpoint whose protocol is TCP, or nothing when
    ///         @p host is not a literal.
    [[nodiscard]] CORE_NET_API std::optional<ResolvedEndpoint> endpointOfLiteral(std::string_view host,
                                                                                 std::uint16_t port) noexcept;

    /// The longest host text an asynchronous resolver looks up: a DNS name's 253 bytes.
    constexpr auto MaxLookupHostLength = std::size_t { 253 };

    /// Refuses host text that names no host before any resolver sees it.
    ///
    /// Empty text reaches `getaddrinfo` as the wildcard address rather than a name, an embedded NUL
    /// cuts the C string it reads short (`a.test\0b.test` would look up `a.test`), and more than
    /// @c MaxLookupHostLength bytes is no DNS name. The asynchronous resolvers ask this first, so none
    /// of the three takes a slot, a thread, or a lookup; @c bind keeps its passive wildcard.
    /// @param host Host text, unbracketed.
    /// @return Nothing, or @c NetErrorCode::AddressError naming which rule @p host broke.
    [[nodiscard]] CORE_NET_API std::expected<void, NetError> checkLookupHost(std::string_view host);

    /// @param gaiCode A failure code `getaddrinfo` returned.
    /// @return Its row's category in @c resolverErrorTable, or @c NetErrorCode::AddressError -- the
    ///         documented catch-all, a permanent resolver failure -- for a code no row names.
    [[nodiscard]] CORE_NET_API NetErrorCode classifyResolverError(int gaiCode) noexcept;

    /// The OS error number behind a `getaddrinfo` failure, for @c NetError::systemCode.
    ///
    /// Per platform, beside its table: on POSIX only `EAI_SYSTEM` has one (it is `errno`, so call
    /// this before anything else can change it), and every other code is 0; on Windows the code is
    /// itself the `WSAGetLastError` value.
    /// @param gaiCode A failure code `getaddrinfo` returned.
    /// @return The OS error number, or 0.
    [[nodiscard]] CORE_NET_API int resolverSystemCode(int gaiCode) noexcept;

    /// Copies a raw sockaddr into a platform-free @c ResolvedEndpoint, reading the family from
    /// the sockaddr itself.
    /// @param sockaddr Pointer to a `sockaddr` / `sockaddr_in` / `sockaddr_in6`.
    /// @param length Valid byte count (a `socklen_t` value).
    /// @return The captured endpoint; an all-zero endpoint when @p length is 0 or exceeds
    ///         @c ResolvedEndpoint::StorageSize.
    [[nodiscard]] CORE_NET_API ResolvedEndpoint endpointFromSockaddr(void const* sockaddr,
                                                                     std::uint32_t length) noexcept;

    /// Reads the port out of a raw sockaddr.
    ///
    /// Spelled once because the two sockaddr layouts put it at different offsets, and reading the
    /// wrong one yields a plausible-looking number rather than an error — so a second copy of the
    /// family switch would be a second chance to get that silently wrong.
    /// @param sockaddr Pointer to a `sockaddr` / `sockaddr_in` / `sockaddr_in6`.
    /// @param length Valid byte count (a `socklen_t` value).
    /// @return The port in host byte order, or 0 for a family that has none.
    [[nodiscard]] CORE_NET_API std::uint16_t portOfSockaddr(void const* sockaddr,
                                                            std::uint32_t length) noexcept;

} // namespace detail

} // namespace core::net
