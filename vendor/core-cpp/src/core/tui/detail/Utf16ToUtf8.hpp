// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// UTF-16 code units, one at a time, to UTF-8: what the Windows console hands over and what the
/// VT parser reads.
///
/// **It has state, because the input does.** A console delivers a character outside the BMP -- an
/// emoji, U+1F600 -- as TWO key events, one per surrogate, and they can arrive in two different
/// reads. endo's `appendUtf16AsUtf8` encoded each code unit on its own, so a surrogate pair became
/// two three-byte sequences (CESU-8), which is not UTF-8 and which the parser then decoded back to
/// two lone surrogates (core-cpp#20). Pairing needs the high surrogate kept until the low one comes.
///
/// Platform-neutral on purpose, although only the Windows arm feeds it: the pairing is arithmetic,
/// and a test of it should run wherever the suite does.

#include <cstdint>
#include <string>

namespace core::tui::detail
{

/// Converts a stream of UTF-16 code units to UTF-8, pairing surrogates across calls.
///
/// A surrogate that cannot be paired -- a low one with no high one before it, or a high one
/// followed by anything but a low one -- becomes U+FFFD, the replacement character, so the output
/// is always valid UTF-8.
class Utf16ToUtf8
{
  public:
    /// The code point a surrogate that cannot be paired becomes.
    static constexpr char32_t Replacement = 0xFFFD;

    /// Appends @p unit to @p output as UTF-8, or holds it if it is a high surrogate.
    /// @param output The string to append to.
    /// @param unit The next UTF-16 code unit.
    void append(std::string& output, char16_t unit)
    {
        if (isLowSurrogate(unit))
        {
            if (_pendingHigh == 0)
            {
                appendCodepoint(output, Replacement);
                return;
            }
            auto const high = static_cast<char32_t>(_pendingHigh - 0xD800);
            auto const low = static_cast<char32_t>(unit - 0xDC00);
            _pendingHigh = 0;
            appendCodepoint(output, 0x10000 + ((high << 10) | low));
            return;
        }

        if (_pendingHigh != 0)
        {
            _pendingHigh = 0;
            appendCodepoint(output, Replacement);
        }

        if (isHighSurrogate(unit))
            _pendingHigh = unit;
        else
            appendCodepoint(output, unit);
    }

    /// @return Whether a high surrogate is held, waiting for its low half.
    [[nodiscard]] bool pending() const noexcept { return _pendingHigh != 0; }

  private:
    [[nodiscard]] static constexpr bool isHighSurrogate(char16_t unit) noexcept
    {
        return unit >= 0xD800 && unit <= 0xDBFF;
    }

    [[nodiscard]] static constexpr bool isLowSurrogate(char16_t unit) noexcept
    {
        return unit >= 0xDC00 && unit <= 0xDFFF;
    }

    static void appendCodepoint(std::string& output, char32_t cp)
    {
        if (cp < 0x80)
        {
            output += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            output += static_cast<char>(0xC0 | (cp >> 6));
            output += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            output += static_cast<char>(0xE0 | (cp >> 12));
            output += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            output += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            output += static_cast<char>(0xF0 | (cp >> 18));
            output += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            output += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            output += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    char16_t _pendingHigh = 0; ///< A high surrogate waiting for its low half, or 0.
};

} // namespace core::tui::detail
