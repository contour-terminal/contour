// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/codec/DecodeError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::codec
{

/// Writes a byte stream of LEB128 varints, zigzag signed varints and length-prefixed frames.
///
/// The stream has no header: a format that wants a magic or a version writes them itself, first.
class Writer
{
  public:
    /// A frame opened by beginFrame(), closed by endFrame().
    struct Frame
    {
        size_t lengthAt = 0; ///< Where its length prefix goes.
    };

    /// Appends one raw byte.
    /// @param value The byte.
    void u8(uint8_t value) { _buffer.push_back(value); }

    /// Appends @p value as an unsigned LEB128 varint.
    /// @param value The number.
    void varint(uint64_t value);

    /// Appends @p value as a zigzag-encoded LEB128 varint.
    /// @param value The number.
    void svarint(int64_t value);

    /// Appends @p data prefixed with its length.
    /// @param data The bytes.
    void bytes(std::span<uint8_t const> data);

    /// Appends @p text prefixed with its length.
    /// @param text The text.
    void string(std::string_view text);

    /// Appends @p data as it is, without a length.
    /// @param data The bytes.
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
    std::vector<uint8_t> _buffer;
};

/// Reads what Writer wrote. Every read can fail, and a failed read leaves the reader unusable.
class Reader
{
  public:
    /// @param data The bytes to read; they must outlive the reader and everything it returns.
    explicit Reader(std::span<uint8_t const> data) noexcept: _data { data } {}

    /// @return Whether every byte has been consumed.
    [[nodiscard]] bool atEnd() const noexcept { return _position == _data.size(); }

    /// @return The number of bytes not yet consumed.
    [[nodiscard]] size_t remaining() const noexcept { return _data.size() - _position; }

    /// @return The next raw byte.
    [[nodiscard]] std::expected<uint8_t, DecodeError> u8();

    /// @return The next unsigned varint; MalformedRecord for one that does not fit 64 bits.
    [[nodiscard]] std::expected<uint64_t, DecodeError> varint();

    /// @return The next zigzag-encoded signed varint.
    [[nodiscard]] std::expected<int64_t, DecodeError> svarint();

    /// @return The next length-prefixed byte run, as a view into the stream.
    [[nodiscard]] std::expected<std::span<uint8_t const>, DecodeError> bytes();

    /// @return The next length-prefixed string.
    [[nodiscard]] std::expected<std::string, DecodeError> string();

    /// @param count How many bytes.
    /// @return The next @p count bytes, as a view into the stream.
    [[nodiscard]] std::expected<std::span<uint8_t const>, DecodeError> raw(size_t count);

  private:
    std::span<uint8_t const> _data;
    size_t _position = 0;
};

} // namespace core::codec
