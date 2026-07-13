// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace contour::ascii
{

/// Locale-free ASCII character predicates, for text whose vocabulary the program itself fixes.
///
/// Action names and command titles are ASCII identifiers this codebase writes, so their case and word
/// boundaries are decided by the character alone. Deliberately NOT std::tolower / std::isupper (nor
/// crispy::toLower, which is built on them): those consult the C locale, so the same title could split
/// into different words — or match a filter differently — depending on where the terminal happens to
/// be running. These also stay constexpr and per-character, where crispy::toLower allocates a whole
/// string.

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

} // namespace contour::ascii
