// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Frames PDUs, and reads them back, checked against the PduTable.
///
///   varint length   (of the kind and the body; excludes itself)
///   varint kind     (a PduKind; zero and anything past the last kind are refused)
///   body            (the PDU's fields, in declaration order, consumed exactly)
///
/// Fields encode as varints (u32 and u64), one byte per enum, one byte per optional error or failure
/// (0 for none, else the enumerator's value + 1), and byte strings and vectors as a varint count followed by
/// their elements.
///
/// A reader never buffers toward a frame it will refuse: the declared length is checked against the largest
/// cap of any kind before the kind is known, then against the kind's own cap once it is (decodeFrame).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>

namespace vtextension::wasm::remote::proto
{

/// Why a PDU could not be encoded.
enum class EncodeError : uint8_t
{
    OverCap, ///< A list it carries holds more elements than its cap (Pdu.hpp): a reader would refuse it.
};

/// @return The kind of @p pdu.
[[nodiscard]] PduKind kindOf(Pdu const& pdu) noexcept;

/// Encodes @p pdu as one frame, unless a reader would refuse it for a list over its cap: a sender learns that
/// its PDU cannot be sent, rather than its peer that the sender broke the protocol.
/// @param pdu The PDU to encode.
/// @return The frame's bytes; `OverCap` if a list exceeds its cap (MaxImports, MaxCallValues, ...).
[[nodiscard]] std::expected<std::vector<uint8_t>, EncodeError> encodeFrame(Pdu const& pdu);

/// One frame read off the front of a buffer.
struct DecodedFrame
{
    Pdu pdu;              ///< The PDU it carries.
    std::size_t consumed; ///< How many bytes of the buffer the frame spans.
};

/// Decodes the frame at the front of @p buffer.
/// @param buffer The bytes received so far; the frame starts at its first byte.
/// @param arriving The way the frame travels: a PDU of the other direction is `WrongDirection`.
/// @param channel The channel it arrived on: a PDU of the other channel is `WrongChannel`.
/// @param limits The caps of the PDUs whose size follows the stream's limits.
/// @return The PDU and the frame's size. `NeedMoreData` while the frame is incomplete and nothing in it is
///         wrong yet; any other error is a violation that ends the stream.
[[nodiscard]] std::expected<DecodedFrame, DecodeError> decodeFrame(std::span<uint8_t const> buffer,
                                                                   Direction arriving,
                                                                   ChannelKind channel,
                                                                   PduLimits const& limits);

} // namespace vtextension::wasm::remote::proto
