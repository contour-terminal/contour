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

/// Feeds one byte of UTF-8 text to @p state: the one decoding policy for text, shared by the state
/// machine, which prints what this returns, and by every consumer of the bulk text path, which hands a
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
    if (state.expectedLength == 0 && byte < 0x80)
        return char32_t { byte };
    auto const result = unicode::from_utf8(state, byte);
    if (std::holds_alternative<unicode::Incomplete>(result))
        return std::nullopt;
    if (auto const* const success = std::get_if<unicode::Success>(&result))
        return success->value;
    return ReplacementCharacter;
}

} // namespace vtparser
