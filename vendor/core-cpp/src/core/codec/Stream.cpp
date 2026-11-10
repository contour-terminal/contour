// SPDX-License-Identifier: Apache-2.0
#include <core/codec/Stream.hpp>

#include <array>
#include <iterator>
#include <limits>
#include <ranges>

namespace core::codec
{

namespace
{
    constexpr uint8_t VarintPayloadMask = 0b0111'1111;
    constexpr uint8_t VarintContinuation = 0b1000'0000;
    constexpr unsigned VarintPayloadBits = 7;
    /// A 64-bit value needs ceil(64 / 7) groups.
    constexpr unsigned MaxVarintBytes =
        (std::numeric_limits<uint64_t>::digits + VarintPayloadBits - 1) / VarintPayloadBits;
    /// The last group carries the one bit 64 leaves over: 64 = 9 * 7 + 1.
    constexpr uint8_t LastGroupPayloadMask = 0b0000'0001;

    /// Encodes @p value as a varint into @p out.
    /// @return How many bytes it took.
    size_t encodeVarint(uint64_t value, std::array<uint8_t, MaxVarintBytes>& out) noexcept
    {
        auto count = size_t { 0 };
        while (value > VarintPayloadMask)
        {
            out.at(count++) = static_cast<uint8_t>((value & VarintPayloadMask) | VarintContinuation);
            value >>= VarintPayloadBits;
        }
        out.at(count++) = static_cast<uint8_t>(value);
        return count;
    }
} // namespace

void Writer::varint(uint64_t value)
{
    auto encoded = std::array<uint8_t, MaxVarintBytes> {};
    auto const count = encodeVarint(value, encoded);
    _buffer.insert(_buffer.end(), encoded.begin(), std::next(encoded.begin(), static_cast<ptrdiff_t>(count)));
}

void Writer::svarint(int64_t value)
{
    constexpr auto SignShift = std::numeric_limits<int64_t>::digits;
    varint((static_cast<uint64_t>(value) << 1U) ^ static_cast<uint64_t>(value >> SignShift));
}

void Writer::bytes(std::span<uint8_t const> data)
{
    varint(data.size());
    raw(data);
}

void Writer::string(std::string_view text)
{
    bytes(std::span { reinterpret_cast<uint8_t const*>(text.data()), text.size() });
}

void Writer::raw(std::span<uint8_t const> data)
{
    _buffer.insert(_buffer.end(), data.begin(), data.end());
}

void Writer::endFrame(Frame frame)
{
    auto const length = _buffer.size() - frame.lengthAt - 1;
    auto encoded = std::array<uint8_t, MaxVarintBytes> {};
    auto const count = encodeVarint(length, encoded);
    _buffer[frame.lengthAt] = encoded[0];
    if (count > 1)
        _buffer.insert(std::next(_buffer.begin(), static_cast<ptrdiff_t>(frame.lengthAt + 1)),
                       std::next(encoded.begin()),
                       std::next(encoded.begin(), static_cast<ptrdiff_t>(count)));
}

std::vector<uint8_t> Writer::take() &&
{
    return std::move(_buffer);
}

std::expected<uint8_t, DecodeError> Reader::u8()
{
    if (atEnd())
        return std::unexpected { DecodeError::Truncated };
    return _data[_position++];
}

std::expected<uint64_t, DecodeError> Reader::varint()
{
    auto result = uint64_t { 0 };
    for (auto const group: std::views::iota(0U, MaxVarintBytes))
    {
        auto const byte = u8();
        if (!byte)
            return std::unexpected { byte.error() };
        // The tenth group holds bit 63 only: anything above it would be dropped, decoding another value.
        if (group == MaxVarintBytes - 1 && (*byte & VarintPayloadMask & ~LastGroupPayloadMask) != 0)
            return std::unexpected { DecodeError::MalformedRecord };
        result |= static_cast<uint64_t>(*byte & VarintPayloadMask) << (group * VarintPayloadBits);
        if ((*byte & VarintContinuation) == 0)
            return result;
    }
    return std::unexpected { DecodeError::MalformedRecord };
}

std::expected<int64_t, DecodeError> Reader::svarint()
{
    return varint().transform([](uint64_t zigzag) {
        return static_cast<int64_t>(zigzag >> 1U) ^ -static_cast<int64_t>(zigzag & 1U);
    });
}

std::expected<std::span<uint8_t const>, DecodeError> Reader::bytes()
{
    return varint().and_then([this](uint64_t length) -> std::expected<std::span<uint8_t const>, DecodeError> {
        if (length > remaining())
            return std::unexpected { DecodeError::Truncated };
        return raw(static_cast<size_t>(length));
    });
}

std::expected<std::string, DecodeError> Reader::string()
{
    return bytes().transform([](std::span<uint8_t const> data) {
        return std::string { reinterpret_cast<char const*>(data.data()), data.size() };
    });
}

std::expected<std::span<uint8_t const>, DecodeError> Reader::raw(size_t count)
{
    if (count > remaining())
        return std::unexpected { DecodeError::Truncated };
    auto const result = _data.subspan(_position, count);
    _position += count;
    return result;
}

} // namespace core::codec
