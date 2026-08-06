// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one decode-or-read-more loop every native-protocol endpoint runs.
/// (Lives beside the transports, not in proto/ — the codec stays std-only.)

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <coro/Task.hpp>
#include <net/ISocket.hpp>
#include <net/Sockets.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Why a PDU pump stopped.
enum class PumpStop : std::uint8_t
{
    HandlerStopped, ///< The handler returned false — a normal detach or a rejected handshake.
    PeerClosed,     ///< A clean end of stream.
    TransportError, ///< The socket read failed; see PumpResult::transportError.
    ProtocolError,  ///< The peer violated the wire format; see PumpResult::decodeError.
};

/// The pump's terminal state: the reason a connection's read loop ended.
///
/// Returned rather than logged. `pumpPdus` is shared by the server and the attach client, so it
/// can pick neither a log category nor a connection identity — logging here would emit the same
/// contextless line from both sides. Handing the reason back lets each caller report it with its
/// own category and its own connection id, and lets the pump be tested with no log sink at all.
struct PumpResult
{
    PumpStop stop = PumpStop::HandlerStopped;
    std::optional<proto::DecodeError> decodeError; ///< Set when stop == ProtocolError.
    std::optional<net::NetError> transportError;   ///< Set when stop == TransportError.

    /// @param stop Why the loop ended, for the reasons that carry no payload.
    /// @return A result carrying only that reason.
    [[nodiscard]] static PumpResult stopped(PumpStop stop)
    {
        auto result = PumpResult {};
        result.stop = stop;
        return result;
    }

    /// @param error What the decoder rejected.
    /// @return The peer violated the wire format.
    [[nodiscard]] static PumpResult protocolError(proto::DecodeError error)
    {
        auto result = stopped(PumpStop::ProtocolError);
        result.decodeError = error;
        return result;
    }

    /// @param error What the transport reported.
    /// @return The socket read failed.
    [[nodiscard]] static PumpResult transportFailure(net::NetError error)
    {
        auto result = stopped(PumpStop::TransportError);
        result.transportError = std::move(error);
        return result;
    }
};

/// Renders why a pump stopped, for the caller to prefix with its own identity.
///
/// The projection lives here, not in each transport, because both the daemon and the attach
/// client had the same four-case switch with the same severity split — and a third transport
/// would have copied it again. The pump still picks neither a category nor an identity: the
/// caller does both.
/// @param outcome What pumpPdus reported.
/// @return The reason, or nullopt when there is nothing worth saying (an ordinary
///         handler-requested stop, already reported wherever it was decided).
[[nodiscard]] inline std::optional<std::string> describe(PumpResult const& outcome)
{
    switch (outcome.stop)
    {
        case PumpStop::ProtocolError:
            return std::format("malformed frame ({}); dropping the connection", *outcome.decodeError);
        case PumpStop::TransportError:
            return std::format("read failed: {}", outcome.transportError->toString());
        case PumpStop::PeerClosed: return std::string { "disconnected" };
        case PumpStop::HandlerStopped: break;
    }
    return std::nullopt;
}

/// @param stop Why a pump stopped.
/// @return Whether that reason is a failure (an error) rather than an ordinary end.
[[nodiscard]] inline bool isFailure(PumpStop stop) noexcept
{
    return stop == PumpStop::ProtocolError || stop == PumpStop::TransportError;
}

/// Decodes PDUs off @p socket and hands each to @p handler until the handler
/// returns false, the stream ends, or a protocol error occurs.
/// @param socket The transport to read from (not owned; a pointer, since
///        coroutine reference parameters can dangle).
/// @param handler Consumes one decoded frame; false stops the pump.
/// @return Why the loop ended.
[[nodiscard]] inline coro::Task<PumpResult> pumpPdus(net::ISocket* socket,
                                                     std::function<bool(proto::DecodedFrame const&)> handler)
{
    auto buffer = std::vector<std::byte> {};
    auto consumed = std::size_t { 0 };
    // Compact only when the consumed prefix grows past a threshold, so the
    // common case — many small PDUs — never shifts the buffer. The threshold
    // is low enough that the accumulated "waste" on a connection that keeps
    // streaming but never hits it stays well under the worst-case frame size.
    static constexpr auto CompactThreshold = std::size_t { 65536 };
    while (true)
    {
        auto const decoded = proto::decodePdu(std::span { buffer }.subspan(consumed));
        if (!decoded)
        {
            if (decoded.error() != proto::DecodeError::NeedMoreData)
                co_return PumpResult::protocolError(decoded.error());

            auto const appended = co_await net::appendReadChunk(socket, &buffer);
            if (!appended)
                co_return PumpResult::transportFailure(appended.error());
            if (*appended == 0)
                co_return PumpResult::stopped(PumpStop::PeerClosed);
            continue;
        }
        consumed += decoded->consumed;
        if (consumed >= CompactThreshold)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(consumed));
            consumed = 0;
        }
        if (!handler(*decoded))
            co_return PumpResult::stopped(PumpStop::HandlerStopped);
    }
}

} // namespace vthost
