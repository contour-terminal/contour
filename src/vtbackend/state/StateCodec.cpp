// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateCodec.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>

namespace vtbackend
{

namespace
{
    constexpr auto Magic = std::array<uint8_t, 4> { 'C', 'T', 'S', 'T' };

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

StateWriter::StateWriter(uint32_t formatVersion)
{
    _buffer.insert(_buffer.end(), Magic.begin(), Magic.end());
    varint(formatVersion);
}

void StateWriter::varint(uint64_t value)
{
    auto encoded = std::array<uint8_t, MaxVarintBytes> {};
    auto const count = encodeVarint(value, encoded);
    _buffer.insert(_buffer.end(), encoded.begin(), std::next(encoded.begin(), static_cast<ptrdiff_t>(count)));
}

void StateWriter::svarint(int64_t value)
{
    constexpr auto SignShift = std::numeric_limits<int64_t>::digits;
    varint((static_cast<uint64_t>(value) << 1U) ^ static_cast<uint64_t>(value >> SignShift));
}

void StateWriter::bytes(std::span<uint8_t const> data)
{
    varint(data.size());
    raw(data);
}

void StateWriter::string(std::string_view text)
{
    bytes(std::span { reinterpret_cast<uint8_t const*>(text.data()), text.size() });
}

void StateWriter::raw(std::span<uint8_t const> data)
{
    _buffer.insert(_buffer.end(), data.begin(), data.end());
}

void StateWriter::endFrame(Frame frame)
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

std::vector<uint8_t> StateWriter::take() &&
{
    return std::move(_buffer);
}

std::expected<StateReader, StateDecodeError> StateReader::open(std::span<uint8_t const> stream)
{
    if (stream.size() < Magic.size())
        return std::unexpected { StateDecodeError::Truncated };
    if (!std::ranges::equal(stream.first(Magic.size()), Magic))
        return std::unexpected { StateDecodeError::MalformedRecord };
    auto reader = StateReader { stream, 0 };
    reader._position = Magic.size();
    auto const version = reader.varint();
    if (!version)
        return std::unexpected { version.error() };
    if (*version < OldestReadableTerminalStateFormat || *version > TerminalStateFormatVersion)
        return std::unexpected { StateDecodeError::UnknownFormatVersion };
    reader._formatVersion = static_cast<uint32_t>(*version);
    return reader;
}

std::expected<uint8_t, StateDecodeError> StateReader::u8()
{
    if (atEnd())
        return std::unexpected { StateDecodeError::Truncated };
    return _data[_position++];
}

std::expected<uint64_t, StateDecodeError> StateReader::varint()
{
    auto result = uint64_t { 0 };
    for (auto const group: std::views::iota(0U, MaxVarintBytes))
    {
        auto const byte = u8();
        if (!byte)
            return std::unexpected { byte.error() };
        // The tenth group holds bit 63 only: anything above it would be dropped, decoding another value.
        if (group == MaxVarintBytes - 1 && (*byte & VarintPayloadMask & ~LastGroupPayloadMask) != 0)
            return std::unexpected { StateDecodeError::MalformedRecord };
        result |= static_cast<uint64_t>(*byte & VarintPayloadMask) << (group * VarintPayloadBits);
        if ((*byte & VarintContinuation) == 0)
            return result;
    }
    return std::unexpected { StateDecodeError::MalformedRecord };
}

std::expected<int64_t, StateDecodeError> StateReader::svarint()
{
    return varint().transform([](uint64_t zigzag) {
        return static_cast<int64_t>(zigzag >> 1U) ^ -static_cast<int64_t>(zigzag & 1U);
    });
}

std::expected<std::span<uint8_t const>, StateDecodeError> StateReader::bytes()
{
    return varint().and_then(
        [this](uint64_t length) -> std::expected<std::span<uint8_t const>, StateDecodeError> {
            if (length > remaining())
                return std::unexpected { StateDecodeError::Truncated };
            return raw(static_cast<size_t>(length));
        });
}

std::expected<std::string, StateDecodeError> StateReader::string()
{
    return bytes().transform([](std::span<uint8_t const> data) {
        return std::string { reinterpret_cast<char const*>(data.data()), data.size() };
    });
}

std::expected<std::span<uint8_t const>, StateDecodeError> StateReader::raw(size_t count)
{
    if (count > remaining())
        return std::unexpected { StateDecodeError::Truncated };
    auto const result = _data.subspan(_position, count);
    _position += count;
    return result;
}

} // namespace vtbackend
