// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a native connection asks of the daemon behind it: its status, a stop, and a census ticket for every
/// client. An endpoint without a daemon behind it -- a test, an embedded host -- is served by the null
/// object, which answers every question NotServed and counts no one.

#include <cstdint>
#include <expected>
#include <string_view>

#include <vthost/ClientCensus.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Why a daemon-control request was not served.
enum class DaemonControlError : uint8_t
{
    NotServed = 0, ///< This endpoint has no daemon behind it (tests, embedded hosts).
};

/// The daemon as its connections see it.
class DaemonControl
{
  public:
    virtual ~DaemonControl() = default;

    /// @return The daemon's status, as a DaemonStatusReply states it.
    [[nodiscard]] virtual std::expected<proto::DaemonStatusReply, DaemonControlError> status() const = 0;

    /// @return The daemon's release, which every accepted ServerHello carries; empty when unknown.
    [[nodiscard]] virtual std::string_view releaseVersion() const noexcept = 0;

    /// Asks the daemon to stop.
    /// @param reason Why.
    /// @return Nothing once the stop is under way.
    [[nodiscard]] virtual std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) = 0;

    /// Counts one more client, for as long as the returned ticket lives.
    /// @return The ticket.
    [[nodiscard]] virtual ClientCensus::Ticket admitClient() = 0;
};

/// The daemon control of an endpoint with no daemon behind it: an empty release, NotServed for status and
/// stop, and an empty ticket for every client.
class NullDaemonControl final: public DaemonControl
{
  public:
    [[nodiscard]] std::expected<proto::DaemonStatusReply, DaemonControlError> status() const override;
    [[nodiscard]] std::string_view releaseVersion() const noexcept override;
    [[nodiscard]] std::expected<void, DaemonControlError> requestStop(proto::StopReason reason) override;
    [[nodiscard]] ClientCensus::Ticket admitClient() override;
};

/// The one NullDaemonControl. It holds no state at all, so every connection of every host may share it, from
/// any thread.
/// @return It, by reference: there is no nullable DaemonControl pointer anywhere.
[[nodiscard]] DaemonControl& nullDaemonControl() noexcept;

/// Wraps @p handler so that each connection it serves holds a census ticket of @p control's for its whole
/// life: for the endpoints whose every connection is a client (tmux control mode, imsg).
/// @param control The daemon that counts the connections; outlives the returned handler.
/// @param handler The connection handler to wrap.
/// @return The counting handler.
[[nodiscard]] ConnectionHandler countedHandler(DaemonControl& control, ConnectionHandler handler);

} // namespace vthost
