// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/StateDecodeError.hpp>

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
inline constexpr uint32_t TerminalStateFormatVersion = 1;

/// The oldest terminal-state format this build can still read.
inline constexpr uint32_t OldestReadableTerminalStateFormat = 1;

/// Writes a state stream: LEB128 varints, zigzag signed varints and length-prefixed frames.
class StateWriter
{
  public:
    /// Starts a stream: writes its header, the magic "CTST" and the format version.
    /// @param formatVersion The format to write. Only tests write anything but the current one.
    explicit StateWriter(uint32_t formatVersion = TerminalStateFormatVersion);

    /// @return A writer with no header, for a payload framed into a stream later.
    [[nodiscard]] static StateWriter payload() { return StateWriter { PayloadTag {} }; }

    /// A frame opened by beginFrame(), closed by endFrame().
    struct Frame
    {
        size_t lengthAt = 0; ///< Where its length prefix goes.
    };

    /// Appends one raw byte.
    void u8(uint8_t value) { _buffer.push_back(value); }

    /// Appends @p value as an unsigned LEB128 varint.
    void varint(uint64_t value);

    /// Appends @p value as a zigzag-encoded LEB128 varint.
    void svarint(int64_t value);

    /// Appends @p data prefixed with its length.
    void bytes(std::span<uint8_t const> data);

    /// Appends @p text prefixed with its length.
    void string(std::string_view text);

    /// Appends @p data as it is, without a length.
    void raw(std::span<uint8_t const> data);

    /// Opens a length-prefixed frame: what is written until endFrame() is its payload.
    /// @return The frame, to be passed to endFrame().
    [[nodiscard]] Frame beginFrame()
    {
        auto const frame = Frame { .lengthAt = _buffer.size() };
        _buffer.push_back(0);
        return frame;
    }

    /// Closes @p frame, writing its length in front of its payload. A payload shorter than 128 bytes costs
    /// nothing more; a longer one is moved up by the bytes its length takes beyond the one reserved.
    /// @param frame The frame beginFrame() opened; frames close innermost first.
    void endFrame(Frame frame);

    /// @return How many bytes are written.
    [[nodiscard]] size_t size() const noexcept { return _buffer.size(); }

    /// Finishes the stream.
    /// @return The bytes written.
    [[nodiscard]] std::vector<uint8_t> take() &&;

  private:
    struct PayloadTag
    {
    };
    explicit StateWriter(PayloadTag) {}

    std::vector<uint8_t> _buffer;
};

/// Reads what StateWriter wrote. Every read can fail, and a failed read leaves the reader unusable.
class StateReader
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

    /// @return Whether every byte has been consumed.
    [[nodiscard]] bool atEnd() const noexcept { return _position == _data.size(); }

    /// @return The number of bytes not yet consumed.
    [[nodiscard]] size_t remaining() const noexcept { return _data.size() - _position; }

    /// @return The next raw byte.
    [[nodiscard]] std::expected<uint8_t, StateDecodeError> u8();

    /// @return The next unsigned varint; MalformedRecord for one that does not fit 64 bits.
    [[nodiscard]] std::expected<uint64_t, StateDecodeError> varint();

    /// @return The next zigzag-encoded signed varint.
    [[nodiscard]] std::expected<int64_t, StateDecodeError> svarint();

    /// @return The next length-prefixed byte run, as a view into the stream.
    [[nodiscard]] std::expected<std::span<uint8_t const>, StateDecodeError> bytes();

    /// @return The next length-prefixed string.
    [[nodiscard]] std::expected<std::string, StateDecodeError> string();

    /// @param count How many bytes.
    /// @return The next @p count bytes, as a view into the stream.
    [[nodiscard]] std::expected<std::span<uint8_t const>, StateDecodeError> raw(size_t count);

  private:
    StateReader(std::span<uint8_t const> data, uint32_t formatVersion) noexcept:
        _data { data }, _formatVersion { formatVersion }
    {
    }

    std::span<uint8_t const> _data;
    size_t _position = 0;
    uint32_t _formatVersion;
};

} // namespace vtbackend
