// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The character classes of RFC 9110 and RFC 3986 as one table, and the small predicates over it
/// that the message factories and the parsers share, and how they spell a refusal. Internal: in no
/// `FILE_SET`.

#include <core/http/Message.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace core::http::detail
{

/// One bit per character class an octet may belong to.
struct CharClass
{
    static constexpr std::uint8_t Token = 0b0000'0001;      ///< tchar (RFC 9110 section 5.6.2).
    static constexpr std::uint8_t FieldValue = 0b0000'0010; ///< field-vchar, SP, HTAB (section 5.5).
    static constexpr std::uint8_t Hex = 0b0000'0100;        ///< HEXDIG.
    static constexpr std::uint8_t Unreserved = 0b0000'1000; ///< unreserved (RFC 3986 section 2.3).
    static constexpr std::uint8_t SubDelim = 0b0001'0000;   ///< sub-delims (RFC 3986 section 2.2).
    static constexpr std::uint8_t Whitespace = 0b0010'0000; ///< SP, HTAB.
    static constexpr std::uint8_t Digit = 0b0100'0000;      ///< DIGIT.
    static constexpr std::uint8_t Alpha = 0b1000'0000;      ///< ALPHA.
};

/// The classes of every octet.
inline constexpr auto CharClasses = [] {
    auto table = std::array<std::uint8_t, 256> {};
    auto const mark = [&table](std::string_view octets, std::uint8_t bits) {
        for (auto const octet: octets)
            table.at(static_cast<std::uint8_t>(octet)) |= bits;
    };
    constexpr auto Digits = std::string_view { "0123456789" };
    constexpr auto Lower = std::string_view { "abcdefghijklmnopqrstuvwxyz" };
    constexpr auto Upper = std::string_view { "ABCDEFGHIJKLMNOPQRSTUVWXYZ" };
    mark(Digits, CharClass::Token | CharClass::Hex | CharClass::Unreserved | CharClass::Digit);
    mark(Lower, CharClass::Token | CharClass::Unreserved | CharClass::Alpha);
    mark(Upper, CharClass::Token | CharClass::Unreserved | CharClass::Alpha);
    mark("abcdefABCDEF", CharClass::Hex);
    mark("!#$%&'*+-.^_`|~", CharClass::Token);
    mark("-._~", CharClass::Unreserved);
    mark("!$&'()*+,;=", CharClass::SubDelim);
    mark(" \t", CharClass::Whitespace | CharClass::FieldValue);
    for (auto const octet: std::views::iota(0x21, 0x7f))
        table.at(static_cast<std::size_t>(octet)) |= CharClass::FieldValue; // VCHAR
    for (auto const octet: std::views::iota(0x80, 0x100))
        table.at(static_cast<std::size_t>(octet)) |= CharClass::FieldValue; // obs-text
    return table;
}();

/// Whether @p octet is in any of @p classes.
[[nodiscard]] constexpr bool isIn(char octet, std::uint8_t classes) noexcept
{
    return (CharClasses.at(static_cast<std::uint8_t>(octet)) & classes) != 0;
}

/// Whether every octet of @p text is in one of @p classes.
[[nodiscard]] constexpr bool allIn(std::string_view text, std::uint8_t classes) noexcept
{
    return std::ranges::all_of(text, [classes](char octet) { return isIn(octet, classes); });
}

/// Whether @p text is a token: one or more tchar.
[[nodiscard]] constexpr bool isToken(std::string_view text) noexcept
{
    return !text.empty() && allIn(text, CharClass::Token);
}

/// The lower-case of an ASCII letter, any other octet unchanged.
[[nodiscard]] constexpr char toLowerAscii(char octet) noexcept
{
    return octet >= 'A' && octet <= 'Z' ? static_cast<char>(octet - 'A' + 'a') : octet;
}

/// Whether @p a and @p b are equal ignoring ASCII case.
[[nodiscard]] constexpr bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, [](char x, char y) { return toLowerAscii(x) == toLowerAscii(y); });
}

/// @p text without leading and trailing SP and HTAB (OWS).
[[nodiscard]] constexpr std::string_view trimWhitespace(std::string_view text) noexcept
{
    auto const isSpace = [](char octet) {
        return octet == ' ' || octet == '\t';
    };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

/// Whether @p text is URI characters only, in @p classes or in @p extra, with every '%' starting a
/// two-hex-digit escape (RFC 3986 section 2.1).
[[nodiscard]] constexpr bool isUriText(std::string_view text,
                                       std::uint8_t classes,
                                       std::string_view extra) noexcept
{
    while (!text.empty())
    {
        auto const octet = text.front();
        if (octet == '%')
        {
            if (text.size() < 3 || !isIn(text[1], CharClass::Hex) || !isIn(text[2], CharClass::Hex))
                return false;
            text.remove_prefix(3);
            continue;
        }
        if (!isIn(octet, classes) && !extra.contains(octet))
            return false;
        text.remove_prefix(1);
    }
    return true;
}

/// The value of @p digits, one or more decimal digits; empty if it is not that or exceeds 64 bits.
[[nodiscard]] constexpr std::optional<std::uint64_t> parseDecimal(std::string_view digits) noexcept
{
    if (digits.empty())
        return std::nullopt;
    constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    auto value = std::uint64_t { 0 };
    for (auto const octet: digits)
    {
        if (!isIn(octet, CharClass::Digit))
            return std::nullopt;
        auto const digit = static_cast<std::uint64_t>(octet - '0');
        if (value > (Max - digit) / 10)
            return std::nullopt;
        value = (value * 10) + digit;
    }
    return value;
}

/// Whether a port must have digits.
enum class PortRule : std::uint8_t
{
    Optional, ///< `host`, `host:` and `host:80` (a Host field, RFC 9110 section 7.2).
    Required, ///< `host:80` only (authority-form, RFC 9112 section 3.2.3).
};

/// Whether @p text is `uri-host [ ":" port ]` (RFC 3986 section 3.2): an IP literal in brackets or a
/// reg-name (which covers IPv4), then a port of digits as @p rule demands.
[[nodiscard]] constexpr bool isAuthority(std::string_view text, PortRule rule) noexcept
{
    auto host = text;
    auto port = std::string_view {};
    auto hasPort = false;
    if (text.starts_with('['))
    {
        auto const close = text.find(']');
        if (close == std::string_view::npos || close == 1)
            return false;
        host = text.substr(1, close - 1);
        auto const rest = text.substr(close + 1);
        if (!rest.empty() && !rest.starts_with(':'))
            return false;
        hasPort = !rest.empty();
        port = hasPort ? rest.substr(1) : rest;
        if (!isUriText(host, CharClass::Unreserved | CharClass::SubDelim, ":"))
            return false;
    }
    else
    {
        auto const colon = text.find(':');
        hasPort = colon != std::string_view::npos;
        host = text.substr(0, colon);
        port = hasPort ? text.substr(colon + 1) : std::string_view {};
        if (!isUriText(host, CharClass::Unreserved | CharClass::SubDelim, ""))
            return false;
    }
    if (!allIn(port, CharClass::Digit))
        return false;
    return rule == PortRule::Optional || (hasPort && !port.empty() && !host.empty());
}

/// A refusal of the caller's or the peer's syntax, framing or size.
/// @param code What failed.
/// @param detail Why.
/// @param fieldName The field it is about, if any.
/// @param size The size past a limit, if any.
/// @param reason What the refusal was for, beyond the code.
[[nodiscard]] inline HttpError refusal(HttpErrorCode code,
                                       std::string detail,
                                       std::optional<std::string> fieldName = std::nullopt,
                                       std::optional<std::uint64_t> size = std::nullopt,
                                       RefusalReason reason = RefusalReason::Unclassified)
{
    return HttpError { .code = code,
                       .size = size,
                       .fieldName = std::move(fieldName),
                       .dnsRcode = std::nullopt,
                       .dnsInfoCode = std::nullopt,
                       .coding = std::nullopt,
                       .tls = std::nullopt,
                       .detail = std::move(detail),
                       .reason = reason };
}

} // namespace core::http::detail
