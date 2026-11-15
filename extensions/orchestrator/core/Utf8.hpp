// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>

namespace orchestrator
{

/// The code point that stands in for every malformed sequence and every value that is not a scalar value.
inline constexpr char32_t ReplacementCharacter = U'�';

/// One code point decoded from the front of a UTF-8 text.
struct Decoded
{
    /// The code point, or ReplacementCharacter for a malformed or truncated sequence.
    char32_t codepoint;
    /// The bytes it occupied: at least 1 for a non-empty text, so a decoding loop always advances.
    std::size_t length;
};

namespace detail
{
    /// One encoded length of UTF-8: what its lead byte looks like and which code points it carries.
    struct Utf8Form
    {
        uint8_t leadMask;   ///< The lead byte's marker bits.
        uint8_t leadBits;   ///< Their value in a lead byte of this form.
        std::size_t length; ///< The bytes of a sequence of this form.
        char32_t minimum;   ///< The smallest code point of this form; anything smaller is overlong.
        char32_t maximum;   ///< The largest code point of this form.
    };

    /// The four forms, shortest first.
    inline constexpr auto Utf8Forms = std::array {
        Utf8Form {
            .leadMask = 0b1000'0000, .leadBits = 0b0000'0000, .length = 1, .minimum = 0, .maximum = 0x7F },
        Utf8Form { .leadMask = 0b1110'0000,
                   .leadBits = 0b1100'0000,
                   .length = 2,
                   .minimum = 0x80,
                   .maximum = 0x7FF },
        Utf8Form { .leadMask = 0b1111'0000,
                   .leadBits = 0b1110'0000,
                   .length = 3,
                   .minimum = 0x800,
                   .maximum = 0xFFFF },
        Utf8Form { .leadMask = 0b1111'1000,
                   .leadBits = 0b1111'0000,
                   .length = 4,
                   .minimum = 0x10000,
                   .maximum = 0x10FFFF },
    };

    inline constexpr uint8_t ContinuationMask = 0b1100'0000;
    inline constexpr uint8_t ContinuationBits = 0b1000'0000;
    inline constexpr uint8_t ContinuationPayload = 0b0011'1111;
    inline constexpr auto ContinuationShift = 6;

    /// Whether @p codepoint is a Unicode scalar value: in range and not a surrogate.
    [[nodiscard]] constexpr bool isScalarValue(char32_t codepoint) noexcept
    {
        return codepoint <= Utf8Forms.back().maximum && (codepoint < 0xD800 || codepoint > 0xDFFF);
    }
} // namespace detail

/// Decodes the code point at the front of @p text.
///
/// A malformed sequence -- a stray continuation byte, an invalid lead byte, a truncated or overlong
/// sequence, a surrogate or a value above U+10FFFF -- decodes as ReplacementCharacter of length 1, so the
/// next call resynchronises on the byte after the bad lead.
/// @param text The bytes to decode from.
/// @return The code point and the bytes it occupied; for an empty @p text, ReplacementCharacter of length 0.
[[nodiscard]] constexpr Decoded decodeUtf8(std::string_view text) noexcept
{
    constexpr auto Invalid = Decoded { .codepoint = ReplacementCharacter, .length = 1 };
    if (text.empty())
        return Decoded { .codepoint = ReplacementCharacter, .length = 0 };

    auto const lead = static_cast<uint8_t>(text.front());
    for (auto const& form: detail::Utf8Forms)
    {
        if ((lead & form.leadMask) != form.leadBits)
            continue;
        if (text.size() < form.length)
            return Invalid;
        auto codepoint = static_cast<char32_t>(lead & static_cast<uint8_t>(~form.leadMask));
        for (auto const byte: text.substr(1, form.length - 1))
        {
            auto const continuation = static_cast<uint8_t>(byte);
            if ((continuation & detail::ContinuationMask) != detail::ContinuationBits)
                return Invalid;
            codepoint =
                (codepoint << detail::ContinuationShift) | (continuation & detail::ContinuationPayload);
        }
        if (codepoint < form.minimum || !detail::isScalarValue(codepoint))
            return Invalid;
        return Decoded { .codepoint = codepoint, .length = form.length };
    }
    return Invalid;
}

/// Appends the UTF-8 encoding of @p codepoint to @p out.
///
/// A value that is not a Unicode scalar value (a surrogate, or above U+10FFFF) is appended as
/// ReplacementCharacter.
/// @param out The string to append to.
/// @param codepoint The code point to encode.
constexpr void appendUtf8(std::string& out, char32_t codepoint)
{
    if (!detail::isScalarValue(codepoint))
        codepoint = ReplacementCharacter;
    for (auto const& form: detail::Utf8Forms)
    {
        if (codepoint > form.maximum)
            continue;
        auto const shiftOf = [codepoint](std::size_t index) {
            return codepoint >> (detail::ContinuationShift * index);
        };
        out.push_back(static_cast<char>(form.leadBits | shiftOf(form.length - 1)));
        for (auto const index: std::views::iota(std::size_t { 0 }, form.length - 1) | std::views::reverse)
            out.push_back(
                static_cast<char>(detail::ContinuationBits | (shiftOf(index) & detail::ContinuationPayload)));
        return;
    }
}

} // namespace orchestrator
