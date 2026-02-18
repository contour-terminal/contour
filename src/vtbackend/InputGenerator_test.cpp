// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/InputGenerator.h>

#include <crispy/escape.h>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace std;
using namespace vtbackend;
using Buffer = vtbackend::InputGenerator::Sequence;
using crispy::escape;

TEST_CASE("InputGenerator.Modifier.encodings")
{
    // Ensures we can construct the correct values that are needed
    // as parameters for input events in the VT protocol.

    auto constexpr Alt = Modifiers { Modifier::Alt };
    auto constexpr Shift = Modifiers { Modifier::Shift };
    auto constexpr Control = Modifiers { Modifier::Control };
    auto constexpr Super = Modifiers { Modifier::Super };

    CHECK((1 + Shift.value()) == 2);
    CHECK((1 + Alt.value()) == 3);
    CHECK((1 + (Shift | Alt).value()) == 4);
    CHECK((1 + Control.value()) == 5);
    CHECK((1 + (Shift | Control).value()) == 6);
    CHECK((1 + (Alt | Control).value()) == 7);
    CHECK((1 + (Shift | Alt | Control).value()) == 8);
    CHECK((1 + Super.value()) == 9);
    CHECK((1 + (Super | Shift).value()) == 10);
    CHECK((1 + (Super | Alt).value()) == 11);
    CHECK((1 + (Super | Alt | Shift).value()) == 12);
    CHECK((1 + (Super | Control).value()) == 13);
    CHECK((1 + (Super | Control | Shift).value()) == 14);
    CHECK((1 + (Super | Control | Alt).value()) == 15);
    CHECK((1 + (Super | Control | Alt | Shift).value()) == 16);
}

TEST_CASE("InputGenerator.consume")
{
    auto input = InputGenerator {};
    input.generateRaw("ABCDEF"sv);
    REQUIRE(input.peek() == "ABCDEF"sv);
    input.consume(2);
    REQUIRE(input.peek() == "CDEF"sv);
    input.consume(3);
    REQUIRE(input.peek() == "F"sv);

    input.generateRaw("abcdef"sv);
    REQUIRE(input.peek() == "Fabcdef"sv);
    input.consume(7);
    REQUIRE(input.peek().empty());
}

TEST_CASE("InputGenerator.Ctrl+Space", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate(L' ', Modifier::Control, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == "\\x00");
}

TEST_CASE("InputGenerator.Ctrl+A", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('A', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x01));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+D", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('D', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+[", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('[', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x1b)); // 27
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+\\", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('\\', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x1c)); // 28
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+]", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate(']', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x1d)); // 29
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+^", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('^', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x1e)); // 30
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+_", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('_', Modifier::Control, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x1f)); // 31
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Modifier+ArrowKeys", "[terminal,input]")
{
    struct Mapping
    {
        Modifiers modifiers;
        Key key;
        string_view expected;
    };

    auto constexpr None = Modifiers {};
    auto constexpr Alt = Modifiers { Modifier::Alt };
    auto constexpr Shift = Modifiers { Modifier::Shift };
    auto constexpr Control = Modifiers { Modifier::Control };
    auto constexpr Super = Modifiers { Modifier::Super };

    auto constexpr Mappings = std::array {
        Mapping { .modifiers = None, .key = Key::UpArrow, .expected = "\033[A"sv },
        Mapping { .modifiers = None, .key = Key::DownArrow, .expected = "\033[B"sv },
        Mapping { .modifiers = None, .key = Key::RightArrow, .expected = "\033[C"sv },
        Mapping { .modifiers = None, .key = Key::LeftArrow, .expected = "\033[D"sv },
        Mapping { .modifiers = Shift, .key = Key::UpArrow, .expected = "\033[1;2A"sv },
        Mapping { .modifiers = Shift, .key = Key::DownArrow, .expected = "\033[1;2B"sv },
        Mapping { .modifiers = Shift, .key = Key::RightArrow, .expected = "\033[1;2C"sv },
        Mapping { .modifiers = Shift, .key = Key::LeftArrow, .expected = "\033[1;2D"sv },
#ifdef __APPLE__
        Mapping { .modifiers = Alt, .key = Key::UpArrow, .expected = "\033[1;5A"sv },
        Mapping { .modifiers = Alt, .key = Key::DownArrow, .expected = "\033[1;5B"sv },
        Mapping { .modifiers = Alt, .key = Key::RightArrow, .expected = "\033[1;5C"sv },
        Mapping { .modifiers = Alt, .key = Key::LeftArrow, .expected = "\033[1;5D"sv },
#else
        Mapping { .modifiers = Alt, .key = Key::UpArrow, .expected = "\033[1;3A"sv },
        Mapping { .modifiers = Alt, .key = Key::DownArrow, .expected = "\033[1;3B"sv },
        Mapping { .modifiers = Alt, .key = Key::RightArrow, .expected = "\033[1;3C"sv },
        Mapping { .modifiers = Alt, .key = Key::LeftArrow, .expected = "\033[1;3D"sv },
#endif
        Mapping { .modifiers = Control, .key = Key::UpArrow, .expected = "\033[1;5A"sv },
        Mapping { .modifiers = Control, .key = Key::DownArrow, .expected = "\033[1;5B"sv },
        Mapping { .modifiers = Control, .key = Key::RightArrow, .expected = "\033[1;5C"sv },
        Mapping { .modifiers = Control, .key = Key::LeftArrow, .expected = "\033[1;5D"sv },
        Mapping { .modifiers = Super, .key = Key::UpArrow, .expected = "\033[1;9A"sv },
        Mapping { .modifiers = Super, .key = Key::DownArrow, .expected = "\033[1;9B"sv },
        Mapping { .modifiers = Super, .key = Key::RightArrow, .expected = "\033[1;9C"sv },
        Mapping { .modifiers = Super, .key = Key::LeftArrow, .expected = "\033[1;9D"sv },
        // some mixes
        Mapping { .modifiers = Shift | Alt, .key = Key::UpArrow, .expected = "\033[1;4A"sv },
        Mapping { .modifiers = Control | Alt, .key = Key::UpArrow, .expected = "\033[1;7A"sv },
        Mapping { .modifiers = Control | Alt | Super, .key = Key::UpArrow, .expected = "\033[1;15A"sv },
    };

    for (auto const& mapping: Mappings)
    {
        auto input = InputGenerator {};
        input.generate(mapping.key, mapping.modifiers, KeyboardEventType::Press);
        INFO(std::format("Testing {}+{} => {}", mapping.modifiers, mapping.key, escape(mapping.expected)));
        REQUIRE(escape(input.peek()) == escape(mapping.expected));
    }
}

TEST_CASE("InputGenerator.all(Ctrl + A..Z)", "[terminal,input]")
{
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        INFO(std::format("Testing Ctrl+{}", ch));
        auto input = InputGenerator {};
        input.generate(static_cast<char32_t>(ch), Modifier::Control, KeyboardEventType::Press);
        auto const c0 = string(1, static_cast<char>(ch - 'A' + 1));
        REQUIRE(escape(input.peek()) == escape(c0));
    }
}

// {{{ Lock modifier tests (NumLock / CapsLock must not break standard input)

TEST_CASE("InputGenerator.Ctrl+D_with_NumLock", "[terminal,input]")
{
    auto constexpr CtrlNumLock = Modifiers { Modifier::Control } | Modifier::NumLock;
    auto input = InputGenerator {};
    input.generate('D', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+A_with_NumLock", "[terminal,input]")
{
    auto constexpr CtrlNumLock = Modifiers { Modifier::Control } | Modifier::NumLock;
    auto input = InputGenerator {};
    input.generate('A', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x01));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+C_with_NumLock", "[terminal,input]")
{
    auto constexpr CtrlNumLock = Modifiers { Modifier::Control } | Modifier::NumLock;
    auto input = InputGenerator {};
    input.generate('C', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x03));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.all_Ctrl_A_to_Z_with_NumLock", "[terminal,input]")
{
    auto constexpr CtrlNumLock = Modifiers { Modifier::Control } | Modifier::NumLock;
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        INFO(std::format("Testing Ctrl+{} with NumLock", ch));
        auto input = InputGenerator {};
        input.generate(static_cast<char32_t>(ch), CtrlNumLock, KeyboardEventType::Press);
        auto const c0 = string(1, static_cast<char>(ch - 'A' + 1));
        REQUIRE(escape(input.peek()) == escape(c0));
    }
}

TEST_CASE("InputGenerator.Ctrl+D_with_CapsLock", "[terminal,input]")
{
    auto constexpr CtrlCapsLock = Modifiers { Modifier::Control } | Modifier::CapsLock;
    auto input = InputGenerator {};
    input.generate('D', CtrlCapsLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+D_with_NumLock_and_CapsLock", "[terminal,input]")
{
    auto constexpr CtrlBothLocks = Modifiers { Modifier::Control } | Modifier::NumLock | Modifier::CapsLock;
    auto input = InputGenerator {};
    input.generate('D', CtrlBothLocks, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Shift+Tab_with_NumLock", "[terminal,input]")
{
    auto constexpr ShiftNumLock = Modifiers { Modifier::Shift } | Modifier::NumLock;
    auto input = InputGenerator {};
    input.generate(static_cast<char32_t>(0x09), ShiftNumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\033[Z"sv));
}

// }}}

// {{{ ExtendedKeyboardInputGenerator

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Ctrl+L", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.generateChar('L', 'L', Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[108;5u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Escape", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    input.generateKey(Key::Escape, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[27u"sv));

    input.generateKey(Key::Escape, Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[27;2u"sv));

    // repeat is not being encoded here, because we did not request ReportEventTypes.
    input.generateKey(Key::Escape, Modifier::Shift, KeyboardEventType::Repeat);
    REQUIRE(escape(input.take()) == escape("\033[27;2u"sv));

    input.generateKey(Key::Escape, Modifier::Shift, KeyboardEventType::Release);
    REQUIRE(escape(input.take()) == escape(""sv));

    // Now we do request ReportEventTypes, so we should get the repeat and release event.
    input.flags().enable(KeyboardEventFlag::ReportEventTypes);

    input.generateKey(Key::Escape, Modifier::Shift, KeyboardEventType::Repeat);
    REQUIRE(escape(input.take()) == escape("\033[27;2:2u"sv));

    input.generateKey(Key::Escape, Modifier::Shift, KeyboardEventType::Release);
    REQUIRE(escape(input.take()) == escape("\033[27;2:3u"sv));
}

TEST_CASE("InputGenerator.DECNKM", "[terminal,input]")
{
    auto input = InputGenerator {};

    // Default: numeric keypad mode
    REQUIRE(input.numericKeypad());
    input.generate(Key::Numpad_5, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("5"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Enable application keypad (DECNKM set)
    input.setApplicationKeypadMode(true);
    REQUIRE(input.applicationKeypad());
    input.generate(Key::Numpad_5, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[E"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Disable application keypad (DECNKM reset)
    input.setApplicationKeypadMode(false);
    REQUIRE(input.numericKeypad());
    input.generate(Key::Numpad_5, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("5"sv));
}

TEST_CASE("InputGenerator.DECBKM.Backspace", "[terminal,input]")
{
    auto input = InputGenerator {};

    // Default (DECBKM reset): Backspace sends DEL (0x7F)
    input.generate(Key::Backspace, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x7F"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Default (DECBKM reset): Ctrl+Backspace sends BS (0x08)
    input.generate(Key::Backspace, Modifier::Control, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x08"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Enable DECBKM: Backspace sends BS (0x08)
    input.setBackarrowKeyMode(true);
    input.generate(Key::Backspace, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x08"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Enable DECBKM: Ctrl+Backspace sends DEL (0x7F)
    input.generate(Key::Backspace, Modifier::Control, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x7F"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Disable DECBKM: Backspace reverts to DEL (0x7F)
    input.setBackarrowKeyMode(false);
    input.generate(Key::Backspace, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x7F"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.CapsLock", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // 'A' with CapsLock active + ReportAllKeys: key code is lowercase (97),
    // modifier is CapsLock (64), encoded = 1 + 64 = 65
    // (CapsLock alone with only DisambiguateEscapeCodes would go to legacy,
    //  because lock modifiers don't trigger CSI u on their own)
    input.generateChar('A', 'a', Modifier::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[97;65u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.NumLock", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // '5' with NumLock (128) + ReportAllKeys, encoded = 1 + 128 = 129
    input.generateChar('5', '5', Modifier::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[53;129u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.CapsLock.ReportEventTypes", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportEventTypes);

    // 'A' with CapsLock, Press event
    // CapsLock is a lock modifier — Press event doesn't need action encoding,
    // so this goes to legacy (no real mods, no action, no report-all-keys)
    input.generateChar('A', 'a', Modifier::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("A"sv));

    // 'A' with CapsLock, Release event — needs action encoding, so CSI u kicks in
    // modifier = CapsLock (64), encoded = 1 + 64 = 65, event type = 3 (Release)
    input.generateChar('A', 'a', Modifier::CapsLock, KeyboardEventType::Release);
    REQUIRE(escape(input.take()) == escape("\033[97;65:3u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.Home", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // Home with no modifiers: CSI 1 H
    input.generateKey(Key::Home, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1H"sv));

    // Ctrl+Home: CSI 1;5 H
    input.generateKey(Key::Home, Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1;5H"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.End", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // End with no modifiers: CSI 1 F
    input.generateKey(Key::End, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1F"sv));

    // Ctrl+End: CSI 1;5 F
    input.generateKey(Key::End, Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1;5F"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Shift+3", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // Shift+3 → '#': primary key must be '3' (51), not '#' (35)
    input.generateChar('#', '3', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[51;2u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Shift+3.AlternateKeys", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAlternateKeys);

    // With alternate keys: CSI 51:35;2u (key='3', shifted_key='#', Shift)
    input.generateChar('#', '3', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[51:35;2u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Shift+semicolon", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // Shift+; → ':' — primary key must be ';' (59), not ':' (58)
    input.generateChar(':', ';', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[59;2u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.plain_hash_german", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // Direct '#' key (German layout): physicalKey='#', no shift
    // No semicolon when modifiers encode to empty string
    input.generateChar('#', '#', Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[35u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.F1_F4", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // F1 no mods: CSI 1 P
    input.generateKey(Key::F1, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1P"sv));

    // F2 no mods: CSI 1 Q
    input.generateKey(Key::F2, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1Q"sv));

    // F3 no mods: CSI 13 ~ (tilde-form to avoid CSI R conflict)
    input.generateKey(Key::F3, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[13~"sv));

    // F4 no mods: CSI 1 S
    input.generateKey(Key::F4, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1S"sv));

    // Shift+F1: CSI 1;2 P
    input.generateKey(Key::F1, Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1;2P"sv));

    // Ctrl+F1: CSI 1;5 P
    input.generateKey(Key::F1, Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1;5P"sv));
}

TEST_CASE("StandardKeyboardInputGenerator.Legacy.F1_F4_with_modifiers", "[terminal,input]")
{
    auto input = InputGenerator {};

    // Shift+F1 (legacy): CSI 1;2 P
    input.generate(Key::F1, Modifier::Shift, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[1;2P"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Ctrl+F3 (legacy): CSI 13;5 ~
    input.generate(Key::F3, Modifier::Control, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[13;5~"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // Ctrl+F4 (legacy): CSI 1;5 S
    input.generate(Key::F4, Modifier::Control, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[1;5S"sv));
    input.consume(static_cast<int>(input.peek().size()));
}

TEST_CASE("ExtendedKeyboardInputGenerator.ReportEventTypes_alone", "[terminal,input]")
{
    // ReportEventTypes alone (flag=0b10) should trigger CSI u for non-Press events
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::ReportEventTypes);

    // 'a' Press: legacy (no mods, no action encoding for Press)
    input.generateChar('a', 'a', Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("a"sv));

    // 'a' Repeat: CSI 97;1:2 u (needs CSI u for action encoding)
    input.generateChar('a', 'a', Modifier::None, KeyboardEventType::Repeat);
    REQUIRE(escape(input.take()) == escape("\033[97;1:2u"sv));

    // 'a' Release: CSI 97;1:3 u
    input.generateChar('a', 'a', Modifier::None, KeyboardEventType::Release);
    REQUIRE(escape(input.take()) == escape("\033[97;1:3u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.ReportAllKeys_alone", "[terminal,input]")
{
    // ReportAllKeysAsEscapeCodes alone (flag=0b1000)
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // 'a' Press: CSI 97 u
    input.generateChar('a', 'a', Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[97u"sv));

    // Enter Press: CSI 13 u
    input.generateKey(Key::Enter, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[13u"sv));

    // Tab Press: CSI 9 u
    input.generateKey(Key::Tab, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[9u"sv));

    // LeftShift Press: CSI 57441 u
    input.generateKey(Key::LeftShift, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[57441u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Shift_only_with_disambiguate", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // Shift+'a' with disambiguate: CSI 97;2 u (Shift is a real modifier)
    input.generateChar('A', 'a', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[97;2u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.LockModifier_handling", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // CapsLock+Enter: legacy (\r) because only lock modifiers
    input.generateKey(Key::Enter, Modifier::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\r"sv));

    // NumLock+Tab: legacy (\t) because only lock modifiers
    input.generateKey(Key::Tab, Modifier::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\t"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.LockModifier_with_ReportAllKeys", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // CapsLock+Enter with ReportAllKeys: CSI 13;65 u
    input.generateKey(Key::Enter, Modifier::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[13;65u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Enter_Tab_Backspace_with_modifiers", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // Shift+Enter: CSI 13;2 u
    input.generateKey(Key::Enter, Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[13;2u"sv));

    // Shift+Tab: CSI 9;2 u
    input.generateKey(Key::Tab, Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[9;2u"sv));

    // Ctrl+Backspace: CSI 127;5 u
    input.generateKey(Key::Backspace, Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[127;5u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.EventType_encoding", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportEventTypes);

    // Press with mods: no :1 suffix (Press is default, omitted per spec)
    input.generateChar('a', 'a', Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[97;5u"sv));

    // Repeat with mods: :2 suffix
    input.generateChar('a', 'a', Modifier::Control, KeyboardEventType::Repeat);
    REQUIRE(escape(input.take()) == escape("\033[97;5:2u"sv));

    // Release with mods: :3 suffix
    input.generateChar('a', 'a', Modifier::Control, KeyboardEventType::Release);
    REQUIRE(escape(input.take()) == escape("\033[97;5:3u"sv));
}

// }}}
