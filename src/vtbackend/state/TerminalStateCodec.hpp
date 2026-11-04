// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/state/TerminalState.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace vtbackend
{

/// Encodes a whole terminal state as a self-contained stream: the magic, the format version
/// (TerminalStateFormatVersion), then one frame of every area's fields (@see detail::FieldEncoder).
///
/// The bytes are a function of @p state alone, so two captures of the same state encode alike.
/// @param state What Terminal::captureState() returned.
/// @return The stream a handover sends.
[[nodiscard]] std::vector<uint8_t> encodeTerminalState(TerminalState const& state);

/// Decodes a stream encodeTerminalState() wrote, of any format from OldestReadableTerminalStateFormat
/// up to TerminalStateFormatVersion.
/// @param stream The whole stream.
/// @return The state, or why the stream was refused.
[[nodiscard]] std::expected<TerminalState, StateDecodeError> decodeTerminalState(
    std::span<uint8_t const> stream);

} // namespace vtbackend
