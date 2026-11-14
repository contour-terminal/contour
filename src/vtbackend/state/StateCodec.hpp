// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/StateDecodeError.hpp>

#include <core/codec/Stream.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// The newest terminal-state format this build writes.
///
/// The format is tolerant (@see detail::FieldEncoder): a field added or dropped later is not a new version.
/// Only a change an older reader cannot skip over -- a field whose meaning changes under the same id, a
/// new framing -- is.
///
/// Version 2 retired the DRCS fields of KeyStringsState (6 and 7, which were never filled with anything
/// displayable) and added the DECDLD soft fonts and the per-cell soft-glyph references.
inline constexpr uint32_t TerminalStateFormatVersion = 2;

/// The oldest terminal-state format this build can still read.
inline constexpr uint32_t OldestReadableTerminalStateFormat = 1;

/// Writes a state stream: a header, the magic "CTST" and the format version, then core::codec's LEB128
/// varints, zigzag signed varints and length-prefixed frames.
class StateWriter: public core::codec::Writer
{
  public:
    /// Starts a stream: writes its header, the magic "CTST" and the format version.
    /// @param formatVersion The format to write. Only tests write anything but the current one.
    explicit StateWriter(uint32_t formatVersion = TerminalStateFormatVersion);

    /// @return A writer with no header, for a payload framed into a stream later.
    [[nodiscard]] static StateWriter payload() { return StateWriter { PayloadTag {} }; }

  private:
    struct PayloadTag
    {
    };
    explicit StateWriter(PayloadTag) {}
};

/// Reads what StateWriter wrote. Every read can fail, and a failed read leaves the reader unusable.
class StateReader: public core::codec::Reader
{
  public:
    /// Validates the stream header.
    /// @param stream The whole stream.
    /// @return A reader positioned after the header, or why the header is not acceptable.
    [[nodiscard]] static std::expected<StateReader, StateDecodeError> open(std::span<uint8_t const> stream);

    /// A reader over a payload of the stream @p formatVersion was read from.
    /// @param payload       The bytes.
    /// @param formatVersion The stream's format version.
    [[nodiscard]] static StateReader ofPayload(std::span<uint8_t const> payload,
                                               uint32_t formatVersion = TerminalStateFormatVersion) noexcept
    {
        return StateReader { payload, formatVersion };
    }

    /// @return The format the stream was written in.
    [[nodiscard]] uint32_t formatVersion() const noexcept { return _formatVersion; }

  private:
    StateReader(std::span<uint8_t const> data, uint32_t formatVersion) noexcept:
        Reader { data }, _formatVersion { formatVersion }
    {
    }

    uint32_t _formatVersion;
};

} // namespace vtbackend
