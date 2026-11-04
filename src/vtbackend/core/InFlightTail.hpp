// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace vtbackend
{

/// How many bytes of an escape sequence in flight a terminal keeps for a state handover by default: far more
/// than any sequence but a large image's, which a handover waits out rather than carries.
inline constexpr std::size_t DefaultInFlightTailLimit = std::size_t { 64 } * 1024 * 1024;

/// The capacity the in-flight tail keeps once a sequence ends; a longer sequence's buffer is let go.
inline constexpr std::size_t InFlightTailRetainedCapacity = std::size_t { 4 } * 1024;

/// Whether a terminal still holds every byte of the escape sequence in flight.
///
/// The parser is not part of a terminal's state: the bytes it consumed since it last stood at rest are, and a
/// terminal restored from that state replays them. A sequence longer than Settings::inFlightTailLimit is not
/// kept, and a terminal parsing one cannot be handed over until it ends.
enum class TailCompleteness : uint8_t
{
    Complete = 0, ///< Every byte in flight is kept; an empty tail when the parser is at rest.
    Overflowed,   ///< The sequence in flight outgrew the limit: its bytes are dropped, and so is a handover.
};

/// Where the DCS whose payload is in flight was hooked: what the hook decided may depend on it.
enum class HookOrigin : uint8_t
{
    Stream = 0, ///< In what the host sent.
    MacroBody,  ///< In the body of a running macro, which a macro definition may not start in.
};

} // namespace vtbackend
