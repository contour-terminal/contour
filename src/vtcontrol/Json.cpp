// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Json.hpp>

#include <cstdint>

namespace vtcontrol
{

namespace
{
    constexpr auto ReplacementCharacter = std::string_view { "\xEF\xBF\xBD" };

    /// @return The length of the well-formed UTF-8 sequence at the start of `text`, or 0.
    [[nodiscard]] std::size_t validSequenceLength(std::string_view text) noexcept
    {
        auto const lead = static_cast<uint8_t>(text.front());
        auto const isContinuation = [&](std::size_t index) noexcept {
            return index < text.size() && (static_cast<uint8_t>(text[index]) & 0xC0) == 0x80;
        };
        if (lead < 0x80)
            return 1;
        if (lead >= 0xC2 && lead <= 0xDF)
            return isContinuation(1) ? 2 : 0;
        if (lead >= 0xE0 && lead <= 0xEF)
        {
            if (!isContinuation(1) || !isContinuation(2))
                return 0;
            auto const second = static_cast<uint8_t>(text[1]);
            if (lead == 0xE0 && second < 0xA0) // overlong
                return 0;
            if (lead == 0xED && second > 0x9F) // surrogate
                return 0;
            return 3;
        }
        if (lead >= 0xF0 && lead <= 0xF4)
        {
            if (!isContinuation(1) || !isContinuation(2) || !isContinuation(3))
                return 0;
            auto const second = static_cast<uint8_t>(text[1]);
            if (lead == 0xF0 && second < 0x90) // overlong
                return 0;
            if (lead == 0xF4 && second > 0x8F) // beyond U+10FFFF
                return 0;
            return 4;
        }
        return 0;
    }
} // namespace

std::string toValidUtf8(std::string_view text)
{
    auto result = std::string {};
    result.reserve(text.size());
    while (!text.empty())
    {
        auto const length = validSequenceLength(text);
        if (length == 0)
        {
            result.append(ReplacementCharacter);
            text.remove_prefix(1);
        }
        else
        {
            result.append(text.substr(0, length));
            text.remove_prefix(length);
        }
    }
    return result;
}

std::string quoteJsonString(std::string_view text)
{
    constexpr auto HexDigits = std::string_view { "0123456789abcdef" };
    auto const valid = toValidUtf8(text);
    auto result = std::string {};
    result.reserve(valid.size() + 2);
    result.push_back('"');
    for (auto const ch: valid)
    {
        auto const byte = static_cast<uint8_t>(ch);
        switch (ch)
        {
            case '"': result.append("\\\""); break;
            case '\\': result.append("\\\\"); break;
            case '\b': result.append("\\b"); break;
            case '\f': result.append("\\f"); break;
            case '\n': result.append("\\n"); break;
            case '\r': result.append("\\r"); break;
            case '\t': result.append("\\t"); break;
            default:
                if (byte < 0x20 || byte == 0x7F)
                {
                    result.append("\\u00");
                    result.push_back(HexDigits[byte >> 4]);
                    result.push_back(HexDigits[byte & 0x0F]);
                }
                else
                    result.push_back(ch);
                break;
        }
    }
    result.push_back('"');
    return result;
}

} // namespace vtcontrol
