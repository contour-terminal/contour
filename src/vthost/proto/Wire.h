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
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vthost::proto
{

/// The protocol revision exchanged in the Hello handshake before anything else.
///
/// **Pinned at 1 while this protocol is unreleased, and deliberately NOT bumped per change.**
/// Nothing outside this source tree speaks it, so there is no peer for a version number to be
/// compatible with: the counter had reached 13 above a changelog of twelve revisions no reader could
/// ever have observed, which is bookkeeping, not compatibility. Field additions, removals and
/// renames all land without touching this constant.
///
/// The price is that two binaries built from different commits pass the exact-match handshake and
/// then disagree about the bytes, instead of being rejected with a diagnostic — so rebuild the
/// daemon and its clients together. Once the protocol ships, versioning resumes here and the
/// per-change bump becomes real: at that point every wire change needs a new number and a line
/// saying what moved.
constexpr uint32_t CodecVersion = 1;

/// The largest frame payload the decoder will accept. A peer-declared length
/// beyond this is rejected outright (FrameTooLarge) rather than treated as
/// NeedMoreData, so a hostile header claiming a huge payload cannot make the
/// read loop buffer toward it and exhaust memory. Sized well above any frame
/// the byte-bounded WriteQueues (≤ 4 MiB) can emit — images and full-grid
/// snapshots included — so it never rejects legitimate traffic. This is the
/// native-protocol analogue of imsg's MaxMessageSize.
constexpr uint64_t MaxFrameSize = uint64_t { 64 } * 1024 * 1024;

/// The widest grid either side may name, per axis — well past any real display, so it rejects
/// only nonsense.
///
/// It applies in BOTH directions, which is the point of it living here. A client proposing a
/// billion columns would have the host allocate toward it; a server ANNOUNCING one would have the
/// client do the same, since the mirrored terminal allocates a grid of the announced shape.
/// Announced dimensions are held to the same bound as proposed ones, so neither peer can steer the
/// other into an allocation it cannot make.
constexpr uint32_t MaxGridExtent = 10000;

/// The largest codepoint either side may name — Unicode's last scalar value.
///
/// Not a redundant guard on a `char32_t` field: char32_t's own range is 32 bits, so a plain
/// narrowing cast lets 0x1'0000'0041 arrive as 'A' (the wrong glyph, silently), and lets anything
/// in (0x10FFFF, 0xFFFFFFFF] through intact — straight into libunicode's width()/grapheme
/// segmenter and the font shaper as a value that is not a Unicode scalar at all.
constexpr uint64_t MaxCodepoint = 0x10FFFF;

/// How many elements a decoder may reserve when a peer claims @p count of them and @p remaining
/// bytes are left in the frame.
///
/// The bound is in BYTES, not in elements, and that distinction is the whole point. Capping the
/// element count at the byte count only limits the ALLOCATION for one-byte elements: a single frame
/// at MaxFrameSize claiming 2^26 `WireLine`s would reserve 2^26 * sizeof(WireLine) — some three
/// gigabytes — from any peer that can reach the socket. Capping the reserved MEMORY at what the
/// frame could still supply holds for every element type.
///
/// Reserving too little costs nothing but a geometric regrow, which is why the tight bound is the
/// right one: a reserve is a hint, and an honest peer whose elements encode smaller than they store
/// simply grows the vector. A lying count is caught by the decode loop running short.
/// @tparam T The element type being reserved.
/// @param count The element count the peer claims.
/// @param remaining Bytes still unread in the frame.
/// @return The element count to reserve.
template <typename T>
[[nodiscard]] constexpr std::size_t boundedReserveCount(std::size_t count, std::size_t remaining) noexcept
{
    auto const affordable = remaining / sizeof(T);
    return count < affordable ? count : affordable;
}

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
    FrameTooLarge,   ///< A frame's declared payload exceeds MaxFrameSize: a fatal
                     ///< protocol violation, NOT "read more" — a peer must never
                     ///< make the reader buffer an unbounded declared length.
    MalformedPdu,    ///< A complete frame's body ran short mid-value: a lie in
                     ///< the body, never "read more" — readFrame already proved
                     ///< the whole frame is buffered, so this is fatal.
};

/// Names a decode failure for diagnostics.
///
/// A switch rather than a table on purpose: with `-Wswitch` promoted to an error by the
/// pedantic build, a scoped-enum switch carrying no `default:` label is COMPILER-ENFORCED
/// exhaustive, so adding an enumerator without its name is a build break. That is a stronger
/// guarantee than an array, where a missing row is silent.
///
/// @param error The decode error to describe.
/// @return Its enumerator name ("MalformedVarint", "FrameTooLarge", …).
[[nodiscard]] constexpr std::string_view toString(DecodeError error) noexcept
{
    switch (error)
    {
        case DecodeError::NeedMoreData: return "NeedMoreData";
        case DecodeError::MalformedVarint: return "MalformedVarint";
        case DecodeError::CompressedFrame: return "CompressedFrame";
        case DecodeError::Truncated: return "Truncated";
        case DecodeError::TrailingBytes: return "TrailingBytes";
        case DecodeError::VersionMismatch: return "VersionMismatch";
        case DecodeError::FrameTooLarge: return "FrameTooLarge";
        case DecodeError::MalformedPdu: return "MalformedPdu";
    }
    return "Unknown";
}

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
            auto const byte = static_cast<uint8_t>(_data[_offset]);
            // A uint64 spans at most ten base-128 groups; the tenth (shift == 63)
            // may carry only bit 63. A higher data bit — or a continuation bit that
            // demands an eleventh group — would overflow and, under the `<< 63` shift,
            // be silently truncated rather than surfaced, so reject it as malformed.
            if (shift >= 64 || (shift == 63 && byte > 0x01))
                return std::unexpected(DecodeError::MalformedVarint);
            ++_offset;
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

} // namespace vthost::proto

template <>
struct std::formatter<vthost::proto::DecodeError>: formatter<std::string_view>
{
    auto format(vthost::proto::DecodeError value, auto& ctx) const
    {
        return formatter<std::string_view>::format(vthost::proto::toString(value), ctx);
    }
};
