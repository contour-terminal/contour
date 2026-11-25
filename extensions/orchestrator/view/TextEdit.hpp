// SPDX-License-Identifier: Apache-2.0
//
// The line-editing keys every text field of the dashboard shares — the start dialog's note, the `n` box's
// text, the message box, the filters and the palette's query. The fields have no cursor, so every edit works
// backwards from the end of the text, as a shell's line editor does with its cursor at the end.
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace orchestrator::view
{

/// What a line-editing key does to a field's text.
enum class TextEdit : uint8_t
{
    DropChar = 0, ///< The last code point (Backspace).
    DropWord,     ///< The last word of letters, digits and `_`, and what follows it (Alt+Backspace).
    DropBigWord,  ///< The last run of non-blanks, and the blanks after it (Ctrl+W).
    Clear,        ///< Everything (Ctrl+U).
};

/// @param key A key the terminal sent.
/// @return The edit it asks of a text field; std::nullopt for a key that edits nothing.
[[nodiscard]] std::optional<TextEdit> textEditOf(core::tui::KeyEvent const& key) noexcept;

/// Applies @p edit to the end of @p text, never leaving a code point cut in half.
/// @param text The field's text, UTF-8.
/// @param edit What to drop.
void applyTextEdit(std::string& text, TextEdit edit);

} // namespace orchestrator::view
