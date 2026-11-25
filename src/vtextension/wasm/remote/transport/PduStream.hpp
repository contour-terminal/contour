// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// PDUs over a Channel: frames what it sends, and reads whole frames back, refusing any the codec refuses.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

/// Why a stream could not send or receive a PDU. Every one ends the stream, except an OverCap from send(),
/// which sent nothing.
enum class StreamError : uint8_t
{
    Closed,    ///< The channel closed, or was shut down.
    Io,        ///< The channel failed.
    Malformed, ///< The peer sent a frame the codec refuses: a malformed one, or one of the wrong kind,
               ///< direction or channel.
    OverCap,   ///< The peer sent a frame larger than its kind may be; or, from send(), the PDU holds a
               ///< list over its cap, and was not sent.
};

/// One channel's PDUs. Owns the channel and a receive buffer; used by one thread at a time, which may both
/// send and receive.
class PduStream
{
  public:
    /// @param channel The channel it reads and writes.
    /// @param kind Which channel it is: a PDU of the other is refused.
    /// @param arriving The way PDUs it receives travel: a PDU travelling the other way is refused.
    /// @param limits The caps of the PDUs whose size follows the stream's limits.
    PduStream(Channel channel, proto::ChannelKind kind, proto::Direction arriving, proto::PduLimits limits);

    /// Sends one PDU.
    /// @param pdu The PDU.
    /// @return Nothing once it is sent; why it was not. `OverCap` if a list it carries exceeds its cap
    ///         (Pdu.hpp): nothing was sent, and the stream remains usable.
    [[nodiscard]] std::expected<void, StreamError> send(proto::Pdu const& pdu);

    /// Receives the next PDU, blocking until all of it has arrived. A frame larger than its kind may be fails
    /// as soon as its header arrives, without waiting for its body.
    /// @return The PDU; why none could be received.
    [[nodiscard]] std::expected<proto::Pdu, StreamError> receive();

    /// Shuts the channel down: a receive blocked on it, from any thread, fails `Closed`.
    void shutdown() noexcept;

  private:
    Channel _channel;
    proto::ChannelKind _kind;
    proto::Direction _arriving;
    proto::PduLimits _limits;
    std::vector<uint8_t> _buffer; ///< Received bytes; the first _buffered of them are not consumed yet.
    std::size_t _buffered = 0;
};

} // namespace vtextension::wasm::remote::transport
