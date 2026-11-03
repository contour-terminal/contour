// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <libunicode/utf8.h>

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace vtparser
{

/// What the parser prints for a byte sequence that is not UTF-8.
inline constexpr auto ReplacementCharacter = char32_t { 0xFFFD };

/// Tells whether @p state sits at a character boundary, with no multi-byte sequence pending.
///
/// Every shortcut that hands ASCII on without decoding it must ask this first: after a lead byte the
/// decoder ends the pending character with U+FFFD before an ASCII byte, which a shortcut would skip.
/// @param state The decoder state to inspect.
/// @return Whether no character is pending in @p state.
[[nodiscard]] constexpr bool isDecoderIdle(unicode::utf8_decoder_state const& state) noexcept
{
    return state.expectedLength == 0;
}

/// Tells whether the pending character of @p state can go on with @p byte. An idle decoder has nothing to
/// continue, so every byte it is asked about can.
/// @param state The decoder state.
/// @param byte  The next byte.
/// @return Whether libunicode's strict decoder takes @p byte as the next byte of the pending character.
[[nodiscard]] inline bool canContinueUtf8(unicode::utf8_decoder_state const& state, uint8_t byte) noexcept
{
    auto probe = state;
    return !std::holds_alternative<unicode::Invalid>(unicode::from_utf8(probe, byte));
}

/// Whether a codepoint decodeUtf8ByteTagged() settled was in the text, or stands in for ill-formed bytes.
enum class Utf8Outcome : uint8_t
{
    Decoded, ///< Decoded from well-formed UTF-8: a literal U+FFFD is this, and is a character like any other.
    Replaced, ///< ReplacementCharacter for one maximal subpart of ill-formed UTF-8.
};

/// Feeds one byte of UTF-8 text to @p state: the one decoding policy for text, shared by the state
/// machine and by every consumer of the bulk text path, which hands a byte that is not UTF-8 on as it
/// came, inside its run.
///
/// libunicode decodes well-formed UTF-8 only (Unicode's Table 3-7): overlong forms, surrogates and values
/// above U+10FFFF are never decoded. Text is replaced as Unicode asks for ill-formed UTF-8, one U+FFFD for
/// each maximal subpart. A pending character ends at the first byte that cannot continue it; that byte is
/// then decoded afresh, so 0xE0 0x80 0x41 gives two U+FFFD and an 'A'. libunicode consumes such a byte
/// only when it starts the next character (C2..F4); any other is fed again here, from an idle decoder.
///
/// An ASCII byte reaching an idle decoder is emitted as it is, without calling unicode::from_utf8:
/// that is from_utf8's own first branch, and the decoder stays idle. The shortcut exists because
/// from_utf8 is out of line in libunicode and this runs once per byte of every non-ASCII run of text.
/// @param state The decoder state, carried from byte to byte; idle at the start of a run.
/// @param byte  The next byte of text.
/// @param sink  Called with each codepoint @p byte settles and whether it was decoded or replaced -- none
///              while a character is still incomplete, up to two when it ends an ill-formed one:
///              ReplacementCharacter, and its own.
template <std::invocable<char32_t, Utf8Outcome> Sink>
void decodeUtf8ByteTagged(unicode::utf8_decoder_state& state, uint8_t byte, Sink const& sink) noexcept(
    std::is_nothrow_invocable_v<Sink const&, char32_t, Utf8Outcome>)
{
    // One call of @p sink, in one place, so that a large sink is inlined once and not once per outcome; and a
    // loop, not recursion, so that the whole function can be inlined into the loop that feeds it. (An ASCII
    // fast path with a call of its own makes the sink inline twice, which costs the utf8 workload 3.7%.)
    for (;;)
    {
        auto const expectedBefore = state.expectedLength;
        auto codepoint = char32_t { byte };
        auto outcome = Utf8Outcome::Decoded;
        auto feedAgain = false;
        if (expectedBefore != 0 || byte >= 0x80)
        {
            auto const result = unicode::from_utf8(state, byte);
            if (std::holds_alternative<unicode::Incomplete>(result))
                return;
            if (auto const* const success = std::get_if<unicode::Success>(&result))
                codepoint = success->value;
            else
            {
                codepoint = ReplacementCharacter;
                outcome = Utf8Outcome::Replaced;
                // Inside a character, a byte that does not start the next one was not consumed.
                feedAgain = unicode::from_utf8_left_byte_unconsumed(expectedBefore, state);
            }
        }
        sink(codepoint, outcome);
        if (!feedAgain)
            return;
    }
}

/// decodeUtf8ByteTagged() for a sink that takes the codepoints alone, a replacement included.
/// @param state The decoder state, carried from byte to byte; idle at the start of a run.
/// @param byte  The next byte of text.
/// @param sink  Called with each codepoint @p byte settles.
template <std::invocable<char32_t> Sink>
void decodeUtf8Byte(unicode::utf8_decoder_state& state,
                    uint8_t byte,
                    Sink const& sink) noexcept(std::is_nothrow_invocable_v<Sink const&, char32_t>)
{
    decodeUtf8ByteTagged(state, byte, [&sink](char32_t codepoint, Utf8Outcome) { sink(codepoint); });
}

} // namespace vtparser
