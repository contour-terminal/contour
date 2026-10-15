// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/JsonString.hpp>

#include <vtparser/Utf8Decoding.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>

namespace vtbackend
{

namespace
{
    /// A code point JSON spells with a short escape of its own.
    struct ShortEscape
    {
        char32_t codePoint;    ///< The code point.
        std::string_view text; ///< How JSON spells it.
    };

    /// Every code point JSON spells with a short escape (RFC 8259 §7).
    constexpr auto ShortEscapes = std::array {
        ShortEscape { .codePoint = U'"', .text = R"(\")" },
        ShortEscape { .codePoint = U'\\', .text = R"(\\)" },
        ShortEscape { .codePoint = U'\b', .text = R"(\b)" },
        ShortEscape { .codePoint = U'\f', .text = R"(\f)" },
        ShortEscape { .codePoint = U'\n', .text = R"(\n)" },
        ShortEscape { .codePoint = U'\r', .text = R"(\r)" },
        ShortEscape { .codePoint = U'\t', .text = R"(\t)" },
    };

    /// Whether @p codePoint is a control code: C0, DEL or C1.
    [[nodiscard]] constexpr bool isControlCode(char32_t codePoint) noexcept
    {
        return codePoint < 0x20 || (0x7F <= codePoint && codePoint <= 0x9F);
    }

    /// Appends @p codePoint to @p json as a JSON string spells it.
    void appendCodePoint(std::string& json, char32_t codePoint)
    {
        if (auto const escape = std::ranges::find(ShortEscapes, codePoint, &ShortEscape::codePoint);
            escape != ShortEscapes.end())
            json += escape->text;
        else if (isControlCode(codePoint))
            json += std::format("\\u{:04x}", static_cast<uint32_t>(codePoint));
        else
            unicode::encoder<char> {}(codePoint, std::back_inserter(json));
    }
} // namespace

std::string jsonString(std::string_view text)
{
    auto json = std::string { '"' };
    json.reserve(text.size() + 2);
    auto decoder = unicode::utf8_decoder_state {};
    for (auto const ch: text)
        if (auto const codePoint = vtparser::decodeUtf8Byte(decoder, static_cast<uint8_t>(ch)))
            appendCodePoint(json, *codePoint);
    json += '"';
    return json;
}

} // namespace vtbackend
