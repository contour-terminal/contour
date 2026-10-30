// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/HotkeyQtKeys.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::config::HotkeyChord;
using contour::platform::hotkeyChordOf;
using contour::platform::HotkeyKeyRows;
using contour::platform::HotkeyQtModifierRows;
using contour::platform::qtKeyCombinationOf;
using vtbackend::Modifier;

TEST_CASE("HotkeyQtKeys.everyKeyRowSurvivesAQtRoundTrip", "[hotkeyqtkeys]")
{
    for (auto const& row: HotkeyKeyRows)
    {
        auto const chord = HotkeyChord { .modifiers = Modifier::Control, .key = row.key };
        auto const spelled = qtKeyCombinationOf(chord);
        REQUIRE(spelled.has_value());
        auto const back = hotkeyChordOf(spelled->key(), spelled->keyboardModifiers());
        REQUIRE(back.has_value());
        CHECK(*back == chord);
    }
}

TEST_CASE("HotkeyQtKeys.everyModifierRowSurvivesAQtRoundTrip", "[hotkeyqtkeys]")
{
    for (auto const& row: HotkeyQtModifierRows)
    {
        auto const chord = HotkeyChord { .modifiers = row.modifier, .key = U'a' };
        auto const spelled = qtKeyCombinationOf(chord);
        REQUIRE(spelled.has_value());
        CHECK(spelled->keyboardModifiers() == Qt::KeyboardModifiers { row.qt });
        CHECK(hotkeyChordOf(spelled->key(), spelled->keyboardModifiers()) == chord);
    }
}

TEST_CASE("HotkeyQtKeys.theBackquoteChordIsQts", "[hotkeyqtkeys]")
{
    auto const spelled = qtKeyCombinationOf({ .modifiers = Modifier::Control, .key = U'`' });
    REQUIRE(spelled.has_value());
    CHECK(spelled->key() == Qt::Key_QuoteLeft);
    CHECK(spelled->keyboardModifiers() == Qt::ControlModifier);

    CHECK(hotkeyChordOf(Qt::Key_Q, Qt::MetaModifier)
          == HotkeyChord { .modifiers = Modifier::Super, .key = U'q' });
    CHECK(hotkeyChordOf(Qt::Key_Return, Qt::ControlModifier)
          == HotkeyChord { .modifiers = Modifier::Control, .key = vtbackend::Key::Enter });
}

TEST_CASE("HotkeyQtKeys.aKeyWithoutARowHasNoChord", "[hotkeyqtkeys]")
{
    CHECK_FALSE(hotkeyChordOf(Qt::Key_Backspace, Qt::ControlModifier).has_value());
    CHECK_FALSE(hotkeyChordOf(Qt::Key_Eacute, Qt::ControlModifier).has_value());
    CHECK_FALSE(hotkeyChordOf(Qt::Key_Control, Qt::ControlModifier).has_value());
    CHECK_FALSE(
        qtKeyCombinationOf({ .modifiers = Modifier::Control, .key = vtbackend::Key::F35 }).has_value());
}
