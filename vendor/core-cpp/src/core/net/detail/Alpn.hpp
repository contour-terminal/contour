// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// ALPN (RFC 7301) protocol lists in their wire format, and a server's choice from one: the part of
/// ALPN that needs no OpenSSL, shared by `Tls.cpp` and `testing/StrictTlsPeer.cpp`, whose OpenSSL
/// callbacks are thin adapters over it.

#include <algorithm>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core::net::detail
{

/// ALPN names in wire format: each one behind a length byte.
using AlpnWire = std::vector<unsigned char>;

/// @param names ALPN names, e.g. `"http/1.1"`.
/// @return @p names in wire format, or a reason if one is empty or longer than 255 bytes.
[[nodiscard]] inline std::expected<AlpnWire, std::string> alpnWireFormat(std::span<std::string const> names)
{
    auto wire = AlpnWire {};
    for (auto const& name: names)
    {
        if (name.empty() || name.size() > std::numeric_limits<unsigned char>::max())
            return std::unexpected("an ALPN protocol name must be 1 to 255 bytes: '" + name + "'");
        wire.push_back(static_cast<unsigned char>(name.size()));
        wire.insert(wire.end(), name.begin(), name.end());
    }
    return wire;
}

/// Splits the next name off the front of a wire-format list.
/// @param list The rest of the list; advanced past the name.
/// @return The name, or no value at the end of the list and for a length byte that is zero or runs
///         past the end -- a malformed list ends there, it is not read beyond.
[[nodiscard]] inline std::optional<std::span<unsigned char const>> nextAlpnName(
    std::span<unsigned char const>& list) noexcept
{
    if (list.empty())
        return std::nullopt;
    auto const length = std::size_t { list.front() };
    if (length == 0 || length > list.size() - 1)
    {
        list = {};
        return std::nullopt;
    }
    auto const name = list.subspan(1, length);
    list = list.subspan(1 + length);
    return name;
}

/// A server's ALPN choice: the first of its own names, in its order of preference, that the client
/// offered (RFC 7301 section 3.2).
/// @param own The server's names, wire format.
/// @param offered The client's names, wire format, as they arrived: possibly malformed.
/// @return The chosen name, a view into @p own; no value when nothing is in common.
[[nodiscard]] inline std::optional<std::span<unsigned char const>> selectAlpn(
    std::span<unsigned char const> own, std::span<unsigned char const> offered) noexcept
{
    while (auto const candidate = nextAlpnName(own))
    {
        auto rest = offered;
        while (auto const name = nextAlpnName(rest))
        {
            if (std::ranges::equal(*candidate, *name))
                return candidate;
        }
    }
    return std::nullopt;
}

} // namespace core::net::detail
