// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/RcText.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <utility>

// A small local codec rather than core-cpp's: core::toUtf16()/toUtf8() are Windows-only, and a profile
// is read the same way on every platform Contour's tests run on.

namespace contour::platform
{

namespace
{
    /// Byte order marks that name an encoding no shell profile is written in. UTF-32LE's mark begins
    /// with UTF-16LE's, so this table is consulted first.
    constexpr auto UnsupportedMarks = std::array<std::string_view, 3> {
        std::string_view { "\xFF\xFE\x00\x00", 4 }, // UTF-32LE
        std::string_view { "\x00\x00\xFE\xFF", 4 }, // UTF-32BE
        std::string_view { "\xFE\xFF", 2 },         // UTF-16BE
    };

    constexpr char32_t HighSurrogateFirst = 0xD800;
    constexpr char32_t LowSurrogateFirst = 0xDC00;
    constexpr char32_t SurrogateLast = 0xDFFF;
    constexpr char32_t SupplementaryFirst = 0x10000;

    [[nodiscard]] constexpr bool isHighSurrogate(char32_t unit) noexcept
    {
        return unit >= HighSurrogateFirst && unit < LowSurrogateFirst;
    }

    [[nodiscard]] constexpr bool isLowSurrogate(char32_t unit) noexcept
    {
        return unit >= LowSurrogateFirst && unit <= SurrogateLast;
    }

    /// Appends @p codePoint to @p out in UTF-8.
    void appendUtf8(std::string& out, char32_t codePoint)
    {
        auto const put = [&](char32_t value) {
            out.push_back(static_cast<char>(value));
        };
        if (codePoint < 0x80)
            put(codePoint);
        else if (codePoint < 0x800)
        {
            put(0xC0 | (codePoint >> 6));
            put(0x80 | (codePoint & 0x3F));
        }
        else if (codePoint < SupplementaryFirst)
        {
            put(0xE0 | (codePoint >> 12));
            put(0x80 | ((codePoint >> 6) & 0x3F));
            put(0x80 | (codePoint & 0x3F));
        }
        else
        {
            put(0xF0 | (codePoint >> 18));
            put(0x80 | ((codePoint >> 12) & 0x3F));
            put(0x80 | ((codePoint >> 6) & 0x3F));
            put(0x80 | (codePoint & 0x3F));
        }
    }

    /// Appends the UTF-16 code unit @p unit to @p out, little endian.
    void appendUtf16Le(std::string& out, char32_t unit)
    {
        out.push_back(static_cast<char>(unit & 0xFF));
        out.push_back(static_cast<char>(unit >> 8));
    }

    /// UTF-16LE (without its mark) to UTF-8.
    [[nodiscard]] std::expected<std::string, RcTextError> utf16LeToUtf8(std::string_view bytes)
    {
        if (bytes.size() % 2 != 0)
            return std::unexpected(RcTextError::Malformed);
        auto out = std::string {};
        out.reserve(bytes.size());
        auto pendingHigh = char32_t { 0 };
        // Indexed rather than std::views::chunk(2), which the libc++ that macOS ships lacks.
        for (auto const index: std::views::iota(0uz, bytes.size() / 2))
        {
            auto const unit =
                static_cast<char32_t>(static_cast<unsigned char>(bytes[2 * index]))
                | (static_cast<char32_t>(static_cast<unsigned char>(bytes[(2 * index) + 1])) << 8);
            if (pendingHigh != 0)
            {
                if (!isLowSurrogate(unit))
                    return std::unexpected(RcTextError::Malformed);
                appendUtf8(out,
                           SupplementaryFirst + ((pendingHigh - HighSurrogateFirst) << 10)
                               + (unit - LowSurrogateFirst));
                pendingHigh = 0;
            }
            else if (isHighSurrogate(unit))
                pendingHigh = unit;
            else if (isLowSurrogate(unit))
                return std::unexpected(RcTextError::Malformed);
            else
                appendUtf8(out, unit);
        }
        if (pendingHigh != 0)
            return std::unexpected(RcTextError::Malformed);
        return out;
    }

    /// A UTF-8 sequence's length, and which bits of its lead byte carry the code point.
    struct LeadByte
    {
        std::size_t length = 1;
        unsigned payload = 0x7F;
    };

    /// What the lead byte @p lead says about the UTF-8 sequence it opens.
    [[nodiscard]] constexpr LeadByte leadOf(unsigned char lead) noexcept
    {
        if (lead < 0x80)
            return { .length = 1, .payload = 0x7F };
        if (lead < 0xE0)
            return { .length = 2, .payload = 0x1F };
        if (lead < 0xF0)
            return { .length = 3, .payload = 0x0F };
        return { .length = 4, .payload = 0x07 };
    }

    /// UTF-8 to UTF-16LE (without a mark). The input is valid UTF-8 by precondition, so only the lead
    /// byte's length is read.
    [[nodiscard]] std::string utf8ToUtf16Le(std::string_view utf8)
    {
        auto out = std::string {};
        out.reserve(utf8.size() * 2);
        auto rest = utf8;
        while (!rest.empty())
        {
            auto const [length, payload] = leadOf(static_cast<unsigned char>(rest.front()));
            auto codePoint = static_cast<char32_t>(static_cast<unsigned char>(rest.front()) & payload);
            for (auto const continuation: rest.substr(1, length - 1))
                codePoint = (codePoint << 6) | (static_cast<unsigned char>(continuation) & 0x3FU);
            rest.remove_prefix(std::min(length, rest.size()));

            if (codePoint < SupplementaryFirst)
                appendUtf16Le(out, codePoint);
            else
            {
                auto const offset = codePoint - SupplementaryFirst;
                appendUtf16Le(out, HighSurrogateFirst + (offset >> 10));
                appendUtf16Le(out, LowSurrogateFirst + (offset & 0x3FF));
            }
        }
        return out;
    }
} // namespace

std::expected<RcText, RcTextError> decodeRcText(std::string_view bytes)
{
    if (std::ranges::any_of(UnsupportedMarks, [&](auto mark) { return bytes.starts_with(mark); }))
        return std::unexpected(RcTextError::UnsupportedEncoding);

    if (auto const mark = byteOrderMark(RcEncoding::Utf16LeBom); bytes.starts_with(mark))
        return utf16LeToUtf8(bytes.substr(mark.size())).transform([](std::string utf8) {
            return RcText { .encoding = RcEncoding::Utf16LeBom, .utf8 = std::move(utf8) };
        });

    if (auto const mark = byteOrderMark(RcEncoding::Utf8Bom); bytes.starts_with(mark))
        return RcText { .encoding = RcEncoding::Utf8Bom, .utf8 = std::string(bytes.substr(mark.size())) };

    return RcText { .encoding = RcEncoding::Utf8, .utf8 = std::string(bytes) };
}

std::string encodeRcText(std::string_view utf8, RcEncoding encoding)
{
    switch (encoding)
    {
        case RcEncoding::Utf8:
        case RcEncoding::Utf8Bom: return std::string(utf8);
        case RcEncoding::Utf16LeBom: return utf8ToUtf16Le(utf8);
    }
    return std::string(utf8);
}

} // namespace contour::platform
