// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// What an OSC 133 prompt mark starts, by its `k=` parameter (the semantic-prompts "kind").
enum class PromptKind : uint8_t
{
    Initial = 0,  ///< `k=i`, or no `k=` at all: the prompt itself.
    Continuation, ///< `k=c` or `k=s`: a further line of the prompt in progress (PS2, reedline).
    Right,        ///< `k=r`: a right-hand prompt drawn beside the one in progress.
};

/// Reads the prompt kind from an OSC 133 mark's parameters.
/// @param parameters Everything after the mark's letter, e.g. ";k=r"; empty for a bare mark.
/// @return The kind; Initial when there is no `k=`, or one this terminal does not know.
[[nodiscard]] PromptKind promptKindOf(std::string_view parameters) noexcept;

/// Whether the shell redraws its prompt when the window is resized, as an OSC 133;A says it (kitty's
/// `redraw=`).
enum class PromptRedraw : uint8_t
{
    OnResize = 0, ///< No `redraw=0`: the shell redraws the prompt at the new size.
    Never,        ///< `redraw=0`: the shell leaves the prompt as it drew it.
};

/// Reads whether the shell redraws its prompt on a resize from an OSC 133 mark's parameters.
/// @param parameters Everything after the mark's letter, e.g. ";redraw=0"; empty for a bare mark.
/// @return Never for `redraw=0`, the one value kitty reads (parse_prompt_mark); OnResize otherwise.
[[nodiscard]] PromptRedraw promptRedrawOf(std::string_view parameters) noexcept;

/// Why shell-quoted text could not be read.
enum class ShellQuotingError : uint8_t
{
    TrailingBackslash, ///< A backslash with nothing after it.
    UnterminatedQuote, ///< A quote that is never closed.
    IncompleteEscape,  ///< `\x`, `\u` or `\U` with no hex digit after it, or `\c` with nothing after it.
    NotACodepoint,     ///< A `\u` or `\U` naming a UTF-16 surrogate.
};

/// The first word of shell-quoted @p text, unquoted: what kitty's `shlex_split(text, True)` yields first.
///
/// That is how kitty reads the `cmdline=` of an OSC 133;C, which its shell integrations fill with the
/// command line quoted by `printf %q`: one word, every blank and special character in it escaped. So the
/// rules are kitty's, quirks included:
/// - Leading blanks (space, tab, CR, LF) are skipped; the word ends at the next unquoted one.
/// - A backslash outside quotes, and inside double quotes, takes the next character as it is.
/// - Single quotes take everything up to the next one as it is.
/// - `$'...'` is ANSI-C quoted: `\a \b \e \E \f \n \r \t \v \\ \' \" \?`, `\cX` (X's low five bits), octal
///   `\NNN` (one to three digits), hex `\xHH`, and `\uHHHH` / `\UHHHHHHHH` (one digit and up) as a code
///   point -- an octal or hex value too, encoded as UTF-8 -- and any other escaped character as itself. A
///   code point past U+10FFFF is dropped.
/// - Quotes that close with nothing between them still make a word, the empty one.
/// @param text The quoted text.
/// @return The word; empty when @p text holds none, or why it could not be read.
[[nodiscard]] std::expected<std::string, ShellQuotingError> firstShellWord(std::string_view text);

/// The command line an OSC 133;C reports, if it reports a non-empty one.
///
/// Read from the first parameter that is one of:
/// - `cmdline_url=`, percent-encoded (FinalTerm, WezTerm, Contour);
/// - `cmdline=`, quoted for a shell, as kitty's integrations send it (`printf %q`), and read as kitty reads
/// it:
///   firstShellWord().
/// Its value runs to the end of the sequence, as kitty's decode_cmdline reads both: a `;` in it is the
/// command's own. An empty or malformed one reports nothing, as though none were sent.
/// @param parameters Everything after the "C", e.g. ";cmdline=ls\ -la"; empty for a bare "C".
/// @return The command line, or nullopt when none was reported.
[[nodiscard]] std::optional<std::string> reportedCommandLine(std::string_view parameters);

} // namespace vtbackend
