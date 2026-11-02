// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <libunicode/utf8.h>

#include <cstdint>
#include <optional>
#include <variant>

namespace vtparser
{

/// What the parser prints for a byte sequence that is not UTF-8.
inline constexpr auto ReplacementCharacter = char32_t { 0xFFFD };

/// Tells whether @p byte is a UTF-8 continuation byte (0x80..0xBF), the only kind that can continue a
/// pending character.
/// @param byte The byte to test.
/// @return Whether @p byte can continue a multi-byte sequence.
[[nodiscard]] constexpr bool isUtf8ContinuationByte(uint8_t byte) noexcept
{
    return (byte & 0xC0) == 0x80;
}

/// Tells whether @p state sits at a character boundary, with no multi-byte sequence pending.
///
/// Every shortcut that hands ASCII on without decoding it must ask this first. A decoder that has
/// seen a lead byte takes the next byte as a continuation byte, even an ASCII one, and folds it into
/// the pending character: fed to decodeUtf8Byte() alone, 0xC3 followed by 'a' decodes to U+00E1, and no 'a'
/// is printed. The Parser therefore ends a pending character itself, printing U+FFFD, before it feeds any
/// byte that is not a continuation byte; only an idle decoder treats an ASCII byte as the character it is.
/// @param state The decoder state to inspect.
/// @return Whether no character is pending in @p state.
[[nodiscard]] constexpr bool isDecoderIdle(unicode::utf8_decoder_state const& state) noexcept
{
    return state.expectedLength == 0;
}

/// Feeds one byte of UTF-8 text to @p state: the one decoding policy for text, shared by the state
/// machine, which prints what this returns (after the Parser has ended a pending character at a byte that
/// cannot continue it), and by every consumer of the bulk text path, which hands a
/// byte that is not UTF-8 on as it came, inside its run.
///
/// An ASCII byte reaching an idle decoder is returned as it is, without calling unicode::from_utf8:
/// that is from_utf8's own first branch, and the decoder stays idle. Every other byte, and every byte
/// of a pending character, goes through from_utf8. The shortcut exists because from_utf8 is out of
/// line in libunicode and this runs once per byte of every non-ASCII run of text.
/// @param state The decoder state, carried from byte to byte; idle at the start of a run.
/// @param byte  The next byte of text.
/// @return The codepoint @p byte completes: ReplacementCharacter for an invalid sequence, nothing while
///         a character is still incomplete.
[[nodiscard]] inline std::optional<char32_t> decodeUtf8Byte(unicode::utf8_decoder_state& state,
                                                            uint8_t byte) noexcept
{
    if (isDecoderIdle(state) && byte < 0x80)
        return char32_t { byte };
    auto const result = unicode::from_utf8(state, byte);
    if (std::holds_alternative<unicode::Incomplete>(result))
        return std::nullopt;
    if (auto const* const success = std::get_if<unicode::Success>(&result))
        return success->value;
    return ReplacementCharacter;
}

} // namespace vtparser
