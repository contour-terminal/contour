// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vtbackend
{

/// What a sanitised command line is for, which decides how much of it may survive.
enum class SanitizePurpose : uint8_t
{
    /// Shown to the user: a notification, a tooltip, the sticky header.
    Display = 0,
    /// Typed back into a shell through a bracketed paste, which keeps TAB and LF from acting as keys.
    Insert,
    /// Typed into the PTY raw, not through the paste path: as Insert, and TAB and LF go too, being keys.
    Type,
    /// Put on the clipboard (CopyCommandLine), to be pasted wherever the user pastes it, bracketed or not:
    /// as Insert, but LF, CR and TAB become a space and trailing spaces go, so the paste is one line that
    /// runs nothing until the user presses Enter.
    Clipboard,
};

/// Makes a command line reported by a shell safe to show, to type back into a shell, or to copy.
///
/// A command line arrives from whatever wrote to the PTY, so it is untrusted.
/// Display: C0/C1 controls, bidi overrides and the line and paragraph separators → U+FFFD; Insert:
/// additionally drops ESC and every C0 except TAB and LF, and every C1; Type: drops TAB and LF as well;
/// Clipboard: as Insert, with TAB, LF and CR turned into spaces and the trailing spaces trimmed.
///
/// In detail: DEL counts as a C0 control. The bidi controls are the embeddings, overrides and
/// isolates (U+202A..U+202E, U+2066..U+2069); the separators are U+2028 and U+2029, at which a text
/// layout breaks the line. Insert, too, replaces rather than drops either kind, so the user sees where
/// one was. Ill-formed UTF-8 becomes one U+FFFD per maximal subpart, for every
/// purpose. Nothing is truncated: bounding the length is the caller's business.
/// @param text The raw command line, as reported or recovered.
/// @param purpose What the result is for.
/// @return The sanitised command line, well-formed UTF-8.
[[nodiscard]] std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose);

/// Whether sanitizeCommandLine() would hand @p text back as it is for @p purpose, answered without building
/// the sanitised copy: false at the first code point it would replace or drop.
/// @param text The text to check.
/// @param purpose What the text is for.
/// @return Whether @p text is already safe for @p purpose.
[[nodiscard]] bool isUnchangedBySanitizing(std::string_view text, SanitizePurpose purpose);

} // namespace vtbackend
