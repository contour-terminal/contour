// SPDX-License-Identifier: Apache-2.0
#include <core/net/SocketOption.hpp>

#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/detail/SocketEndpoint.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>

namespace core::net::detail
{

namespace
{
    /// @return The row for @p option on a socket of @p family, or nothing where the platform has none.
    [[nodiscard]] std::optional<SocketOptionRow> rowFor(SocketOption option, AddressFamily family) noexcept
    {
        auto const table = socketOptionTable();
        auto const row = std::ranges::find_if(table, [&](SocketOptionRow const& candidate) noexcept {
            return candidate.option == option && candidate.family == family;
        });
        if (row == table.end())
            return std::nullopt;
        return *row;
    }

    /// @return The refusal for an option the platform has no row for.
    [[nodiscard]] NetError noRow()
    {
        return makeNetError(NetErrorCode::Unsupported, 0, "this platform has no such socket option");
    }

    /// @return The family of the bound socket @p socket, as the kernel reports it.
    [[nodiscard]] std::expected<AddressFamily, NetError> familyOf(platform::NativeHandle socket)
    {
        return socketEndpoint(socket, SocketSide::Local)
            .and_then([](ResolvedEndpoint const& local) { return toIpSocketAddress(local); })
            .transform([](IpSocketAddress const& address) { return address.family(); });
    }
} // namespace

std::expected<std::uint64_t, NetError> readSocketOption(platform::NativeHandle socket,
                                                        AddressFamily family,
                                                        SocketOption option)
{
    auto const row = rowFor(option, family);
    if (!row.has_value())
        return std::unexpected(noRow());
    return getIntOption(socket, NativeOption { .level = row->level, .name = row->name })
        .transform([scale = row->scale](int kernel) { return fromKernel(scale, kernel); });
}

std::expected<void, NetError> writeSocketOption(platform::NativeHandle socket,
                                                AddressFamily family,
                                                SocketOption option,
                                                std::uint64_t value)
{
    auto const row = rowFor(option, family);
    if (!row.has_value())
        return std::unexpected(noRow());
    return setIntOption(
        socket, NativeOption { .level = row->level, .name = row->name }, toKernel(*row, value));
}

std::expected<std::uint64_t, NetError> readSocketOption(platform::NativeHandle socket, SocketOption option)
{
    return familyOf(socket).and_then(
        [&](AddressFamily family) { return readSocketOption(socket, family, option); });
}

std::expected<void, NetError> writeSocketOption(platform::NativeHandle socket,
                                                SocketOption option,
                                                std::uint64_t value)
{
    return familyOf(socket).and_then(
        [&](AddressFamily family) { return writeSocketOption(socket, family, option, value); });
}

} // namespace core::net::detail
