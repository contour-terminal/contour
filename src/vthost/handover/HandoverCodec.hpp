// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vthost/handover/HandoverImage.hpp>

namespace vthost::handover
{

/// Why a handover image could not be read, written to memory or read back from it.
enum class HandoverError : uint8_t
{
    Truncated = 0,        ///< The stream ends inside a value or a frame.
    UnknownFormatVersion, ///< The stream's format is newer than this build, or older than it reads.
    Malformed,            ///< Not a handover image, or a value in it is out of range.
    TerminalState,        ///< A session's terminal state was refused (reported by the adopter).
    MemoryUnavailable,    ///< The handover memory object could not be created, written or read.
    NotSupported,         ///< This platform has no handover memory, or no in-place exec to capture for.
    /// A session runs on a device that is no child process on a PTY of this one (SSH, a channel): nothing
    /// could adopt it after the exec. Refused before any session was quiesced.
    UnsupportedSession,
    /// A session is still starting (HostQuiesceError::SessionStarting). Passes within moments: a capture
    /// asked again succeeds.
    SessionStarting,
    /// A session's escape sequence in flight outgrew what a capture keeps (HostQuiesceError).
    InFlightTailOverflowed,
};

/// The four bytes a handover stream starts with.
inline constexpr std::array<uint8_t, 4> HandoverMagic { 'C', 'H', 'O', 'V' };

/// Encodes @p image as a self-contained stream: the magic, the format version (HandoverFormatVersion), then
/// one frame of its fields. One frame, so a stream cut short is refused as truncated even at a field
/// boundary.
/// @param image What the old process hands over.
/// @return The stream.
[[nodiscard]] std::vector<uint8_t> encodeHandover(HandoverImage const& image);

/// Decodes a stream encodeHandover() wrote, of any format from OldestReadableHandoverFormat up to
/// HandoverFormatVersion. A field the stream lacks is left at its default, one it carries that this build
/// does not know is skipped. Every count is bounded by what the stream holds.
/// @param stream The whole stream.
/// @return The image, or why the stream was refused.
[[nodiscard]] std::expected<HandoverImage, HandoverError> decodeHandover(std::span<uint8_t const> stream);

} // namespace vthost::handover
