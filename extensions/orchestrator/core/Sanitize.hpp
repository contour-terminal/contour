// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// Why singleLineNote refused a note.
enum class NoteError : uint8_t
{
    StartsWithDash, ///< The sanitised note starts with '-', so a command could read it as an option.
    TooLong,        ///< The input exceeded MaxNoteChars characters.
};

/// The most characters a note may have, counted on the input before sanitising.
inline constexpr std::size_t MaxNoteChars = 500;

/// Text safe to draw: no control characters, no direction overrides, one line, at most @p maxChars.
///
/// Drops every C0 and C1 control character and DEL (a TAB, LF, U+2028 or U+2029 becomes a space instead),
/// the bidirectional marks and overrides, and each byte of malformed UTF-8; collapses runs of spaces and
/// trims them; and cuts the result to @p maxChars characters (code points, not display columns), the last
/// of which is a '…' when it cut.
/// @param input Untrusted UTF-8, such as a pull request's title.
/// @param maxChars The most characters the result may have, the '…' included.
/// @return The sanitised text.
[[nodiscard]] std::string displayText(std::string_view input, std::size_t maxChars);

/// The most cells of indentation displayLines() keeps of a line.
inline constexpr std::size_t MaxIndentColumns = 40;

/// The cells displayLines() draws a leading TAB as.
inline constexpr std::size_t TabIndentColumns = 4;

/// Untrusted multi-line text as lines safe to draw.
///
/// Splits at LF (a CR LF ends a line as an LF does; a lone CR is dropped). A line's leading spaces are kept,
/// each leading TAB as TabIndentColumns spaces, at most MaxIndentColumns in all; the rest of it is
/// displayText(rest, maxChars). A line with nothing after its indentation is empty.
/// @param input Untrusted UTF-8, such as an agent's STATUS.md.
/// @param maxLines The most lines returned; when more follow, the last of them is a lone '…' instead.
/// @param maxChars The most characters of a line after its indentation, as displayText() counts them.
/// @return The lines, in order; a final LF ends the last line rather than starting an empty one.
[[nodiscard]] std::vector<std::string> displayLines(std::string_view input,
                                                    std::size_t maxLines,
                                                    std::size_t maxChars);

/// Whether a text holds a code point that draws as nothing: a zero-width space or joiner, a word joiner or
/// invisible operator, a byte-order mark, a soft hyphen, a combining grapheme joiner, the Mongolian vowel
/// separator, a variation selector, a tag character or a Hangul filler.
///
/// displayText() keeps them, since they are part of text people write (an emoji's ZWJ sequence). A text that
/// is shown so that the user can copy it and run it, such as a path, must not hold one: what is drawn would
/// read the same as another text that names something else.
/// @param text UTF-8; a malformed byte holds none.
/// @return Whether one of them is in @p text.
[[nodiscard]] bool holdsInvisible(std::string_view text) noexcept;

/// A user's note, sanitised into one line that is safe to pass as a single argument.
/// @param input The note as the user typed it.
/// @return displayText(input, MaxNoteChars), or why it was refused: the input exceeded MaxNoteChars
///         characters, or the result starts with '-'.
[[nodiscard]] std::expected<std::string, NoteError> singleLineNote(std::string_view input);

} // namespace orchestrator
