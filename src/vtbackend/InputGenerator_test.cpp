// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/InputBinding.h>
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

    // Home with no modifiers: CSI H (code=1 omitted per Kitty spec)
    input.generateKey(Key::Home, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[H"sv));

    // Ctrl+Home: CSI 1;5 H
    input.generateKey(Key::Home, Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[1;5H"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.End", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // End with no modifiers: CSI F (code=1 omitted per Kitty spec)
    input.generateKey(Key::End, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[F"sv));

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

    // F1 no mods: CSI P (code=1 omitted per Kitty spec)
    input.generateKey(Key::F1, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[P"sv));

    // F2 no mods: CSI Q (code=1 omitted per Kitty spec)
    input.generateKey(Key::F2, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[Q"sv));

    // F3 no mods: CSI 13 ~ (tilde-form to avoid CSI R conflict)
    input.generateKey(Key::F3, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[13~"sv));

    // F4 no mods: CSI S (code=1 omitted per Kitty spec)
    input.generateKey(Key::F4, Modifier::None, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[S"sv));

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

TEST_CASE("ExtendedKeyboardInputGenerator.legacy.Shift_only_with_disambiguate", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // Shift+'a' with disambiguate only: legacy 'A' (Shift+printable is unambiguous)
    input.generateChar('A', 'a', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("A"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.legacy.colon_with_disambiguate_only", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    // Shift+';' -> ':' with DisambiguateEscapeCodes only: must send ':' (not CSI u)
    input.generateChar(':', ';', Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape(":"sv));
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

// {{{ Legacy multi-modifier tests (Kitty keyboard protocol cross-check)

TEST_CASE("InputGenerator.Legacy.Shift_Alt_letter", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Shift+Alt+'a' → platform sends 'A' with Shift+Alt → should send ESC + 'A'
    input.generate(
        static_cast<char32_t>('A'), Modifiers { Modifier::Shift } | Modifier::Alt, KeyboardEventType::Press);
    REQUIRE(escape(input.peek())
            == escape("\x1b"
                      "A"sv));
}

TEST_CASE("InputGenerator.Legacy.Ctrl_Alt_letter", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Ctrl+Alt+'a' → platform sends 'A' with Ctrl+Alt → should send ESC + C0
    input.generate(static_cast<char32_t>('A'),
                   Modifiers { Modifier::Control } | Modifier::Alt,
                   KeyboardEventType::Press);
    auto const expected = string("\x1b") + string(1, '\x01');
    REQUIRE(escape(input.peek()) == escape(expected));
}

TEST_CASE("InputGenerator.Legacy.Alt_Shift_Tab", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Alt+Shift+Tab → should send ESC + backtab
    input.generate(
        static_cast<char32_t>(0x09), Modifiers { Modifier::Alt } | Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x1b\x1b[Z"sv));
}

TEST_CASE("InputGenerator.Legacy.Ctrl_Shift_Tab", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Ctrl+Shift+Tab → backtab (Ctrl ignored for backtab)
    input.generate(static_cast<char32_t>(0x09),
                   Modifiers { Modifier::Control } | Modifier::Shift,
                   KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x1b[Z"sv));
}

TEST_CASE("InputGenerator.Legacy.Ctrl_Alt_Tab", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Ctrl+Alt+Tab → ESC + Tab
    input.generate(Key::Tab, Modifiers { Modifier::Control } | Modifier::Alt, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x1b\t"sv));
}

TEST_CASE("InputGenerator.Legacy.Alt_Escape", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Alt+Escape → ESC ESC
    input.generate(Key::Escape, Modifier::Alt, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x1b\x1b"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Ctrl_Shift_letter", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    // Ctrl+Shift+'i' is unrepresentable in legacy → CSI u fallback: ESC[105;6u
    input.generateChar('I', 'i', Modifiers { Modifier::Control } | Modifier::Shift, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\x1b[105;6u"sv));
}

TEST_CASE("InputGenerator.Legacy.Ctrl_slash", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Ctrl+/ → 0x1F (Kitty ctrled_key mapping)
    input.generate(static_cast<char32_t>('/'), Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x1f"sv));
}

TEST_CASE("InputGenerator.Legacy.Ctrl_at", "[terminal,input]")
{
    auto input = InputGenerator {};
    // Ctrl+@ → 0x00 (Kitty ctrled_key mapping)
    input.generate(static_cast<char32_t>('@'), Modifier::Control, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\x00"sv));
}

// }}}

// {{{ Numpad + NumLock tests

TEST_CASE("InputGenerator.Numpad_with_NumLock_overrides_DECKPAM", "[terminal,input]")
{
    // When NumLock is active, numpad digit keys must produce their literal characters
    // even in application keypad mode (DECKPAM), not escape sequences.
    auto input = InputGenerator {};
    input.setApplicationKeypadMode(true);
    REQUIRE(input.applicationKeypad());

    auto constexpr NumLock = Modifiers { Modifier::NumLock };

    struct Mapping
    {
        Key key;
        string_view expected;
    };

    auto constexpr Mappings = std::array {
        Mapping { .key = Key::Numpad_0, .expected = "0"sv },
        Mapping { .key = Key::Numpad_1, .expected = "1"sv },
        Mapping { .key = Key::Numpad_2, .expected = "2"sv },
        Mapping { .key = Key::Numpad_3, .expected = "3"sv },
        Mapping { .key = Key::Numpad_4, .expected = "4"sv },
        Mapping { .key = Key::Numpad_5, .expected = "5"sv },
        Mapping { .key = Key::Numpad_6, .expected = "6"sv },
        Mapping { .key = Key::Numpad_7, .expected = "7"sv },
        Mapping { .key = Key::Numpad_8, .expected = "8"sv },
        Mapping { .key = Key::Numpad_9, .expected = "9"sv },
        Mapping { .key = Key::Numpad_Decimal, .expected = "."sv },
        Mapping { .key = Key::Numpad_Divide, .expected = "/"sv },
        Mapping { .key = Key::Numpad_Multiply, .expected = "*"sv },
        Mapping { .key = Key::Numpad_Add, .expected = "+"sv },
        Mapping { .key = Key::Numpad_Subtract, .expected = "-"sv },
        Mapping { .key = Key::Numpad_Enter, .expected = "\r"sv },
        Mapping { .key = Key::Numpad_Equal, .expected = "="sv },
    };

    for (auto const& mapping: Mappings)
    {
        INFO(std::format("Testing {}+NumLock in DECKPAM => {}", mapping.key, escape(mapping.expected)));
        input.generate(mapping.key, NumLock, KeyboardEventType::Press);
        CHECK(escape(input.peek()) == escape(mapping.expected));
        input.consume(static_cast<int>(input.peek().size()));
    }
}

TEST_CASE("InputGenerator.Numpad_without_NumLock_in_DECKPAM", "[terminal,input]")
{
    // Without NumLock, numpad keys in DECKPAM must send their application keypad sequences.
    auto input = InputGenerator {};
    input.setApplicationKeypadMode(true);
    REQUIRE(input.applicationKeypad());

    // KP_5 without NumLock in DECKPAM → CSI E
    input.generate(Key::Numpad_5, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[E"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Numpad5_with_NumLock", "[terminal,input]")
{
    // KP_5 + NumLock in Kitty disambiguate mode:
    // code=57404, modifier=1+128=129, final='u'
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);

    input.generateKey(Key::Numpad_5, Modifier::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[57404;129u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Numpad5_AssociatedText", "[terminal,input]")
{
    // KP_5 + NumLock with ReportAssociatedText enabled:
    // code=57404, modifier=129, associated_text=53 ('5'), final='u'
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAssociatedText);

    input.generateKey(Key::Numpad_5, Modifier::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[57404;129;53u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Numpad_AssociatedText_no_mods", "[terminal,input]")
{
    // KP_5 without modifiers but with ReportAssociatedText:
    // code=57404, modifier=1 (default, needed for text parameter), associated_text=53, final='u'
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAssociatedText);

    input.generateKey(Key::Numpad_5, Modifiers {}, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[57404;1;53u"sv));
}

// }}}

// {{{ InputBinding lock modifier tests
// These tests verify that lock modifiers (NumLock/CapsLock) cause matching failures
// when not stripped, and that stripping them via without(LockModifiers) restores correct
// matching — the same pattern used by config::apply().

TEST_CASE("InputBinding.match_with_NumLock_stripped", "[terminal,input]")
{
    auto const binding = InputBinding<Key, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Shift } | Modifier::Control,
        .input = Key::Enter,
        .binding = 42,
    };
    auto const modsWithNumLock = Modifiers { Modifier::Shift } | Modifier::Control | Modifier::NumLock;
    // NumLock causes a false negative — the binding should match but doesn't without stripping
    CHECK_FALSE(match(binding, MatchModes {}, modsWithNumLock, Key::Enter));
    // After stripping lock modifiers, the binding matches correctly
    CHECK(match(binding, MatchModes {}, modsWithNumLock.without(LockModifiers), Key::Enter));
}

TEST_CASE("InputBinding.match_with_CapsLock_stripped", "[terminal,input]")
{
    auto const binding = InputBinding<Key, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Control },
        .input = Key::F5,
        .binding = 1,
    };
    auto const modsWithCapsLock = Modifiers { Modifier::Control } | Modifier::CapsLock;
    // CapsLock causes a false negative
    CHECK_FALSE(match(binding, MatchModes {}, modsWithCapsLock, Key::F5));
    // After stripping lock modifiers, the binding matches correctly
    CHECK(match(binding, MatchModes {}, modsWithCapsLock.without(LockModifiers), Key::F5));
}

TEST_CASE("InputBinding.match_with_NumLock_and_CapsLock_stripped", "[terminal,input]")
{
    auto const binding = InputBinding<char32_t, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Shift } | Modifier::Control,
        .input = U'N',
        .binding = 0,
    };
    auto const modsWithBothLocks =
        Modifiers { Modifier::Shift } | Modifier::Control | Modifier::NumLock | Modifier::CapsLock;
    // Both lock modifiers cause a false negative
    CHECK_FALSE(match(binding, MatchModes {}, modsWithBothLocks, U'N'));
    // After stripping lock modifiers, the binding matches correctly
    CHECK(match(binding, MatchModes {}, modsWithBothLocks.without(LockModifiers), U'N'));
}

TEST_CASE("InputBinding.match_still_requires_real_modifiers", "[terminal,input]")
{
    auto const binding = InputBinding<Key, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Shift } | Modifier::Control,
        .input = Key::Enter,
        .binding = 42,
    };
    // NumLock alone, after stripping locks, leaves no modifiers — should NOT match Shift+Control
    CHECK_FALSE(
        match(binding, MatchModes {}, Modifiers { Modifier::NumLock }.without(LockModifiers), Key::Enter));
    // No modifiers should NOT match
    CHECK_FALSE(match(binding, MatchModes {}, Modifiers {}, Key::Enter));
}

// }}}
// {{{ Win32 Input Mode tests

TEST_CASE("InputGenerator.Win32InputMode.character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'a' (VK=0x41 on Windows, passed as physicalKey) with no modifiers
    // Expected: CSI 65;0;97;1;0;1_
    input.generate(U'a', 0x41, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.character_with_shift", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'A' (Shift+a), VK=0x41, unicode='A'=65, Shift pressed -> CS=0x0010
    input.generate(U'A', 0x41, Modifier::Shift, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;65;1;16;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.character_with_ctrl", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+L: VK=0x4C, unicode char is Ctrl-mapped: 'l' -> 0x0C (12), Ctrl pressed -> CS=0x0008
    input.generate(U'l', 0x4C, Modifier::Control, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[76;0;12;1;8;1_"); // UC=12 (0x0C), not 108 ('l')
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.arrow_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Up arrow: VK_UP=0x26, no unicode char, no modifiers, ENHANCED_KEY=0x0100=256
    input.generate(Key::UpArrow, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[38;0;0;1;256;1_"); // VK_UP=0x26=38, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.function_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // F5: VK_F5=0x74=116, no unicode char, no modifiers
    input.generate(Key::F5, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[116;0;0;1;0;1_"); // VK_F5=0x74=116
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.key_release", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Key release should set Kd=0
    input.generate(U'a', 0x41, Modifier::None, KeyboardEventType::Release);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;0;0;1_"); // Kd=0 for release
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.combined_modifiers", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+Alt+Delete: VK_DELETE=0x2E=46
    // CS = LeftAlt(0x0002) | LeftCtrl(0x0008) | ENHANCED_KEY(0x0100) = 0x010A = 266
    auto const ctrlAlt = Modifiers { Modifier::Control } | Modifiers { Modifier::Alt };
    input.generate(Key::Delete, ctrlAlt, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[46;0;0;1;266;1_"); // CS=266 (0x010A)
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.supersedes_csiu", "[terminal,input]")
{
    auto input = InputGenerator {};

    // Enable both CSI u (Kitty keyboard protocol) and Win32 Input Mode.
    // Win32 Input Mode should take precedence.
    input.keyboardProtocol().enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.setWin32InputMode(true);

    // Typing 'a' should produce Win32 format, not CSI u format
    input.generate(U'a', 0x41, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;0;1_"); // Win32 format, NOT "\033[97u"
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.reset_clears_mode", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);
    CHECK(input.win32InputMode() == true);

    input.reset();
    CHECK(input.win32InputMode() == false);

    // After reset, should generate legacy format, not Win32
    input.generate(U'a', 0x41, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "a"); // Legacy: plain character
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.enter_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Enter key: VK_RETURN=0x0D=13, unicode='\r'=13
    input.generate(Key::Enter, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[13;0;0;1;0;1_"); // VK_RETURN via Key path (no unicode char)
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.escape_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Escape: VK_ESCAPE=0x1B=27
    input.generate(Key::Escape, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[27;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.alt_press", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Alt press: VK_MENU=0x12=18 (generic, not VK_LMENU=0xA4), CS=LeftAltPressed(0x0002)
    input.generate(Key::LeftAlt, Modifier::Alt, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[18;0;0;1;2;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.alt_release", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Alt release: self-modifier stripped from CS, so CS=0 (not LeftAltPressed)
    input.generate(Key::LeftAlt, Modifier::Alt, KeyboardEventType::Release);
    auto const seq = input.peek();
    CHECK(seq == "\033[18;0;0;0;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_press", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl press: VK_CONTROL=0x11=17, CS=LeftCtrlPressed(0x0008)
    input.generate(Key::LeftControl, Modifier::Control, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[17;0;0;1;8;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_release", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl release: self-modifier stripped from CS
    input.generate(Key::LeftControl, Modifier::Control, KeyboardEventType::Release);
    auto const seq = input.peek();
    CHECK(seq == "\033[17;0;0;0;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.shift_press", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Shift press: VK_SHIFT=0x10=16, CS=ShiftPressed(0x0010)
    input.generate(Key::LeftShift, Modifier::Shift, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[16;0;0;1;16;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.shift_release", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Shift release: self-modifier stripped from CS
    input.generate(Key::LeftShift, Modifier::Shift, KeyboardEventType::Release);
    auto const seq = input.peek();
    CHECK(seq == "\033[16;0;0;0;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

// {{{ Win32 Input Mode — Navigation keys with ENHANCED_KEY

TEST_CASE("InputGenerator.Win32InputMode.down_arrow", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Down arrow: VK_DOWN=0x28=40, ENHANCED_KEY=0x0100=256
    input.generate(Key::DownArrow, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[40;0;0;1;256;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.left_arrow", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::LeftArrow, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[37;0;0;1;256;1_"); // VK_LEFT=0x25=37, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.right_arrow", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::RightArrow, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[39;0;0;1;256;1_"); // VK_RIGHT=0x27=39, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.home_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::Home, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[36;0;0;1;256;1_"); // VK_HOME=0x24=36, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.end_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::End, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[35;0;0;1;256;1_"); // VK_END=0x23=35, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.insert_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::Insert, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[45;0;0;1;256;1_"); // VK_INSERT=0x2D=45, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.delete_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::Delete, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[46;0;0;1;256;1_"); // VK_DELETE=0x2E=46, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.page_up", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::PageUp, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[33;0;0;1;256;1_"); // VK_PRIOR=0x21=33, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.page_down", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    input.generate(Key::PageDown, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[34;0;0;1;256;1_"); // VK_NEXT=0x22=34, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Numpad keys

TEST_CASE("InputGenerator.Win32InputMode.numpad_0", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad 0: VK_NUMPAD0=0x60=96, NOT enhanced
    input.generate(Key::Numpad_0, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[96;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_5", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad 5: VK_NUMPAD5=0x65=101, NOT enhanced
    input.generate(Key::Numpad_5, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[101;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_add", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad +: VK_ADD=0x6B=107, NOT enhanced
    input.generate(Key::Numpad_Add, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[107;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_enter", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad Enter: VK_RETURN=0x0D=13, IS enhanced (E0 scan code prefix)
    input.generate(Key::Numpad_Enter, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[13;0;0;1;256;1_"); // CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_divide", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad /: VK_DIVIDE=0x6F=111, IS enhanced (E0 scan code prefix)
    input.generate(Key::Numpad_Divide, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[111;0;0;1;256;1_"); // CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Lock modifiers in control key state

TEST_CASE("InputGenerator.Win32InputMode.capslock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'a' with CapsLock on: CS=CAPSLOCK_ON(0x0080)=128
    input.generate(U'a', 0x41, Modifier::CapsLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;128;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numlock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'a' with NumLock on: CS=NUMLOCK_ON(0x0020)=32
    input.generate(U'a', 0x41, Modifier::NumLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;32;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.capslock_and_numlock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Both locks: CS=CAPSLOCK_ON|NUMLOCK_ON = 0x0080|0x0020 = 0x00A0 = 160
    auto const locks = Modifiers { Modifier::CapsLock } | Modifiers { Modifier::NumLock };
    input.generate(U'a', 0x41, locks, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;160;1_");
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Tab and Backspace

TEST_CASE("InputGenerator.Win32InputMode.tab_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Tab: VK_TAB=0x09=9, NOT enhanced
    input.generate(Key::Tab, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[9;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.backspace_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Backspace: VK_BACK=0x08=8, NOT enhanced
    input.generate(Key::Backspace, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[8;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Multi-modifier combinations

TEST_CASE("InputGenerator.Win32InputMode.shift_alt_character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Shift+Alt+'a': CS = ShiftPressed(0x0010) | LeftAltPressed(0x0002) = 0x0012 = 18
    auto const mods = Modifiers { Modifier::Shift } | Modifiers { Modifier::Alt };
    input.generate(U'a', 0x41, mods, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;18;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_shift_character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+Shift+'a': CS = LeftCtrlPressed(0x0008) | ShiftPressed(0x0010) = 0x0018 = 24
    // UC = Ctrl-mapped 'a' -> 0x01 = 1
    auto const mods = Modifiers { Modifier::Control } | Modifiers { Modifier::Shift };
    input.generate(U'a', 0x41, mods, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;1;1;24;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_shift_alt_character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+Shift+Alt+'a': CS = LeftCtrl(0x0008) | Shift(0x0010) | LeftAlt(0x0002) = 0x001A = 26
    // UC = Ctrl-mapped 'a' -> 0x01 = 1
    auto const mods =
        Modifiers { Modifier::Control } | Modifiers { Modifier::Shift } | Modifiers { Modifier::Alt };
    input.generate(U'a', 0x41, mods, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;1;1;26;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_alt_arrow", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+Alt+Left: CS = LeftCtrl(0x0008) | LeftAlt(0x0002) | ENHANCED_KEY(0x0100) = 0x010A = 266
    auto const mods = Modifiers { Modifier::Control } | Modifiers { Modifier::Alt };
    input.generate(Key::LeftArrow, mods, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[37;0;0;1;266;1_");
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Repeat event type

TEST_CASE("InputGenerator.Win32InputMode.repeat_event", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Repeat event should produce Kd=1 (same as Press, since any non-Release is key-down)
    input.generate(U'a', 0x41, Modifier::None, KeyboardEventType::Repeat);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;0;1_"); // Kd=1
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — F-keys with modifiers

TEST_CASE("InputGenerator.Win32InputMode.shift_f5", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Shift+F5: VK_F5=0x74=116, CS=ShiftPressed(0x0010)=16, NOT enhanced
    input.generate(Key::F5, Modifier::Shift, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[116;0;0;1;16;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_f1", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+F1: VK_F1=0x70=112, CS=LeftCtrlPressed(0x0008)=8, NOT enhanced
    input.generate(Key::F1, Modifier::Control, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[112;0;0;1;8;1_");
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Character variety

TEST_CASE("InputGenerator.Win32InputMode.space_character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Space: VK_SPACE=0x20=32, UC=' '=32
    input.generate(U' ', 0x20, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[32;0;32;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.digit_character", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Digit '1': VK='1'=0x31=49, UC='1'=49
    input.generate(U'1', 0x31, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[49;0;49;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.ctrl_c", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Ctrl+C: VK=0x43=67, UC = Ctrl-mapped 'c' -> 0x03 = 3, CS=LeftCtrl(0x0008)=8
    input.generate(U'c', 0x43, Modifier::Control, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[67;0;3;1;8;1_");
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Enhanced key negative cases

TEST_CASE("InputGenerator.Win32InputMode.enter_not_enhanced", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Main Enter key: VK_RETURN=0x0D=13, NOT enhanced (unlike Numpad Enter)
    input.generate(Key::Enter, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[13;0;0;1;0;1_"); // CS=0, no ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.f5_not_enhanced", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // F5: VK_F5=0x74=116, NOT enhanced (function keys don't have E0 prefix)
    input.generate(Key::F5, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[116;0;0;1;0;1_"); // CS=0, no ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — MediaStop regression

TEST_CASE("InputGenerator.Win32InputMode.media_stop_vk", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // MediaStop: VK_MEDIA_STOP=0xB2=178 (not 0xB3 which is VK_MEDIA_PLAY_PAUSE)
    input.generate(Key::MediaStop, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[178;0;0;1;0;1_"); // VK=178 (0xB2)
    input.consume(static_cast<int>(seq.size()));
}

// }}}

// }}}
