// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/view/TextEdit.hpp>

using orchestrator::view::applyTextEdit;
using orchestrator::view::TextEdit;
using orchestrator::view::textEditOf;

namespace
{
using core::tui::KeyCode;
using core::tui::KeyEvent;
using core::tui::Modifier;

[[nodiscard]] KeyEvent ctrl(char32_t letter)
{
    return KeyEvent { .key = static_cast<KeyCode>(letter), .modifiers = Modifier::Ctrl, .codepoint = letter };
}
} // namespace

TEST_CASE("the line-editing keys", "[orchestrator][view][edit]")
{
    struct Row
    {
        KeyEvent key;
        std::optional<TextEdit> edit;
    };
    auto const rows = std::array {
        Row { .key = KeyEvent { .key = KeyCode::Backspace }, .edit = TextEdit::DropChar },
        Row { .key = KeyEvent { .key = KeyCode::Backspace, .modifiers = Modifier::Alt },
              .edit = TextEdit::DropWord },
        Row { .key = KeyEvent { .key = KeyCode::Backspace, .modifiers = Modifier::Ctrl },
              .edit = TextEdit::DropWord },
        Row { .key = ctrl(U'w'), .edit = TextEdit::DropBigWord },
        Row { .key = ctrl(U'u'), .edit = TextEdit::Clear },
        Row { .key = ctrl(U'k'), .edit = std::nullopt },
        Row { .key = KeyEvent { .key = static_cast<KeyCode>(U'w'), .codepoint = U'w' },
              .edit = std::nullopt },
    };
    for (auto const& row: rows)
        CHECK(textEditOf(row.key) == row.edit);
}

TEST_CASE("an edit drops from the end of the text", "[orchestrator][view][edit]")
{
    struct Row
    {
        std::string_view text;
        TextEdit edit;
        std::string_view after;
    };
    constexpr auto Rows = std::array {
        Row { .text = "fix the test", .edit = TextEdit::DropChar, .after = "fix the tes" },
        Row { .text = "naïve", .edit = TextEdit::DropChar, .after = "naïv" },
        Row { .text = "café", .edit = TextEdit::DropChar, .after = "caf" },
        Row { .text = "", .edit = TextEdit::DropChar, .after = "" },
        Row { .text = "fix the flaky-test", .edit = TextEdit::DropWord, .after = "fix the flaky-" },
        Row { .text = "fix the test  ", .edit = TextEdit::DropWord, .after = "fix the " },
        Row { .text = "fix the flaky-test", .edit = TextEdit::DropBigWord, .after = "fix the " },
        Row { .text = "fix the test  ", .edit = TextEdit::DropBigWord, .after = "fix the " },
        Row { .text = "über straße", .edit = TextEdit::DropBigWord, .after = "über " },
        Row { .text = "über straße", .edit = TextEdit::DropWord, .after = "über " },
        Row { .text = "word", .edit = TextEdit::DropBigWord, .after = "" },
        Row { .text = "   ", .edit = TextEdit::DropBigWord, .after = "" },
        Row { .text = "fix the test", .edit = TextEdit::Clear, .after = "" },
    };
    for (auto const& row: Rows)
    {
        INFO(row.text);
        auto text = std::string { row.text };
        applyTextEdit(text, row.edit);
        CHECK(text == row.after);
    }
}
