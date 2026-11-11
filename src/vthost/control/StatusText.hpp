// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/UntrustedText.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vthost::control
{

/// What replaces a byte that is not valid UTF-8.
inline constexpr std::string_view ReplacementCharacter = "\xEF\xBF\xBD";

namespace detail
{
    /// One decoded code point and how many bytes it took.
    struct DecodedCodePoint
    {
        char32_t codePoint; ///< The code point, or nothing meaningful when `valid` is false.
        std::size_t length; ///< Bytes consumed; 1 for an invalid byte.
        bool valid;         ///< Whether the bytes were well-formed UTF-8.
    };

    /// Decodes the code point at the start of @p text, strictly: no overlong form, surrogate or value past
    /// U+10FFFF is valid.
    /// @param text Non-empty UTF-8 text.
    [[nodiscard]] constexpr DecodedCodePoint decodeCodePoint(std::string_view text) noexcept
    {
        auto const lead = static_cast<uint8_t>(text.front());
        auto const invalid = DecodedCodePoint { .codePoint = 0, .length = 1, .valid = false };
        if (lead < 0x80)
            return { .codePoint = lead, .length = 1, .valid = true };
        auto length = std::size_t { 0 };
        auto codePoint = char32_t { 0 };
        auto minimum = char32_t { 0 };
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            codePoint = lead & 0x1Fu;
            minimum = 0x80;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            codePoint = lead & 0x0Fu;
            minimum = 0x800;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            codePoint = lead & 0x07u;
            minimum = 0x10000;
        }
        else
            return invalid;
        if (text.size() < length)
            return invalid;
        for (auto const index: std::array<std::size_t, 3> { 1, 2, 3 })
        {
            if (index >= length)
                break;
            auto const next = static_cast<uint8_t>(text[index]);
            if ((next & 0xC0) != 0x80)
                return invalid;
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return invalid;
        return { .codePoint = codePoint, .length = length, .valid = true };
    }
} // namespace detail

/// Makes text a program or hook sent fit to show: it is untrusted, so it is never interpreted, only filtered.
///
/// Drops C0 and C1 controls (ESC and BEL with them, so an escape sequence loses its introducer) and the
/// invisible and bidirectional formatting characters, as vtbackend::UntrustedTextTreatments says; shows a run
/// of line breaks between two kept characters as one space, and drops one at either end; makes each byte that
/// is not valid UTF-8 U+FFFD; and cuts the result to at most @p maxBytes bytes, on a code-point boundary.
/// @param untrusted The text as received.
/// @param maxBytes The most UTF-8 bytes to keep.
/// @return The text, valid UTF-8 of at most @p maxBytes bytes.
[[nodiscard]] inline std::string sanitizeStatusText(std::string_view untrusted, std::size_t maxBytes = 200)
{
    auto result = std::string {};
    auto filter = vtbackend::UntrustedTextFilter {};
    while (!untrusted.empty())
    {
        auto const decoded = detail::decodeCodePoint(untrusted);
        auto const bytes = decoded.valid ? untrusted.substr(0, decoded.length) : ReplacementCharacter;
        untrusted.remove_prefix(decoded.length);
        auto const shown = filter.next(decoded.valid ? vtbackend::treatmentOf(decoded.codePoint)
                                                     : vtbackend::CodePointTreatment::Keep);
        if (shown == vtbackend::ShownAs::Nothing)
            continue;
        auto const separator =
            shown == vtbackend::ShownAs::AfterSpace ? std::string_view { " " } : std::string_view {};
        if (result.size() + separator.size() + bytes.size() > maxBytes)
            break;
        result += separator;
        result += bytes;
    }
    return result;
}

} // namespace vthost::control
