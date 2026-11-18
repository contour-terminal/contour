// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A glyph written a number of times: the rules of the frames and dividers the host draws itself.

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>

namespace vthost
{

/// @param glyph The text to repeat, UTF-8.
/// @param count How many times.
/// @return @p glyph written @p count times.
[[nodiscard]] inline std::string repeatedText(std::string_view glyph, std::size_t count)
{
    return std::ranges::to<std::string>(std::views::repeat(glyph, count) | std::views::join);
}

} // namespace vthost
