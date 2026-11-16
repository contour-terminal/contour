// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <string>

namespace core::tui
{

/// Builds a complete SGR (Select Graphic Rendition) escape sequence for the given style.
///
/// Encodes bold, dim, italic, underline (with extended styles), inverse, strikethrough,
/// foreground/background colors (256-color and truecolor), and underline color (SGR 58).
/// Returns an empty string if the style is fully default.
///
/// Underline color is emitted as a separate SGR sequence to avoid exceeding the
/// 16-element parameter array limit in some terminals.
///
/// @param style The visual style to encode.
/// @return The SGR escape sequence string, or empty if no attributes are set.
[[nodiscard]] std::string buildSgrSequence(Style const& style);

/// Builds the bare SGR reset, `CSI m`, which returns every attribute to the terminal's default.
///
/// The counterpart of @c buildSgrSequence for a caller that composes its own output: the same
/// bytes @c TerminalOutput::writeText ends styled text with, and @c TerminalOutput::resetStyle
/// writes. Before it existed the reset was private to @c TerminalOutput, and a caller that needed
/// one spelled it itself.
/// @return The SGR reset sequence.
[[nodiscard]] std::string buildSgrReset();

} // namespace core::tui
