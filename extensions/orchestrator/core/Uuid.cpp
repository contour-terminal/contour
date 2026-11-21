// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Uuid.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace orchestrator
{

namespace
{
    /// The byte that carries the version in its high nibble.
    constexpr auto VersionByte = std::size_t { 6 };
    /// The byte that carries the variant in its two high bits.
    constexpr auto VariantByte = std::size_t { 8 };
    constexpr auto Version4 = uint8_t { 0b0100'0000 };
    constexpr auto LowNibble = uint8_t { 0b0000'1111 };
    constexpr auto VariantRfc = uint8_t { 0b1000'0000 };
    constexpr auto LowSixBits = uint8_t { 0b0011'1111 };
    /// The bytes after which the canonical form puts a dash.
    constexpr auto DashesAfter = std::array<std::size_t, 4> { 3, 5, 7, 9 };
    constexpr auto HexDigits = std::string_view { "0123456789abcdef" };
} // namespace

std::string formatUuidV4(std::array<uint8_t, 16> bytes)
{
    bytes[VersionByte] = static_cast<uint8_t>((bytes[VersionByte] & LowNibble) | Version4);
    bytes[VariantByte] = static_cast<uint8_t>((bytes[VariantByte] & LowSixBits) | VariantRfc);
    auto text = std::string {};
    text.reserve(36);
    auto index = std::size_t { 0 };
    for (auto const byte: bytes)
    {
        text += HexDigits[byte >> 4U];
        text += HexDigits[byte & LowNibble];
        if (std::ranges::contains(DashesAfter, index))
            text += '-';
        ++index;
    }
    return text;
}

} // namespace orchestrator
