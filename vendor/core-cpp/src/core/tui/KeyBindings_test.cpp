// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyBindings.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

namespace
{

// Helper to create a KeyEvent for a printable character
KeyEvent charEvent(char c, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = keyCodeFromCodepoint(static_cast<char32_t>(c)),
                      .modifiers = mod,
                      .codepoint = static_cast<char32_t>(c) };
}

// Helper to create a KeyEvent for a special key
KeyEvent keyEvent(KeyCode key, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = key, .modifiers = mod, .codepoint = 0 };
}

} // namespace

// ============================================================================
// KeyChord tests
// ============================================================================

TEST_CASE("KeyChord.matches_character")
{
    auto chord = KeyChord::fromChar('a', Modifier::Ctrl);

    CHECK(chord.matches(charEvent('a', Modifier::Ctrl)));
    CHECK_FALSE(chord.matches(charEvent('a')));                 // Missing Ctrl
    CHECK_FALSE(chord.matches(charEvent('b', Modifier::Ctrl))); // Wrong char
    CHECK_FALSE(chord.matches(charEvent('a', Modifier::Alt)));  // Wrong modifier
}

TEST_CASE("KeyChord.matches_special_key")
{
    auto chord = KeyChord::fromKey(KeyCode::Enter, Modifier::Shift);

    CHECK(chord.matches(keyEvent(KeyCode::Enter, Modifier::Shift)));
    CHECK_FALSE(chord.matches(keyEvent(KeyCode::Enter)));                // Missing Shift
    CHECK_FALSE(chord.matches(keyEvent(KeyCode::Tab, Modifier::Shift))); // Wrong key
}

TEST_CASE("KeyChord.matches_combined_modifiers")
{
    auto chord = KeyChord::fromChar('z', Modifier::Ctrl | Modifier::Shift);

    CHECK(chord.matches(charEvent('z', Modifier::Ctrl | Modifier::Shift)));
    CHECK_FALSE(chord.matches(charEvent('z', Modifier::Ctrl)));  // Missing Shift
    CHECK_FALSE(chord.matches(charEvent('z', Modifier::Shift))); // Missing Ctrl
}

TEST_CASE("KeyChord.matches_shifted_symbol_ignores_shift")
{
    // A shifted symbol such as '?' (Shift+'/') already encodes Shift in its glyph. Terminals
    // differ on whether they additionally report the Shift modifier (e.g. Windows win32-input
    // mode does, most POSIX terminals do not). The binding must match either way.
    auto chord = *KeyChord::parse("?");
    REQUIRE(chord.codepoint == U'?');
    REQUIRE(chord.modifiers == Modifier::None);

    CHECK(chord.matches(charEvent('?')));                  // POSIX: no Shift reported
    CHECK(chord.matches(charEvent('?', Modifier::Shift))); // Windows: redundant Shift reported
    // Lock keys are still ignored alongside the redundant Shift.
    CHECK(chord.matches(charEvent('?', Modifier::Shift | Modifier::CapsLock)));
    // A genuine extra modifier (Ctrl) must still differentiate.
    CHECK_FALSE(chord.matches(charEvent('?', Modifier::Ctrl)));
}

TEST_CASE("KeyChord.matches_letter_keeps_shift_significant")
{
    // Letters are the exception: "g" and "shift+g" are distinct bindings, so Shift must remain
    // significant for alphabetic codepoints (regression guard for the shifted-symbol relaxation).
    auto plain = *KeyChord::parse("g");
    CHECK(plain.matches(charEvent('g')));
    CHECK_FALSE(plain.matches(charEvent('g', Modifier::Shift)));

    auto shifted = *KeyChord::parse("shift+g");
    REQUIRE(shifted.codepoint == U'g');
    REQUIRE(shifted.modifiers == Modifier::Shift);
    CHECK(shifted.matches(charEvent('g', Modifier::Shift)));
    CHECK_FALSE(shifted.matches(charEvent('g')));
}

// ============================================================================
// withoutLockKeys tests
// ============================================================================

TEST_CASE("withoutLockKeys.strips_capslock")
{
    CHECK(withoutLockKeys(Modifier::CapsLock) == Modifier::None);
}

TEST_CASE("withoutLockKeys.strips_numlock")
{
    CHECK(withoutLockKeys(Modifier::NumLock) == Modifier::None);
}

TEST_CASE("withoutLockKeys.strips_both_lock_keys")
{
    CHECK(withoutLockKeys(Modifier::CapsLock | Modifier::NumLock) == Modifier::None);
}

TEST_CASE("withoutLockKeys.preserves_ctrl_shift_when_stripping_numlock")
{
    auto const mods = Modifier::Ctrl | Modifier::Shift | Modifier::NumLock;
    CHECK(withoutLockKeys(mods) == (Modifier::Ctrl | Modifier::Shift));
}

TEST_CASE("withoutLockKeys.preserves_none")
{
    CHECK(withoutLockKeys(Modifier::None) == Modifier::None);
}

// ============================================================================
// KeyChord lock key tolerance tests
// ============================================================================

TEST_CASE("KeyChord.matches_with_numlock")
{
    auto chord = KeyChord::fromChar('a', Modifier::Ctrl);
    CHECK(chord.matches(charEvent('a', Modifier::Ctrl | Modifier::NumLock)));
}

TEST_CASE("KeyChord.matches_with_capslock")
{
    auto chord = KeyChord::fromChar('a', Modifier::Ctrl);
    CHECK(chord.matches(charEvent('a', Modifier::Ctrl | Modifier::CapsLock)));
}

TEST_CASE("KeyChord.matches_with_both_lock_keys")
{
    auto chord = KeyChord::fromChar('a', Modifier::Ctrl);
    CHECK(chord.matches(charEvent('a', Modifier::Ctrl | Modifier::NumLock | Modifier::CapsLock)));
}

TEST_CASE("KeyChord.matches_no_modifier_with_numlock")
{
    auto chord = KeyChord::fromKey(KeyCode::Enter, Modifier::None);
    CHECK(chord.matches(keyEvent(KeyCode::Enter, Modifier::NumLock)));
}

TEST_CASE("KeyChord.matches_no_modifier_with_capslock")
{
    auto chord = KeyChord::fromKey(KeyCode::Tab, Modifier::None);
    CHECK(chord.matches(keyEvent(KeyCode::Tab, Modifier::CapsLock)));
}

// ============================================================================
// KeyBindings tests
// ============================================================================

TEST_CASE("KeyBindings.bind_and_lookup")
{
    KeyBindings bindings;

    bindings.bind(KeyChord::fromChar('z', Modifier::Ctrl), EditAction::Undo);

    auto result = bindings.lookup(charEvent('z', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Undo);
}

TEST_CASE("KeyBindings.lookup_returns_nullopt_for_unbound")
{
    KeyBindings const bindings;

    auto result = bindings.lookup(charEvent('x', Modifier::Ctrl));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("KeyBindings.bind_replaces_existing")
{
    KeyBindings bindings;

    bindings.bind(KeyChord::fromChar('z', Modifier::Ctrl), EditAction::Undo);
    bindings.bind(KeyChord::fromChar('z', Modifier::Ctrl), EditAction::Redo);

    auto result = bindings.lookup(charEvent('z', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Redo);
}

TEST_CASE("KeyBindings.bindMultiple")
{
    KeyBindings bindings;

    bindings.bindMultiple({ KeyChord::fromChar('y', Modifier::Ctrl),
                            KeyChord::fromChar('z', Modifier::Ctrl | Modifier::Shift) },
                          EditAction::Redo);

    auto result1 = bindings.lookup(charEvent('y', Modifier::Ctrl));
    REQUIRE(result1.has_value());
    CHECK(*result1 == EditAction::Redo);

    auto result2 = bindings.lookup(charEvent('z', Modifier::Ctrl | Modifier::Shift));
    REQUIRE(result2.has_value());
    CHECK(*result2 == EditAction::Redo);
}

TEST_CASE("KeyBindings.unbind")
{
    KeyBindings bindings;

    bindings.bind(KeyChord::fromChar('z', Modifier::Ctrl), EditAction::Undo);
    bindings.unbind(KeyChord::fromChar('z', Modifier::Ctrl));

    auto result = bindings.lookup(charEvent('z', Modifier::Ctrl));
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// Default bindings tests
// ============================================================================

TEST_CASE("KeyBindings.defaults_undo")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('z', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Undo);
}

TEST_CASE("KeyBindings.defaults_redo_ctrl_y")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('y', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Redo);
}

TEST_CASE("KeyBindings.defaults_redo_ctrl_shift_z")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('z', Modifier::Ctrl | Modifier::Shift));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Redo);
}

TEST_CASE("KeyBindings.defaults_copy")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('c', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Copy);
}

TEST_CASE("KeyBindings.defaults_cut")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('x', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Cut);
}

TEST_CASE("KeyBindings.defaults_paste_unbound")
{
    auto bindings = KeyBindings::defaults();

    // Paste has no default binding (terminal paste handled via PasteEvent)
    auto result = bindings.lookup(charEvent('v', Modifier::Ctrl));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("KeyBindings.defaults_select_all")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('a', Modifier::Ctrl | Modifier::Shift));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::SelectAll);
}

TEST_CASE("KeyBindings.defaults_smart_move_to_line_start")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('a', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::SmartMoveToLineStart);
}

TEST_CASE("KeyBindings.defaults_smart_move_to_line_end")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('e', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::SmartMoveToLineEnd);
}

TEST_CASE("KeyBindings.defaults_delete_char_ctrl_d")
{
    auto bindings = KeyBindings::defaults();

    // Ctrl+D is bound to DeleteCharForward in keybindings
    // (InputField handles the special case of EOF on empty buffer before keybinding lookup)
    auto result = bindings.lookup(charEvent('d', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::DeleteCharForward);
}

TEST_CASE("KeyBindings.defaults_submit_enter")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(keyEvent(KeyCode::Enter));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Submit);
}

TEST_CASE("KeyBindings.defaults_insert_newline_shift_enter")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(keyEvent(KeyCode::Enter, Modifier::Shift));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::InsertNewline);
}

TEST_CASE("KeyBindings.defaults_movement_arrows")
{
    auto bindings = KeyBindings::defaults();

    CHECK(bindings.lookup(keyEvent(KeyCode::Left)) == EditAction::MoveBackwardChar);
    CHECK(bindings.lookup(keyEvent(KeyCode::Right)) == EditAction::MoveForwardChar);
    CHECK(bindings.lookup(keyEvent(KeyCode::Up)) == EditAction::MoveUp);
    CHECK(bindings.lookup(keyEvent(KeyCode::Down)) == EditAction::MoveDown);
}

TEST_CASE("KeyBindings.defaults_movement_ctrl_arrows")
{
    auto bindings = KeyBindings::defaults();

    CHECK(bindings.lookup(keyEvent(KeyCode::Left, Modifier::Ctrl)) == EditAction::MoveBackwardWord);
    CHECK(bindings.lookup(keyEvent(KeyCode::Right, Modifier::Ctrl)) == EditAction::MoveForwardWord);
}

TEST_CASE("KeyBindings.defaults_home_end")
{
    auto bindings = KeyBindings::defaults();

    CHECK(bindings.lookup(keyEvent(KeyCode::Home)) == EditAction::MoveToLineStart);
    CHECK(bindings.lookup(keyEvent(KeyCode::End)) == EditAction::MoveToLineEnd);
    CHECK(bindings.lookup(keyEvent(KeyCode::Home, Modifier::Ctrl)) == EditAction::MoveToBufferStart);
    CHECK(bindings.lookup(keyEvent(KeyCode::End, Modifier::Ctrl)) == EditAction::MoveToBufferEnd);
}

TEST_CASE("KeyBindings.defaults_delete_backspace")
{
    auto bindings = KeyBindings::defaults();

    CHECK(bindings.lookup(keyEvent(KeyCode::Backspace)) == EditAction::DeleteCharBackward);
    CHECK(bindings.lookup(keyEvent(KeyCode::Delete)) == EditAction::DeleteCharForward);
    CHECK(bindings.lookup(keyEvent(KeyCode::Backspace, Modifier::Ctrl)) == EditAction::DeleteWordBackward);
    // Alt+Backspace deletes a whole whitespace-delimited word (Fish bigword), distinct from
    // Ctrl+Backspace / Ctrl+W which use small-word boundaries.
    CHECK(bindings.lookup(keyEvent(KeyCode::Backspace, Modifier::Alt)) == EditAction::DeleteBigWordBackward);
}

TEST_CASE("KeyBindings.defaults_kill")
{
    auto bindings = KeyBindings::defaults();

    CHECK(bindings.lookup(charEvent('k', Modifier::Ctrl)) == EditAction::KillToEnd);
    CHECK(bindings.lookup(charEvent('u', Modifier::Ctrl)) == EditAction::KillToStart);
}

TEST_CASE("KeyBindings.customization_override_default")
{
    auto bindings = KeyBindings::defaults();

    // Override Ctrl+Y from Redo to Yank
    bindings.bind(KeyChord::fromChar('y', Modifier::Ctrl), EditAction::Yank);

    auto result = bindings.lookup(charEvent('y', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::Yank);
}

TEST_CASE("KeyBindings.bindings_accessor")
{
    KeyBindings bindings;
    bindings.bind(KeyChord::fromChar('a', Modifier::Ctrl), EditAction::SelectAll);
    bindings.bind(KeyChord::fromChar('z', Modifier::Ctrl), EditAction::Undo);

    auto all = bindings.bindings();
    CHECK(all.size() == 2);
}

// ============================================================================
// KeyChord parsing tests
// ============================================================================

TEST_CASE("KeyChord.parse_single_letter")
{
    auto chord = KeyChord::parse("a");
    REQUIRE(chord.has_value());
    CHECK(chord->codepoint == 'a');
    CHECK(chord->modifiers == Modifier::None);
}

TEST_CASE("KeyChord.parse_ctrl_letter")
{
    auto chord = KeyChord::parse("ctrl+y");
    REQUIRE(chord.has_value());
    CHECK(chord->codepoint == 'y');
    CHECK(chord->modifiers == Modifier::Ctrl);
}

TEST_CASE("KeyChord.parse_ctrl_shift_letter")
{
    auto chord = KeyChord::parse("ctrl+shift+z");
    REQUIRE(chord.has_value());
    CHECK(chord->codepoint == 'z');
    CHECK(chord->modifiers == (Modifier::Ctrl | Modifier::Shift));
}

TEST_CASE("KeyChord.parse_alt_letter")
{
    auto chord = KeyChord::parse("alt+f");
    REQUIRE(chord.has_value());
    CHECK(chord->codepoint == 'f');
    CHECK(chord->modifiers == Modifier::Alt);
}

TEST_CASE("KeyChord.parse_special_key")
{
    auto chord = KeyChord::parse("enter");
    REQUIRE(chord.has_value());
    CHECK(chord->key == KeyCode::Enter);
    CHECK(chord->codepoint == 0);
}

TEST_CASE("KeyChord.parse_ctrl_special_key")
{
    auto chord = KeyChord::parse("ctrl+home");
    REQUIRE(chord.has_value());
    CHECK(chord->key == KeyCode::Home);
    CHECK(chord->modifiers == Modifier::Ctrl);
}

TEST_CASE("KeyChord.parse_shift_enter")
{
    auto chord = KeyChord::parse("shift+enter");
    REQUIRE(chord.has_value());
    CHECK(chord->key == KeyCode::Enter);
    CHECK(chord->modifiers == Modifier::Shift);
}

TEST_CASE("KeyChord.parse_case_insensitive")
{
    auto chord1 = KeyChord::parse("CTRL+Y");
    auto chord2 = KeyChord::parse("Ctrl+y");
    auto chord3 = KeyChord::parse("ctrl+Y");

    REQUIRE(chord1.has_value());
    REQUIRE(chord2.has_value());
    REQUIRE(chord3.has_value());

    CHECK(chord1->codepoint == chord2->codepoint);
    CHECK(chord2->codepoint == chord3->codepoint);
    CHECK(chord1->modifiers == chord2->modifiers);
}

TEST_CASE("KeyChord.parse_function_keys")
{
    CHECK(KeyChord::parse("f1")->key == KeyCode::F1);
    CHECK(KeyChord::parse("f12")->key == KeyCode::F12);
    CHECK(KeyChord::parse("ctrl+f5")->key == KeyCode::F5);
    CHECK(KeyChord::parse("ctrl+f5")->modifiers == Modifier::Ctrl);
}

TEST_CASE("KeyChord.parse_navigation_keys")
{
    CHECK(KeyChord::parse("up")->key == KeyCode::Up);
    CHECK(KeyChord::parse("down")->key == KeyCode::Down);
    CHECK(KeyChord::parse("left")->key == KeyCode::Left);
    CHECK(KeyChord::parse("right")->key == KeyCode::Right);
    CHECK(KeyChord::parse("home")->key == KeyCode::Home);
    CHECK(KeyChord::parse("end")->key == KeyCode::End);
    CHECK(KeyChord::parse("pageup")->key == KeyCode::PageUp);
    CHECK(KeyChord::parse("pagedown")->key == KeyCode::PageDown);
}

TEST_CASE("KeyChord.parse_editing_keys")
{
    CHECK(KeyChord::parse("backspace")->key == KeyCode::Backspace);
    CHECK(KeyChord::parse("delete")->key == KeyCode::Delete);
    CHECK(KeyChord::parse("del")->key == KeyCode::Delete);
    CHECK(KeyChord::parse("tab")->key == KeyCode::Tab);
    CHECK(KeyChord::parse("escape")->key == KeyCode::Escape);
    CHECK(KeyChord::parse("esc")->key == KeyCode::Escape);
}

TEST_CASE("KeyChord.parse_invalid_returns_nullopt")
{
    CHECK_FALSE(KeyChord::parse("").has_value());
    CHECK_FALSE(KeyChord::parse("ctrl+").has_value());
    CHECK_FALSE(KeyChord::parse("unknown").has_value());
    CHECK_FALSE(KeyChord::parse("ctrl+unknown").has_value());
}

TEST_CASE("KeyChord.toString_letter")
{
    auto chord = KeyChord::fromChar('y', Modifier::Ctrl);
    CHECK(chord.toString() == "ctrl+y");
}

TEST_CASE("KeyChord.toString_combined_modifiers")
{
    auto chord = KeyChord::fromChar('z', Modifier::Ctrl | Modifier::Shift);
    CHECK(chord.toString() == "ctrl+shift+z");
}

TEST_CASE("KeyChord.toString_special_key")
{
    auto chord = KeyChord::fromKey(KeyCode::Enter, Modifier::Shift);
    CHECK(chord.toString() == "shift+enter");
}

TEST_CASE("KeyChord.parse_roundtrip")
{
    // Test that parse -> toString -> parse gives the same result
    auto testRoundtrip = [](std::string_view input) {
        auto parsed = KeyChord::parse(input);
        REQUIRE(parsed.has_value());
        auto str = parsed->toString();
        auto reparsed = KeyChord::parse(str);
        REQUIRE(reparsed.has_value());
        CHECK(*parsed == *reparsed);
    };

    testRoundtrip("ctrl+y");
    testRoundtrip("ctrl+shift+z");
    testRoundtrip("alt+enter");
    testRoundtrip("shift+home");
    testRoundtrip("f1");
}

// ============================================================================
// EditAction parsing tests
// ============================================================================

TEST_CASE("parseEditAction.basic_actions")
{
    CHECK(parseEditAction("undo") == EditAction::Undo);
    CHECK(parseEditAction("redo") == EditAction::Redo);
    CHECK(parseEditAction("copy") == EditAction::Copy);
    CHECK(parseEditAction("cut") == EditAction::Cut);
    CHECK(parseEditAction("paste") == EditAction::Paste);
    CHECK(parseEditAction("select-all") == EditAction::SelectAll);
}

TEST_CASE("parseEditAction.movement_actions")
{
    CHECK(parseEditAction("move-forward-char") == EditAction::MoveForwardChar);
    CHECK(parseEditAction("move-backward-char") == EditAction::MoveBackwardChar);
    CHECK(parseEditAction("move-forward-word") == EditAction::MoveForwardWord);
    CHECK(parseEditAction("move-backward-word") == EditAction::MoveBackwardWord);
    CHECK(parseEditAction("move-to-line-start") == EditAction::MoveToLineStart);
    CHECK(parseEditAction("move-to-line-end") == EditAction::MoveToLineEnd);
    CHECK(parseEditAction("move-to-buffer-start") == EditAction::MoveToBufferStart);
    CHECK(parseEditAction("move-to-buffer-end") == EditAction::MoveToBufferEnd);
    CHECK(parseEditAction("move-up") == EditAction::MoveUp);
    CHECK(parseEditAction("move-down") == EditAction::MoveDown);
    CHECK(parseEditAction("smart-move-to-line-start") == EditAction::SmartMoveToLineStart);
    CHECK(parseEditAction("smart-move-to-line-end") == EditAction::SmartMoveToLineEnd);
}

TEST_CASE("parseEditAction.editing_actions")
{
    CHECK(parseEditAction("delete-char-backward") == EditAction::DeleteCharBackward);
    CHECK(parseEditAction("delete-char-forward") == EditAction::DeleteCharForward);
    CHECK(parseEditAction("delete-word") == EditAction::DeleteWord);
    CHECK(parseEditAction("delete-word-backward") == EditAction::DeleteWordBackward);
    CHECK(parseEditAction("delete-big-word-backward") == EditAction::DeleteBigWordBackward);
    CHECK(parseEditAction("kill-to-end") == EditAction::KillToEnd);
    CHECK(parseEditAction("kill-to-start") == EditAction::KillToStart);
    CHECK(parseEditAction("transpose") == EditAction::Transpose);
}

TEST_CASE("parseEditAction.kill_ring_actions")
{
    CHECK(parseEditAction("yank") == EditAction::Yank);
    CHECK(parseEditAction("yank-pop") == EditAction::YankPop);
}

TEST_CASE("parseEditAction.control_actions")
{
    CHECK(parseEditAction("submit") == EditAction::Submit);
    CHECK(parseEditAction("abort") == EditAction::Abort);
    CHECK(parseEditAction("insert-newline") == EditAction::InsertNewline);
}

TEST_CASE("parseEditAction.history_actions")
{
    CHECK(parseEditAction("history-prev") == EditAction::HistoryPrev);
    CHECK(parseEditAction("history-next") == EditAction::HistoryNext);
}

TEST_CASE("parseEditAction.case_insensitive")
{
    CHECK(parseEditAction("UNDO") == EditAction::Undo);
    CHECK(parseEditAction("Undo") == EditAction::Undo);
    CHECK(parseEditAction("Copy") == EditAction::Copy);
    CHECK(parseEditAction("MOVE-FORWARD-CHAR") == EditAction::MoveForwardChar);
}

TEST_CASE("parseEditAction.invalid_returns_nullopt")
{
    CHECK_FALSE(parseEditAction("").has_value());
    CHECK_FALSE(parseEditAction("unknown").has_value());
    CHECK_FALSE(parseEditAction("invalid-action").has_value());
}

TEST_CASE("editActionToString.basic")
{
    CHECK(editActionToString(EditAction::Undo) == "undo");
    CHECK(editActionToString(EditAction::Redo) == "redo");
    CHECK(editActionToString(EditAction::Copy) == "copy");
    CHECK(editActionToString(EditAction::MoveForwardChar) == "move-forward-char");
}

TEST_CASE("editActionToString_parseEditAction_roundtrip")
{
    // Test that toString -> parse gives the same action
    auto testRoundtrip = [](EditAction action) {
        auto str = editActionToString(action);
        auto parsed = parseEditAction(str);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == action);
    };

    testRoundtrip(EditAction::Undo);
    testRoundtrip(EditAction::Redo);
    testRoundtrip(EditAction::Copy);
    testRoundtrip(EditAction::Cut);
    testRoundtrip(EditAction::Paste);
    testRoundtrip(EditAction::MoveForwardChar);
    testRoundtrip(EditAction::DeleteWordBackward);
    testRoundtrip(EditAction::DeleteBigWordBackward);
    testRoundtrip(EditAction::KillToEnd);
    testRoundtrip(EditAction::Yank);
    testRoundtrip(EditAction::Submit);
    testRoundtrip(EditAction::HistoryPrev);
    testRoundtrip(EditAction::AgentMode);
}

// ============================================================================
// AgentMode binding tests
// ============================================================================

TEST_CASE("KeyBindings.defaults_ctrl_t_agent_mode")
{
    auto bindings = KeyBindings::defaults();

    auto result = bindings.lookup(charEvent('t', Modifier::Ctrl));
    REQUIRE(result.has_value());
    CHECK(*result == EditAction::AgentMode);
}

TEST_CASE("parseEditAction.agent_mode")
{
    CHECK(parseEditAction("agent-mode") == EditAction::AgentMode);
}

TEST_CASE("editActionToString.agent_mode")
{
    CHECK(editActionToString(EditAction::AgentMode) == "agent-mode");
}
