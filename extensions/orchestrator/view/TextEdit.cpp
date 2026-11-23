// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/TextEdit.hpp>

#include <algorithm>
#include <array>
#include <ranges>

#include <orchestrator/view/Keys.hpp>

namespace orchestrator::view
{

namespace
{
    /// One line-editing key: a special key with exact modifiers, or a Ctrl+letter.
    struct TextEditKey
    {
        core::tui::KeyCode key = core::tui::KeyCode::None;         ///< The special key; None: a letter.
        core::tui::Modifier modifiers = core::tui::Modifier::None; ///< Its modifiers, exactly.
        char32_t ctrlLetter = 0;                                   ///< The letter pressed with Ctrl.
        TextEdit edit = TextEdit::DropChar;                        ///< What it does.
    };

    using core::tui::KeyCode;
    using core::tui::Modifier;

    // Ctrl+Backspace reaches us as Backspace from most terminals (0x08), so it drops a character there.
    constexpr auto TextEditKeys = std::array {
        TextEditKey { .key = KeyCode::Backspace, .edit = TextEdit::DropChar },
        TextEditKey { .key = KeyCode::Backspace, .modifiers = Modifier::Ctrl, .edit = TextEdit::DropWord },
        TextEditKey { .key = KeyCode::Backspace, .modifiers = Modifier::Alt, .edit = TextEdit::DropWord },
        TextEditKey { .ctrlLetter = U'w', .edit = TextEdit::DropBigWord },
        TextEditKey { .ctrlLetter = U'u', .edit = TextEdit::Clear },
    };

    [[nodiscard]] bool matches(TextEditKey const& row, core::tui::KeyEvent const& key) noexcept
    {
        if (row.ctrlLetter != 0)
            return isCtrl(key, row.ctrlLetter);
        return key.key == row.key && key.modifiers == row.modifiers;
    }

    /// @return Whether @p c belongs to a word: a letter, a digit, `_`, or any byte of a non-ASCII code point.
    [[nodiscard]] constexpr bool isWordByte(char c) noexcept
    {
        auto const byte = static_cast<unsigned char>(c);
        return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z')
               || (byte >= 'A' && byte <= 'Z') || byte == '_';
    }

    [[nodiscard]] constexpr bool isBlank(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n';
    }

    /// Drops the bytes at the end of @p text that @p skip holds, then those that @p take holds. Every byte of
    /// a non-ASCII code point is taken or skipped alike, so that no code point is cut in half.
    template <typename Skip, typename Take>
    void dropBackwards(std::string& text, Skip skip, Take take)
    {
        auto const reversed = text | std::views::reverse;
        auto const word = std::ranges::find_if_not(reversed, skip);
        auto const start = std::ranges::find_if_not(std::ranges::subrange(word, reversed.end()), take);
        text.erase(static_cast<std::size_t>(std::ranges::distance(start, reversed.end())));
    }
} // namespace

std::optional<TextEdit> textEditOf(core::tui::KeyEvent const& key) noexcept
{
    auto const row = std::ranges::find_if(
        TextEditKeys, [&](TextEditKey const& candidate) { return matches(candidate, key); });
    if (row == TextEditKeys.end())
        return std::nullopt;
    return row->edit;
}

void applyTextEdit(std::string& text, TextEdit edit)
{
    switch (edit)
    {
        case TextEdit::DropChar: dropLastCodepoint(text); return;
        case TextEdit::DropWord:
            dropBackwards(text, [](char c) { return !isWordByte(c); }, isWordByte);
            return;
        case TextEdit::DropBigWord: dropBackwards(text, isBlank, [](char c) { return !isBlank(c); }); return;
        case TextEdit::Clear: text.clear(); return;
    }
}

} // namespace orchestrator::view
