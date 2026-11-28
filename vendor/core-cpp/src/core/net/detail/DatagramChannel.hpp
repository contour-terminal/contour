// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::IDatagramChannel` — the platform half of an @c IAsyncDatagramSocket, and the one place
/// its state machine is written.
///
/// The socket (`AsyncDatagramSocket.cpp`) decides what a call means in each state: whether it is
/// refused, whether it binds the wildcard first, which destination a send takes. A channel only
/// does what it is told, on the OS (`posix/ReadinessDatagramSocket.cpp`,
/// `windows/IocpDatagramSocket.cpp`) or in memory (`testing/InMemoryAsyncDatagram.cpp`). Every
/// state decision is therefore made once, for the real socket and its double alike.

#include <core/async/Task.hpp>
#include <core/net/IAsyncDatagramSocket.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/detail/SocketEndpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace core::net::detail
{

/// What a datagram socket does on the OS, or in memory, once the socket has decided it may.
///
/// **Shared, because a send or a receive outlives the socket that started it.** The task a caller
/// holds keeps its own share of the channel, so a socket destroyed while a receive is parked leaves
/// that frame something to read its outcome from. @c receive and @c send therefore take that share
/// when they are called, before the task first runs.
///
/// No method here checks a state the socket owns (bound, connected, closed by the socket's own
/// @c close); each answers the OS's view, or for a closed channel @c NetErrorCode::Cancelled from a
/// pending operation.
class IDatagramChannel: public std::enable_shared_from_this<IDatagramChannel>
{
  public:
    IDatagramChannel() = default;
    virtual ~IDatagramChannel() = default;
    IDatagramChannel(IDatagramChannel const&) = delete;
    IDatagramChannel& operator=(IDatagramChannel const&) = delete;
    IDatagramChannel(IDatagramChannel&&) = delete;
    IDatagramChannel& operator=(IDatagramChannel&&) = delete;

    /// @param local The address to bind, already checked to be of the channel's family.
    /// @return Nothing, or why it could not bind.
    [[nodiscard]] virtual std::expected<void, NetError> bind(ResolvedEndpoint const& local) = 0;

    /// @param remote The remote to connect to, or `std::nullopt` to disconnect.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] virtual std::expected<void, NetError> connect(
        std::optional<ResolvedEndpoint> const& remote) = 0;

    /// @param side Which end.
    /// @return That end's address.
    [[nodiscard]] virtual std::expected<ResolvedEndpoint, NetError> endpoint(SocketSide side) const = 0;

    /// @param option One of the datagram options.
    /// @return Its value.
    [[nodiscard]] virtual std::expected<std::uint64_t, NetError> option(SocketOption option) const = 0;

    /// @param option One of the datagram options.
    /// @param value Its value.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] virtual std::expected<void, NetError> setOption(SocketOption option,
                                                                  std::uint64_t value) = 0;

    /// @param maxBytes The longest payload accepted, already clamped.
    /// @return One whole datagram; @c NetErrorCode::MessageTooLarge for a longer one, which is dropped.
    [[nodiscard]] virtual async::Task<std::expected<AsyncDatagram, NetError>> receive(
        std::size_t maxBytes) = 0;

    /// @param payload The whole message.
    /// @param to The destination, or `std::nullopt` for the connected remote.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] virtual async::Task<std::expected<void, NetError>> send(
        std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) = 0;

    /// Closes the channel; a pending receive or send resolves with @c NetErrorCode::Cancelled.
    /// Idempotent.
    virtual void close() noexcept = 0;
};

/// Opens the platform's channel: an unbound, non-blocking UDP socket on @p loop.
/// @param loop The loop to drive it on.
/// @param family The address family.
/// @return The channel, or why the socket could not be opened.
[[nodiscard]] std::expected<std::shared_ptr<IDatagramChannel>, NetError> openDatagramChannel(
    EventLoop& loop, AddressFamily family);

/// Wraps @p channel in the one @c IAsyncDatagramSocket implementation.
/// @param channel The channel, unbound.
/// @param family The family it was opened with.
/// @return The socket; it closes the channel when it is closed or destroyed.
[[nodiscard]] std::unique_ptr<IAsyncDatagramSocket> makeAsyncDatagramSocket(
    std::shared_ptr<IDatagramChannel> channel, AddressFamily family);

/// The refusal of a pending operation whose channel was closed under it.
/// @param operation What was pending.
/// @return @c NetErrorCode::Cancelled, systemCode 0.
[[nodiscard]] inline NetError closedUnder(char const* operation)
{
    return makeNetError(NetErrorCode::Cancelled, 0, std::string { operation } + ": the socket was closed");
}

} // namespace core::net::detail
