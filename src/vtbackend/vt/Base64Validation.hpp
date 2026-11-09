// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// The standard base64 digits (RFC 4648 section 4), which core::base64::decode reads. Spelled here
/// rather than borrowed from the library's detail namespace, which a re-vendor may rename.
inline constexpr auto Base64Digits =
    std::string_view { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };

/// Checks that @p text is base64 that core::base64::decode reads whole: digits, then at most two `=` that
/// complete a quartet. Padding is optional, but a lone digit in the last quartet carries no byte and is
/// never valid. (The decoder itself silently stops at the first stray byte.)
/// @param text The candidate base64 text.
/// @return How many digits (padding not counted) it holds, if it is valid; so a caller can bound the
///         decoded size (`digits * 3 / 4`) without scanning again.
[[nodiscard]] inline std::optional<std::size_t> base64DigitCount(std::string_view text) noexcept
{
    auto const digits = text.substr(0, text.find_first_not_of(Base64Digits));
    auto const padding = text.substr(digits.size());
    if (padding.size() > 2 || !std::ranges::all_of(padding, [](char ch) { return ch == '='; }))
        return std::nullopt;
    if (digits.size() % 4 == 1)
        return std::nullopt;
    if (!padding.empty() && (digits.size() + padding.size()) % 4 != 0)
        return std::nullopt;
    return digits.size();
}

/// @param text The candidate base64 text.
/// @return Whether @p text is valid base64 (@see base64DigitCount).
[[nodiscard]] inline bool isValidBase64(std::string_view text) noexcept
{
    return base64DigitCount(text).has_value();
}

/// Drops every byte of @p text that is not a base64 digit: padding, whitespace and any stray byte. That is
/// how xterm reads OSC 52's _Pd_ (button.c, AppendToSelectionBuffer), skipping such a byte where
/// core::base64::decode stops at it.
/// @param text The candidate base64 text.
/// @return The digits of @p text, in order, which core::base64::decode reads whole.
[[nodiscard]] inline std::string base64DigitsOf(std::string_view text)
{
    auto digits = std::string {};
    digits.reserve(text.size());
    std::ranges::copy_if(text, std::back_inserter(digits), [](char ch) { return Base64Digits.contains(ch); });
    return digits;
}

} // namespace vtbackend
