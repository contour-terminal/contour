// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host protocol's byte-level primitives: LEB128 varints, single bytes and raw byte runs, and
/// the error every decode reports. Dependency-free, so that every framing rule is table-testable.
///
/// Shaped like the native protocol's primitives (vthost/proto/Wire.hpp) but not shared with them: the
/// extension host is a layer below vthost, and must not depend on it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote::proto
{

/// Why a decode could not produce a value. NeedMoreData alone is not a violation: the caller reads more
/// bytes and retries. Every other value is fatal to the stream it came from.
enum class DecodeError : uint8_t
{
    NeedMoreData,    ///< The buffer ends inside the frame; not a protocol violation.
    MalformedVarint, ///< A varint runs past its widest form, or exceeds the field's width.
    OverCap,         ///< A frame, or an element count inside one, exceeds what its kind may carry.
    UnknownKind,     ///< The frame's kind names no PDU.
    WrongDirection,  ///< The PDU travels the other way.
    WrongChannel,    ///< The PDU belongs on the other channel.
    Truncated,       ///< A value runs past the end of its frame.
    TrailingBytes,   ///< The frame holds bytes after its PDU's last field.
    BadEnum,         ///< An enumeration or status byte names no enumerator.
};

/// Names a decode failure for diagnostics.
/// @param error The decode error to describe.
/// @return Its enumerator name.
[[nodiscard]] constexpr std::string_view toString(DecodeError error) noexcept
{
    switch (error)
    {
        case DecodeError::NeedMoreData: return "NeedMoreData";
        case DecodeError::MalformedVarint: return "MalformedVarint";
        case DecodeError::OverCap: return "OverCap";
        case DecodeError::UnknownKind: return "UnknownKind";
        case DecodeError::WrongDirection: return "WrongDirection";
        case DecodeError::WrongChannel: return "WrongChannel";
        case DecodeError::Truncated: return "Truncated";
        case DecodeError::TrailingBytes: return "TrailingBytes";
        case DecodeError::BadEnum: return "BadEnum";
    }
    return "Unknown";
}

/// The most bytes a varint holding a u32 takes.
inline constexpr auto MaxVarint32Bytes = std::size_t { 5 };

/// The most bytes a varint holding a u64 takes.
inline constexpr auto MaxVarint64Bytes = std::size_t { 10 };

/// How many elements a decoder may reserve when a peer claims @p count of them and @p remaining bytes are
/// left in the frame.
///
/// The bound is in bytes, not in elements: capping the element count at the byte count bounds the
/// allocation only for one-byte elements, while capping the reserved memory at what the frame could still
/// supply holds for every element type. A lying count is caught by the decode loop running short.
/// @tparam T The element type being reserved.
/// @param count The element count the peer claims.
/// @param remaining Bytes still unread in the frame.
/// @return The element count to reserve.
template <typename T>
[[nodiscard]] constexpr std::size_t boundedReserveCount(uint64_t count, std::size_t remaining) noexcept
{
    auto const affordable = remaining / sizeof(T);
    return count < affordable ? static_cast<std::size_t>(count) : affordable;
}

/// Append-only byte sink with the wire's primitive writers.
class Writer
{
  public:
    /// Appends @p value as an unsigned LEB128 varint.
    void varint(uint64_t value)
    {
        constexpr auto PayloadMask = uint64_t { 0b0111'1111 };
        constexpr auto ContinueBit = uint8_t { 0b1000'0000 };
        while (value > PayloadMask)
        {
            _buffer.push_back(static_cast<uint8_t>((value & PayloadMask) | ContinueBit));
            value >>= 7;
        }
        _buffer.push_back(static_cast<uint8_t>(value));
    }

    /// Appends one byte.
    void u8(uint8_t value) { _buffer.push_back(value); }

    /// Appends @p data verbatim.
    void bytes(std::span<std::byte const> data)
    {
        auto const offset = _buffer.size();
        _buffer.resize(offset + data.size());
        std::ranges::transform(data,
                               _buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                               [](std::byte value) { return std::to_integer<uint8_t>(value); });
    }

    /// @return How many bytes have been written.
    [[nodiscard]] std::size_t size() const noexcept { return _buffer.size(); }

    /// @return What has been written; the writer is left empty.
    [[nodiscard]] std::vector<uint8_t> take() noexcept { return std::exchange(_buffer, {}); }

  private:
    std::vector<uint8_t> _buffer;
};

/// Sequential reader over one frame's bytes; every read is checked, and running past the end is
/// `Truncated`.
class Reader
{
  public:
    /// @param data The bytes to read, front to back.
    explicit Reader(std::span<uint8_t const> data) noexcept: _data { data } {}

    /// @return The next varint as a u32; `MalformedVarint` past five bytes or 32 bits.
    [[nodiscard]] std::expected<uint32_t, DecodeError> varint32() noexcept
    {
        return varint(MaxVarint32Bytes, 32).transform([](uint64_t value) {
            return static_cast<uint32_t>(value);
        });
    }

    /// @return The next varint as a u64; `MalformedVarint` past ten bytes or 64 bits.
    [[nodiscard]] std::expected<uint64_t, DecodeError> varint64() noexcept
    {
        return varint(MaxVarint64Bytes, 64);
    }

    /// @return The next byte.
    [[nodiscard]] std::expected<uint8_t, DecodeError> u8() noexcept
    {
        if (atEnd())
            return std::unexpected(DecodeError::Truncated);
        return _data[_offset++];
    }

    /// @return The next @p count bytes, viewing the input.
    [[nodiscard]] std::expected<std::span<uint8_t const>, DecodeError> bytes(uint64_t count) noexcept
    {
        if (count > remaining())
            return std::unexpected(DecodeError::Truncated);
        auto const view = _data.subspan(_offset, static_cast<std::size_t>(count));
        _offset += view.size();
        return view;
    }

    /// @return How many bytes are left.
    [[nodiscard]] std::size_t remaining() const noexcept { return _data.size() - _offset; }

    /// @return How many bytes have been read.
    [[nodiscard]] std::size_t consumed() const noexcept { return _offset; }

    /// @return Whether every byte has been read.
    [[nodiscard]] bool atEnd() const noexcept { return _offset == _data.size(); }

  private:
    /// Reads a varint of at most @p maxBytes bytes holding at most @p valueBits bits.
    [[nodiscard]] std::expected<uint64_t, DecodeError> varint(std::size_t maxBytes,
                                                              unsigned valueBits) noexcept
    {
        constexpr auto PayloadMask = uint8_t { 0b0111'1111 };
        constexpr auto ContinueBit = uint8_t { 0b1000'0000 };
        // The last byte may carry only the bits left of the value, and no continuation.
        auto const lastByteBits = valueBits - (7 * static_cast<unsigned>(maxBytes - 1));
        auto const lastByteLimit = static_cast<uint8_t>((1U << lastByteBits) - 1);
        auto value = uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, maxBytes))
        {
            auto const next = u8();
            if (!next)
                return std::unexpected(next.error());
            if (index + 1 == maxBytes && *next > lastByteLimit)
                return std::unexpected(DecodeError::MalformedVarint);
            value |= static_cast<uint64_t>(*next & PayloadMask) << (7 * index);
            if ((*next & ContinueBit) == 0)
                return value;
        }
        return std::unexpected(DecodeError::MalformedVarint);
    }

    std::span<uint8_t const> _data;
    std::size_t _offset = 0;
};

} // namespace vtextension::wasm::remote::proto
