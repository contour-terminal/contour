// SPDX-License-Identifier: Apache-2.0
//
// ASCII-only case folding, the fold Windows path and environment-name comparisons use here. Every byte
// outside `A`-`Z` and `a`-`z`, the UTF-8 ones among them, is left as it is, so a non-ASCII case difference
// never matches: a name is refused rather than admitted by a folding Windows may not share.
#pragma once

#include <algorithm>
#include <functional>
#include <string_view>

namespace vtpty
{

/// @param c A byte.
/// @return Whether @p c is an ASCII letter, the drive letters of a Windows path among them.
[[nodiscard]] constexpr bool isAsciiLetter(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// @param c A byte.
/// @return @p c upper-cased if it is an ASCII lower-case letter; as it is otherwise.
[[nodiscard]] constexpr char asciiUpper(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

/// @param c A byte.
/// @return @p c lower-cased if it is an ASCII upper-case letter; as it is otherwise.
[[nodiscard]] constexpr char asciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// @param text One text, UTF-8.
/// @param other The other text, UTF-8.
/// @return Whether @p text and @p other are equal once their ASCII letters are folded.
[[nodiscard]] constexpr bool equalsIgnoringAsciiCase(std::string_view text, std::string_view other) noexcept
{
    return std::ranges::equal(text, other, std::ranges::equal_to {}, asciiLower, asciiLower);
}

} // namespace vtpty
