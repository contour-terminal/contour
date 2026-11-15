// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

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

/// A user's note, sanitised into one line that is safe to pass as a single argument.
/// @param input The note as the user typed it.
/// @return displayText(input, MaxNoteChars), or why it was refused: the input exceeded MaxNoteChars
///         characters, or the result starts with '-'.
[[nodiscard]] std::expected<std::string, NoteError> singleLineNote(std::string_view input);

} // namespace orchestrator
