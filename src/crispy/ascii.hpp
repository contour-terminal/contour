// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace crispy::ascii
{

/// Locale-free ASCII character predicates, for text whose vocabulary the program itself fixes.
///
/// Identifiers a codebase writes down itself — action names, command titles, config keys — have their
/// case and word boundaries decided by the character alone. Deliberately NOT std::tolower /
/// std::isupper (nor crispy::toLower, which is built on them): those consult the C locale, so the
/// same title could split into different words — or match a filter differently — depending on where
/// the program happens to be running. These also stay constexpr and per-character, where
/// crispy::toLower allocates a whole string.

[[nodiscard]] constexpr bool isUpper(char ch) noexcept
{
    return 'A' <= ch && ch <= 'Z';
}

[[nodiscard]] constexpr bool isLower(char ch) noexcept
{
    return 'a' <= ch && ch <= 'z';
}

[[nodiscard]] constexpr bool isDigit(char ch) noexcept
{
    return '0' <= ch && ch <= '9';
}

/// A letter or a digit — i.e. not a separator. Used to find word boundaries.
[[nodiscard]] constexpr bool isWordCharacter(char ch) noexcept
{
    return isLower(ch) || isUpper(ch) || isDigit(ch);
}

/// @return @p ch lower-cased, for case-insensitive comparison.
[[nodiscard]] constexpr char fold(char ch) noexcept
{
    return isUpper(ch) ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// @return @p ch upper-cased, for callers that must normalize onto the upper case.
///
/// Codepoint-wide, and deliberately still US-ASCII only: a caller normalizing text it did not author
/// must not fold the rest of Unicode along with it, because simple case mapping is not
/// locale-neutral there — Turkish dotless 'ı' upper-cases onto 'I', colliding with 'i', and MICRO
/// SIGN 'µ' becomes GREEK CAPITAL MU. @see config::foldedBindingCodepoint for the one caller.
[[nodiscard]] constexpr char32_t foldUpper(char32_t ch) noexcept
{
    return U'a' <= ch && ch <= U'z' ? ch - (U'a' - U'A') : ch;
}

} // namespace crispy::ascii
