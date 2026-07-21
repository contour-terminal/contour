// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native protocol's byte-level wire primitives (wezterm's codec shape,
/// dependency-free): LEB128 varints, little-endian scalars, and the PDU frame
///
///   varint taggedLength   (= payloadLength << 1 | compressedBit; excludes itself)
///   varint serial         (0 = unsolicited server push)
///   varint ident          (the PDU tag; unknown idents are data, not errors)
///   payload bytes
///
/// The compressed bit is RESERVED: encoders never set it and decoders reject it,
/// which is what lets compression arrive later without a version break.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace muxserver::proto
{

/// The protocol revision exchanged in the Hello handshake before anything else.
constexpr uint32_t CodecVersion = 2;

/// Why a decode could not produce a value. NeedMoreData is a NON-error state:
/// the caller reads more bytes and retries.
enum class DecodeError : uint8_t
{
    NeedMoreData,    ///< The buffer ends mid-value; not a protocol violation.
    MalformedVarint, ///< A varint ran past its maximum width.
    CompressedFrame, ///< The reserved compression bit was set (not supported yet).
    Truncated,       ///< A declared length exceeds the remaining payload.
    TrailingBytes,   ///< A PDU body decoded fine but left bytes over.
    VersionMismatch, ///< The peer speaks an incompatible CodecVersion.
};

/// Append-only byte sink with the wire's primitive writers.
class Writer
{
  public:
    void varint(uint64_t value)
    {
        while (value >= 0x80)
        {
            _buffer.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        _buffer.push_back(static_cast<std::byte>(value));
    }

    /// ZigZag-encoded signed varint (stable row ids are signed).
    void svarint(int64_t value)
    {
        varint((static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63));
    }

    void u8(uint8_t value) { _buffer.push_back(static_cast<std::byte>(value)); }

    void u16(uint16_t value)
    {
        u8(static_cast<uint8_t>(value));
        u8(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value)
    {
        u16(static_cast<uint16_t>(value));
        u16(static_cast<uint16_t>(value >> 16));
    }

    void bytes(std::span<std::byte const> data) { _buffer.insert(_buffer.end(), data.begin(), data.end()); }

    /// varint length + raw bytes.
    void blob(std::span<std::byte const> data)
    {
        varint(data.size());
        bytes(data);
    }

    /// varint length + UTF-8 bytes.
    void string(std::string_view text)
    {
        varint(text.size());
        _buffer.insert(_buffer.end(),
                       reinterpret_cast<std::byte const*>(text.data()),
                       reinterpret_cast<std::byte const*>(text.data() + text.size()));
    }

    [[nodiscard]] std::span<std::byte const> view() const noexcept { return _buffer; }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(_buffer); }
    [[nodiscard]] std::size_t size() const noexcept { return _buffer.size(); }

  private:
    std::vector<std::byte> _buffer;
};

/// Sequential reader over a byte span; every read is checked.
class Reader
{
  public:
    explicit Reader(std::span<std::byte const> data) noexcept: _data(data) {}

    [[nodiscard]] std::expected<uint64_t, DecodeError> varint()
    {
        auto value = uint64_t { 0 };
        auto shift = 0U;
        while (true)
        {
            if (_offset >= _data.size())
                return std::unexpected(DecodeError::NeedMoreData);
            if (shift >= 64)
                return std::unexpected(DecodeError::MalformedVarint);
            auto const byte = static_cast<uint8_t>(_data[_offset++]);
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
                return value;
            shift += 7;
        }
    }

    [[nodiscard]] std::expected<int64_t, DecodeError> svarint()
    {
        return varint().transform(
            [](uint64_t zigzag) { return static_cast<int64_t>((zigzag >> 1) ^ (~(zigzag & 1) + 1)); });
    }

    [[nodiscard]] std::expected<uint8_t, DecodeError> u8()
    {
        if (_offset >= _data.size())
            return std::unexpected(DecodeError::NeedMoreData);
        return static_cast<uint8_t>(_data[_offset++]);
    }

    [[nodiscard]] std::expected<uint16_t, DecodeError> u16()
    {
        return u8().and_then([this](uint8_t low) {
            return u8().transform([low](uint8_t high) { return static_cast<uint16_t>(low | (high << 8)); });
        });
    }

    [[nodiscard]] std::expected<uint32_t, DecodeError> u32()
    {
        return u16().and_then([this](uint16_t low) {
            return u16().transform([low](uint16_t high) {
                return static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16);
            });
        });
    }

    [[nodiscard]] std::expected<std::span<std::byte const>, DecodeError> bytes(std::size_t count)
    {
        if (_data.size() - _offset < count)
            return std::unexpected(DecodeError::Truncated);
        auto const view = _data.subspan(_offset, count);
        _offset += count;
        return view;
    }

    /// varint length + raw bytes.
    [[nodiscard]] std::expected<std::span<std::byte const>, DecodeError> blob()
    {
        return varint().and_then([this](uint64_t count) { return bytes(count); });
    }

    /// varint length + UTF-8 bytes.
    [[nodiscard]] std::expected<std::string, DecodeError> string()
    {
        return blob().transform([](std::span<std::byte const> view) {
            return std::string { reinterpret_cast<char const*>(view.data()), view.size() };
        });
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return _data.size() - _offset; }
    [[nodiscard]] std::size_t consumed() const noexcept { return _offset; }

  private:
    std::span<std::byte const> _data;
    std::size_t _offset = 0;
};

/// One decoded frame: views into the input buffer, plus how many input bytes it spans.
struct Frame
{
    uint64_t serial = 0; ///< Request correlation; 0 = unsolicited push.
    uint64_t ident = 0;  ///< The PDU tag.
    std::span<std::byte const> body;
    std::size_t consumed = 0; ///< Total input bytes this frame occupied.
};

/// Encodes one frame around @p body.
/// @param sink The output writer.
/// @param serial The request serial (0 for pushes).
/// @param ident The PDU tag.
/// @param body The encoded PDU payload.
void writeFrame(Writer& sink, uint64_t serial, uint64_t ident, std::span<std::byte const> body);

/// Decodes the next frame from @p data, or NeedMoreData while it is incomplete.
[[nodiscard]] std::expected<Frame, DecodeError> readFrame(std::span<std::byte const> data);

} // namespace muxserver::proto
