// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/InputBinding.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/test_helpers.h>

#include <crispy/escape.h>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <string>
#include <type_traits>

using namespace std;
using namespace vtbackend;
using namespace vtbackend::test;
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

TEST_CASE("InputGenerator.Modifier.chordModifierTable")
{
    SECTION("the derived constants follow the table")
    {
        STATIC_REQUIRE(ChordModifierTable.size() == 6);
        STATIC_REQUIRE(AllChordModifiers.value() == 0b11'1111);
        STATIC_REQUIRE(ChordModifierBitWidth == 6u);

        // The property ViInputHandler's InputMatch packs on: a full chord never reaches above the
        // shift, so it cannot alias onto the character it is packed next to.
        STATIC_REQUIRE((AllChordModifiers.value() >> ChordModifierBitWidth) == 0);
    }

    SECTION("every row names exactly one bit, ascending and without gaps")
    {
        auto expectedBit = 1u;
        for (auto const& row: ChordModifierTable)
        {
            CHECK(static_cast<unsigned>(row.modifier) == expectedBit);
            CHECK(AllChordModifiers.test(row.modifier));
            CHECK_FALSE(row.name.empty());
            expectedBit <<= 1u;
        }
    }

    SECTION("formatting a modifier reads its name from the table")
    {
        CHECK(std::format("{}", Modifier::None) == "None");
        for (auto const& row: ChordModifierTable)
            CHECK(std::format("{}", row.modifier) == row.name);
    }

    SECTION("a value naming no chord modifier formats empty")
    {
        // crispy::flags's formatter walks every bit position of the underlying type and skips the
        // ones that format empty. Were a lock bit to name itself here, it would leak into the
        // rendering of a Modifiers set.
        CHECK(std::format("{}", static_cast<Modifier>(LockKey::CapsLock)).empty());
        CHECK(std::format("{}", static_cast<Modifier>(LockKey::NumLock)).empty());

        CHECK(std::format("{}", Modifiers { Modifier::Shift, Modifier::Meta }) == "Shift|Meta");
    }
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

namespace
{
constexpr auto CtrlNumLock = KeyboardModifiers { Modifier::Control, NumLockOnly };
constexpr auto CtrlCapsLock = KeyboardModifiers { Modifier::Control, CapsLockOnly };
constexpr auto CtrlBothLocks = KeyboardModifiers { Modifier::Control, BothLocks };
constexpr auto ShiftNumLock = KeyboardModifiers { Modifier::Shift, NumLockOnly };
} // namespace

TEST_CASE("InputGenerator.Ctrl+D_with_NumLock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('D', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+A_with_NumLock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('A', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x01));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+C_with_NumLock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('C', CtrlNumLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x03));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.all_Ctrl_A_to_Z_with_NumLock", "[terminal,input]")
{
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
    auto input = InputGenerator {};
    input.generate('D', CtrlCapsLock, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+D_with_NumLock_and_CapsLock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('D', CtrlBothLocks, KeyboardEventType::Press);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Shift+Tab_with_NumLock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate(static_cast<char32_t>(0x09), ShiftNumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.peek()) == escape("\033[Z"sv));
}

namespace
{
/// A key and the byte sequence it must produce when no chord modifier is held.
struct UnmodifiedKeyMapping
{
    Key key;
    std::string_view expected;
};
} // namespace

// A key pressed while only CapsLock and/or NumLock are latched must encode exactly as the
// unmodified key. Before this was fixed, select() stripped only NumLock, so the lock bit reached
// makeVirtualTerminalParam() and UpArrow+CapsLock encoded as CSI 1;65 A (65 == 1 + CapsLock).
TEST_CASE("InputGenerator.function_keys_ignore_lock_modifiers", "[terminal,input]")
{
    auto constexpr Mappings = std::array {
        UnmodifiedKeyMapping { .key = Key::UpArrow, .expected = "\033[A"sv },
        UnmodifiedKeyMapping { .key = Key::DownArrow, .expected = "\033[B"sv },
        UnmodifiedKeyMapping { .key = Key::RightArrow, .expected = "\033[C"sv },
        UnmodifiedKeyMapping { .key = Key::LeftArrow, .expected = "\033[D"sv },
        UnmodifiedKeyMapping { .key = Key::Home, .expected = "\033[H"sv },
        UnmodifiedKeyMapping { .key = Key::End, .expected = "\033[F"sv },
        UnmodifiedKeyMapping { .key = Key::PageUp, .expected = "\033[5~"sv },
        UnmodifiedKeyMapping { .key = Key::PageDown, .expected = "\033[6~"sv },
        UnmodifiedKeyMapping { .key = Key::Insert, .expected = "\033[2~"sv },
        UnmodifiedKeyMapping { .key = Key::Delete, .expected = "\033[3~"sv },
        UnmodifiedKeyMapping { .key = Key::F1, .expected = "\033OP"sv },
        UnmodifiedKeyMapping { .key = Key::F5, .expected = "\033[15~"sv },
    };

    for (auto const& [key, expected]: Mappings)
        for (auto const locks: LockCombinations)
        {
            INFO(std::format("key {} with lock modifiers {}", key, locks));
            auto input = InputGenerator {};
            input.generate(key, locks, KeyboardEventType::Press);
            CHECK(escape(input.peek()) == escape(expected));
        }
}

// The application-cursor-keys fallback must be reachable with locks latched. Pre-fix the lock bit
// made select() take the .mods branch, so SS3 A was never emitted.
TEST_CASE("InputGenerator.application_cursor_keys_ignore_lock_modifiers", "[terminal,input]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto input = InputGenerator {};
        input.setCursorKeysMode(KeyMode::Application);
        input.generate(Key::UpArrow, locks, KeyboardEventType::Press);
        CHECK(escape(input.peek()) == escape("\033OA"sv));
    }
}

// Stripping locks must not swallow the chord modifiers that ride along with them.
TEST_CASE("InputGenerator.function_keys_keep_chord_modifiers_across_locks", "[terminal,input]")
{
    auto constexpr Control = Modifiers { Modifier::Control };

    // 5 == 1 + Modifier::Control, regardless of which locks are additionally latched.
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("Control with lock modifiers {}", locks));
        auto input = InputGenerator {};
        input.generate(Key::UpArrow, KeyboardModifiers { Control, locks }, KeyboardEventType::Press);
        CHECK(escape(input.peek()) == escape("\033[1;5A"sv));
    }

    // Shift stays a real modifier: 2 == 1 + Modifier::Shift.
    auto input = InputGenerator {};
    input.generate(
        Key::UpArrow, KeyboardModifiers { Modifier::Shift, LockKey::CapsLock }, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[1;2A"sv));
}

// XTerm's modifyOtherKeys mode 2 must treat lock modifiers as absent: a latched lock must neither
// make an otherwise unmodified key look modified, nor contribute to the encoded modifier parameter.
TEST_CASE("InputGenerator.modifyOtherKeys2_ignores_lock_modifiers", "[terminal,input]")
{
    auto const generateLowerA = [](KeyboardModifiers modifiers) {
        auto input = InputGenerator {};
        input.setModifyOtherKeys(2);
        input.generate(U'a', modifiers, KeyboardEventType::Press);
        return std::string { input.peek() };
    };

    // Locks alone leave a plain character alone (pre-fix: CSI 27;65;97~ for CapsLock).
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        CHECK(escape(generateLowerA(locks)) == escape("a"sv));
    }

    // 5 == 1 + Modifier::Control, whichever locks are latched (pre-fix: 69 with CapsLock).
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("Control with lock modifiers {}", locks));
        CHECK(escape(generateLowerA(KeyboardModifiers { Modifier::Control, locks }))
              == escape("\033[27;5;97~"sv));
    }

    // 6 == 1 + Modifier::Control + Modifier::Shift; chord modifiers survive lock stripping.
    CHECK(escape(generateLowerA(
              KeyboardModifiers { Modifiers { Modifier::Control, Modifier::Shift }, LockKey::CapsLock }))
          == escape("\033[27;6;97~"sv));

    // Shift alone never engages modifyOtherKeys, with or without locks.
    CHECK(escape(generateLowerA(KeyboardModifiers { Modifier::Shift, LockKey::NumLock })) == escape("a"sv));
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
    input.generateChar('A', 'a', LockKey::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[97;65u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.NumLock", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // '5' with NumLock (128) + ReportAllKeys, encoded = 1 + 128 = 129
    input.generateChar('5', '5', LockKey::NumLock, KeyboardEventType::Press);
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
    input.generateChar('A', 'a', LockKey::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("A"sv));

    // 'A' with CapsLock, Release event — needs action encoding, so CSI u kicks in
    // modifier = CapsLock (64), encoded = 1 + 64 = 65, event type = 3 (Release)
    input.generateChar('A', 'a', LockKey::CapsLock, KeyboardEventType::Release);
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
    input.generateKey(Key::Enter, LockKey::CapsLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\r"sv));

    // NumLock+Tab: legacy (\t) because only lock modifiers
    input.generateKey(Key::Tab, LockKey::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\t"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.LockModifier_with_ReportAllKeys", "[terminal,input]")
{
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAllKeysAsEscapeCodes);

    // CapsLock+Enter with ReportAllKeys: CSI 13;65 u
    input.generateKey(Key::Enter, LockKey::CapsLock, KeyboardEventType::Press);
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
        input.generate(mapping.key, NumLockOnly, KeyboardEventType::Press);
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

    input.generateKey(Key::Numpad_5, LockKey::NumLock, KeyboardEventType::Press);
    REQUIRE(escape(input.take()) == escape("\033[57404;129u"sv));
}

TEST_CASE("ExtendedKeyboardInputGenerator.CSIu.Numpad5_AssociatedText", "[terminal,input]")
{
    // KP_5 + NumLock with ReportAssociatedText enabled:
    // code=57404, modifier=129, associated_text=53 ('5'), final='u'
    auto input = ExtendedKeyboardInputGenerator {};
    input.enter(KeyboardEventFlag::DisambiguateEscapeCodes);
    input.flags().enable(KeyboardEventFlag::ReportAssociatedText);

    input.generateKey(Key::Numpad_5, LockKey::NumLock, KeyboardEventType::Press);
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

// {{{ InputBinding chord matching
// Lock modifiers used to break key-binding matching (#1901) because they shared a bitset with the
// chord modifiers, and InputBinding compares modifier sets exactly. They are now a distinct type
// and simply cannot appear in a binding's modifier set, so the failure mode is gone by
// construction. These assertions pin that construction.

static_assert(!std::is_constructible_v<Modifiers, LockKey>,
              "a lock key must not be representable as a chord modifier");
static_assert(!std::is_constructible_v<Modifiers, LockKeys>,
              "a set of lock keys must not be representable as a set of chord modifiers");
static_assert(std::is_convertible_v<Modifiers, KeyboardModifiers>,
              "a chord must widen implicitly to a full keyboard modifier state");
static_assert(!std::is_convertible_v<KeyboardModifiers, Modifiers>,
              "extracting the chord from a keyboard modifier state must be explicit");

TEST_CASE("InputBinding.match_ignores_lock_state", "[terminal,input]")
{
    auto const binding = InputBinding<Key, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Shift } | Modifier::Control,
        .input = Key::Enter,
        .binding = 42,
    };

    // However the locks are latched, config::apply() matches on the chord alone.
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock keys {}", locks));
        auto const modifiers = KeyboardModifiers { Modifiers { Modifier::Shift } | Modifier::Control, locks };
        CHECK(match(binding, MatchModes {}, modifiers.chord, Key::Enter));
    }
}

TEST_CASE("InputBinding.match_still_requires_chord_modifiers", "[terminal,input]")
{
    auto const binding = InputBinding<Key, int> {
        .modes = MatchModes {},
        .modifiers = Modifiers { Modifier::Shift } | Modifier::Control,
        .input = Key::Enter,
        .binding = 42,
    };
    // A latched lock leaves an empty chord, which must not match Shift+Control.
    CHECK_FALSE(match(binding, MatchModes {}, KeyboardModifiers { LockKey::NumLock }.chord, Key::Enter));
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
    CHECK(seq == "\033[13;0;13;1;0;1_"); // VK_RETURN via Key path, UC='\r'
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.escape_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Escape: VK_ESCAPE=0x1B=27, unicode=ESC=0x1B=27. ConPTY has no VK fallback for Escape, so the
    // Unicode-char field must carry 0x1B or applications (e.g. neovim) never see the Escape.
    input.generate(Key::Escape, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[27;0;27;1;0;1_");
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

    // Numpad 0 with NumLock latched (the state in which Qt reports a keypad digit): VK_NUMPAD0=
    // 0x60=96, unicode='0'=48, CS=NUMLOCK_ON(0x20)=32. The digit must ride in the Unicode-char
    // field because ConPTY cannot reconstruct it from the virtual-key code in numeric keypad mode.
    input.generate(Key::Numpad_0, LockKey::NumLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[96;0;48;1;32;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_0_without_numlock", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Without NumLock the numpad key is a navigation key and carries no character (Uc=0), matching
    // a real KEY_EVENT_RECORD and ToUnicodeEx.
    input.generate(Key::Numpad_0, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[96;0;0;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_5", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad 5 with NumLock latched: VK_NUMPAD5=0x65=101, unicode='5'=53, CS=NUMLOCK_ON(0x20)=32.
    input.generate(Key::Numpad_5, LockKey::NumLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[101;0;53;1;32;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_add", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad +: VK_ADD=0x6B=107, unicode='+'=43. Operators carry their character regardless of
    // NumLock (they are not a navigation key), so the char rides even without a NumLock latch.
    input.generate(Key::Numpad_Add, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[107;0;43;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_enter", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad Enter: VK_RETURN=0x0D=13, unicode=CR=0x0D=13, IS enhanced (E0 scan code prefix).
    input.generate(Key::Numpad_Enter, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[13;0;13;1;256;1_"); // Uc=CR, CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numpad_divide", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Numpad /: VK_DIVIDE=0x6F=111, unicode='/'=47, IS enhanced (E0 scan code prefix).
    input.generate(Key::Numpad_Divide, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[111;0;47;1;256;1_"); // Uc='/', CS=ENHANCED_KEY
    input.consume(static_cast<int>(seq.size()));
}

// }}}
// {{{ Win32 Input Mode — Lock modifiers in control key state

TEST_CASE("InputGenerator.Win32InputMode.capslock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'a' with CapsLock on: CS=CAPSLOCK_ON(0x0080)=128
    input.generate(U'a', 0x41, LockKey::CapsLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;128;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.numlock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Typing 'a' with NumLock on: CS=NUMLOCK_ON(0x0020)=32
    input.generate(U'a', 0x41, LockKey::NumLock, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[65;0;97;1;32;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.capslock_and_numlock_in_cs", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Both locks: CS=CAPSLOCK_ON|NUMLOCK_ON = 0x0080|0x0020 = 0x00A0 = 160
    auto const locks = LockKeys { LockKey::CapsLock } | LockKey::NumLock;
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

    // Tab: VK_TAB=0x09=9, UC=0x09='\t', NOT enhanced
    input.generate(Key::Tab, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[9;0;9;1;0;1_");
    input.consume(static_cast<int>(seq.size()));
}

TEST_CASE("InputGenerator.Win32InputMode.backspace_key", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.setWin32InputMode(true);

    // Backspace: VK_BACK=0x08=8, UC=0x08='\b', NOT enhanced
    input.generate(Key::Backspace, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[8;0;8;1;0;1_");
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

    // Main Enter key: VK_RETURN=0x0D=13, UC=0x0D='\r', NOT enhanced (unlike Numpad Enter)
    input.generate(Key::Enter, Modifier::None, KeyboardEventType::Press);
    auto const seq = input.peek();
    CHECK(seq == "\033[13;0;13;1;0;1_"); // CS=0, no ENHANCED_KEY
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

// {{{ mouse wheel (alternate-scroll)

namespace
{
// A representative on-screen cursor position for wheel events (the exact position is
// irrelevant for cursor-key translation, which ignores coordinates).
constexpr auto WheelPos = CellLocation { LineOffset(5), ColumnOffset(10) };
constexpr auto NoPixel = PixelCoordinate {};
} // namespace

TEST_CASE("InputGenerator.Wheel.NormalCursorKeys.emits_cursor_keys", "[terminal,input]")
{
    // Core of #1951: no mouse protocol (as in plain `less`/`most`), yet the wheel must
    // translate into cursor-up/down. Previously emitted nothing because of the protocol gate.
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);

    CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelDown, WheelPos, NoPixel, false));
    CHECK(escape(input.peek()) == "\\e[B");
    input.consume(static_cast<int>(input.peek().size()));

    CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelUp, WheelPos, NoPixel, false));
    CHECK(escape(input.peek()) == "\\e[A");
}

TEST_CASE("InputGenerator.Wheel.ApplicationCursorKeys.emits_SS3", "[terminal,input]")
{
    // DECCKM / ?1007 on the alternate screen selects application cursor keys (SS3 form).
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);

    CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelDown, WheelPos, NoPixel, false));
    CHECK(escape(input.peek()) == "\\eOB");
    input.consume(static_cast<int>(input.peek().size()));

    CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelUp, WheelPos, NoPixel, false));
    CHECK(escape(input.peek()) == "\\eOA");
}

TEST_CASE("InputGenerator.Wheel.Default.no_protocol_emits_nothing", "[terminal,input]")
{
    // Primary-screen default: wheel mode Default and no protocol -> nothing is generated,
    // so the frontend falls back to local scrollback scrolling.
    auto input = InputGenerator {};

    CHECK_FALSE(input.generateMousePress(Modifiers {}, MouseButton::WheelDown, WheelPos, NoPixel, false));
    CHECK(input.peek().empty());
}

TEST_CASE("InputGenerator.Wheel.PassiveTracking.no_cursor_keys", "[terminal,input]")
{
    // Passive mouse tracking must not be shadowed by the cursor-key translation.
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
    input.setPassiveMouseTracking(true);

    // No protocol + passive tracking: the wheel branch is skipped and the protocol gate stops
    // generation, so nothing is emitted (and certainly no cursor keys).
    CHECK_FALSE(input.generateMousePress(Modifiers {}, MouseButton::WheelDown, WheelPos, NoPixel, false));
    CHECK(input.peek().empty());
}

TEST_CASE("InputGenerator.Wheel.Horizontal.no_cursor_keys", "[terminal,input]")
{
    // Horizontal wheel has no cursor-key equivalent; with no protocol it emits nothing.
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);

    CHECK_FALSE(input.generateMousePress(Modifiers {}, MouseButton::WheelLeft, WheelPos, NoPixel, false));
    CHECK(input.peek().empty());
    CHECK_FALSE(input.generateMousePress(Modifiers {}, MouseButton::WheelRight, WheelPos, NoPixel, false));
    CHECK(input.peek().empty());
}

TEST_CASE("InputGenerator.Wheel.ProtocolActive.passes_through_as_SGR", "[terminal,input]")
{
    // With a protocol active (`less --mouse`, `ov`), setMouseProtocol resets the wheel mode to
    // Default, so the wheel is reported to the app (SGR) instead of translated. Passthrough guard.
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
    input.setMouseTransport(MouseTransport::SGR);
    input.setMouseProtocol(MouseProtocol::NormalTracking, true); // resets wheel mode to Default

    CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelDown, WheelPos, NoPixel, false));
    // SGR mouse report, not a cursor key.
    CHECK(escape(input.peek()).starts_with("\\e[<"));
}

TEST_CASE("InputGenerator.Wheel.ScrollMultiplier.repeats_cursor_keys", "[terminal,input]")
{
    // The scroll multiplier makes one wheel notch emit N cursor keys, matching the
    // primary-screen scrollback feel (default 3).
    auto input = InputGenerator {};
    input.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);

    SECTION("multiplier 3")
    {
        input.setMouseWheelScrollMultiplier(3);
        CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelUp, WheelPos, NoPixel, false));
        CHECK(escape(input.peek()) == "\\e[A\\e[A\\e[A");
    }

    SECTION("multiplier 0 is clamped to 1")
    {
        input.setMouseWheelScrollMultiplier(0);
        CHECK(input.generateMousePress(Modifiers {}, MouseButton::WheelUp, WheelPos, NoPixel, false));
        CHECK(escape(input.peek()) == "\\e[A");
    }
}

// }}}

// {{{ focus events (DECSET 1004)

// NB: bracket-per-tag. Catch2 reads "[terminal,input]" (used widely above) as ONE tag literally named
// `terminal,input`, so such cases are unreachable via a `[focus]` filter.
TEST_CASE("InputGenerator.focusEvents", "[terminal][input][focus]")
{
    // DECMode::FocusTracking (1004) gates focus notification. The return value is load-bearing:
    // Terminal::sendFocus{In,Out}Event keys its flushInput() on it, so "nothing was generated" must
    // read as false. generateFocusOutEvent used to return true on both branches, claiming an event
    // had been sent with the mode off.

    SECTION("mode off: nothing is generated and neither event claims to have been sent")
    {
        auto input = InputGenerator {};
        REQUIRE_FALSE(input.generateFocusEvents());

        CHECK_FALSE(input.generateFocusInEvent());
        CHECK_FALSE(input.generateFocusOutEvent());
        CHECK(input.peek().empty());
    }

    SECTION("mode on: focus-in emits CSI I")
    {
        auto input = InputGenerator {};
        input.setGenerateFocusEvents(true);

        CHECK(input.generateFocusInEvent());
        CHECK(escape(input.peek()) == "\\e[I");
    }

    SECTION("mode on: focus-out emits CSI O")
    {
        auto input = InputGenerator {};
        input.setGenerateFocusEvents(true);

        CHECK(input.generateFocusOutEvent());
        CHECK(escape(input.peek()) == "\\e[O");
    }
}

// }}}

// }}}

TEST_CASE("InputGenerator.reset.clears_keyboard_modes", "[terminal][input]")
{
    // RIS must leave the generator agreeing with Terminal's freshly-defaulted mode bitset. The
    // keyboard state used to be reset only as far as the CSIu protocol stack, so a hard reset left
    // application cursor keys (and friends) still in effect while the modes read as default.
    auto input = InputGenerator {};

    input.setCursorKeysMode(KeyMode::Application);
    input.setApplicationKeypadMode(true);
    input.setBackarrowKeyMode(true);
    input.setAutomaticNewLineMode(true);

    REQUIRE(input.applicationCursorKeys());
    REQUIRE(input.applicationKeypad());

    input.reset();

    CHECK(input.normalCursorKeys());
    CHECK(input.numericKeypad());

    // The generated bytes follow: UpArrow is the normal-mode CSI A again, not SS3 A.
    input.generate(Key::UpArrow, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\033[A"sv));
    input.consume(static_cast<int>(input.peek().size()));

    // DECBKM is back off too, so Backspace sends DEL rather than BS.
    input.generate(Key::Backspace, Modifiers {}, KeyboardEventType::Press);
    CHECK(escape(input.peek()) == escape("\x7F"sv));
}
