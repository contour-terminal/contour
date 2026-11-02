// SPDX-License-Identifier: Apache-2.0
//
// The Windows normalizer and its US virtual-key table, on any host: AltGr is a layout level and not a chord,
// the lock toggles ride in the sampled native modifiers, and OEM punctuation resolves through the layout
// instead of being read as a codepoint.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>

#include <QtCore/Qt>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
using contour::config::KeyboardPlatformKind;
using contour::input::CtrlAltRole;
using contour::input::InputMethodRoute;
using contour::input::NativeKeyCode;
using contour::input::NativeKeyEvent;
using vtbackend::LockKey;
using vtbackend::LockKeys;
using vtbackend::Modifier;
using vtbackend::Modifiers;
using namespace contour::test::keybits;

constexpr auto CtrlAlt = Ctrl | Alt;

/// @return The Windows normalizer over the US virtual-key table.
[[nodiscard]] std::unique_ptr<contour::input::KeyboardPlatform> win32Platform()
{
    static auto const layout = contour::input::makeWin32UsKeyboardLayout();
    return contour::input::makeKeyboardPlatform(KeyboardPlatformKind::Win32, {}, *layout);
}
} // namespace

TEST_CASE("the US VK table resolves OEM punctuation", "[input][platform][win32]")
{
    auto const layout = contour::input::makeWin32UsKeyboardLayout();
    auto const resolve = [&](uint32_t virtualKey) {
        return layout->unshiftedKeyOf(NativeKeyCode { .virtualKey = virtualKey });
    };

    CHECK(resolve(0xBA) == U';');  // VK_OEM_1
    CHECK(resolve(0xBB) == U'=');  // VK_OEM_PLUS
    CHECK(resolve(0xBC) == U',');  // VK_OEM_COMMA
    CHECK(resolve(0xBD) == U'-');  // VK_OEM_MINUS
    CHECK(resolve(0xBE) == U'.');  // VK_OEM_PERIOD
    CHECK(resolve(0xBF) == U'/');  // VK_OEM_2
    CHECK(resolve(0xC0) == U'`');  // VK_OEM_3
    CHECK(resolve(0xDB) == U'[');  // VK_OEM_4
    CHECK(resolve(0xDC) == U'\\'); // VK_OEM_5
    CHECK(resolve(0xDD) == U']');  // VK_OEM_6
    CHECK(resolve(0xDE) == U'\''); // VK_OEM_7
    CHECK(resolve(0x41) == U'a');
    CHECK(resolve(0x5A) == U'z');
    CHECK(resolve(0x30) == U'0');
    CHECK(resolve(0x39) == U'9');
    CHECK(resolve(0x20) == U' ');
    CHECK(resolve(0x65) == U'5'); // VK_NUMPAD5
    CHECK(resolve(0x6B) == U'+'); // VK_ADD
    // Keys that carry no character resolve to nothing.
    CHECK(resolve(0x70) == 0); // VK_F1
    CHECK(resolve(0x25) == 0); // VK_LEFT
    CHECK(resolve(0) == 0);
}

TEST_CASE("win32VirtualKeyOf passes the VK through", "[input][platform][win32]")
{
    auto const layout = contour::input::makeWin32UsKeyboardLayout();
    CHECK(layout->win32VirtualKeyOf(NativeKeyCode { .virtualKey = 0xBA }) == 0xBA);
    CHECK(layout->win32VirtualKeyOf(NativeKeyCode { .virtualKey = 0x41, .scanCode = 0x1E }) == 0x41);
    CHECK(layout->win32VirtualKeyOf(NativeKeyCode {}) == 0);
}

TEST_CASE("ctrlAltRoleOf tells AltGr from a Ctrl+Alt chord by the text produced", "[input][platform][win32]")
{
    using contour::input::ctrlAltRoleOf;

    // The AltGr level of a layout yields a graphical character -- ASCII or not.
    CHECK(ctrlAltRoleOf(CtrlAlt, U"[") == CtrlAltRole::AltGr);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"@") == CtrlAltRole::AltGr);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"\\") == CtrlAltRole::AltGr);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"€") == CtrlAltRole::AltGr); // €
    CHECK(ctrlAltRoleOf(CtrlAlt | Shift, U"{") == CtrlAltRole::AltGr);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"\U0001F600") == CtrlAltRole::AltGr); // 😀
    // A ligature entry, or a dead key that did not combine with the next character.
    CHECK(ctrlAltRoleOf(CtrlAlt, U"ab") == CtrlAltRole::AltGr);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"´e") == CtrlAltRole::AltGr);

    // A genuine chord yields nothing, or a control character (Ctrl+Alt+Esc is 0x1B).
    CHECK(ctrlAltRoleOf(CtrlAlt, U"") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"\x1b") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"\x7f") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"\u009B") == CtrlAltRole::Chord); // C1 CSI
    CHECK(ctrlAltRoleOf(CtrlAlt, U" ") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(CtrlAlt, U"a\x1b") == CtrlAltRole::Chord);

    // Without both modifiers there is no AltGr to recognize.
    CHECK(ctrlAltRoleOf(Ctrl, U"[") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(Alt, U"[") == CtrlAltRole::Chord);
    CHECK(ctrlAltRoleOf(0, U"[") == CtrlAltRole::Chord);
}

TEST_CASE("the Win32 platform strips Ctrl+Alt only when it selected the character",
          "[input][platform][win32]")
{
    auto const platform = win32Platform();

    // Swedish AltGr+8 is '[': Ctrl and Alt are the layout level, not a chord (#2127).
    auto const altGr =
        platform->normalize({ .qtKey = '[', .text = U"[", .qtModifiers = CtrlAlt, .nativeVirtualKey = 0x38 });
    CHECK(altGr.text == U"[");
    CHECK(altGr.modifiers.chord.none());
    CHECK(altGr.identity.nativeVirtualKey == 0x38);

    // A genuine Ctrl+Alt+A yields no text and keeps both modifiers.
    auto const chord =
        platform->normalize({ .qtKey = 'A', .qtModifiers = CtrlAlt, .nativeVirtualKey = 0x41 });
    CHECK(chord.modifiers.chord == Modifiers { Modifier::Control, Modifier::Alt });

    // AltGr with Shift keeps the Shift.
    auto const shifted = platform->normalize(
        { .qtKey = '{', .text = U"{", .qtModifiers = CtrlAlt | Shift, .nativeVirtualKey = 0x37 });
    CHECK(shifted.modifiers.chord == Modifiers { Modifier::Shift });
}

TEST_CASE("the Win32 platform decodes the lock toggles the capture sampled", "[input][platform][win32]")
{
    auto const platform = win32Platform();
    // A letter: a keypad key would infer NumLock from being a keypad digit.
    auto const letter = NativeKeyEvent { .qtKey = 'X', .text = U"x" };

    auto caps = letter;
    caps.nativeModifiers = contour::input::Win32CapsLockBit;
    CHECK(platform->normalize(caps).modifiers.locks.contains(LockKey::CapsLock));

    auto num = letter;
    num.nativeModifiers = contour::input::Win32NumLockBit;
    CHECK(platform->normalize(num).modifiers.locks.contains(LockKey::NumLock));

    // The Win32 bits are not the X11 ones: 0x10 (X11's NumLock) means nothing here.
    auto other = letter;
    other.nativeModifiers = XcbNumLock;
    CHECK(platform->normalize(other).modifiers.locks.none());

    STATIC_CHECK(contour::input::win32LockKeysOf(0x3).contains(LockKey::CapsLock));
    STATIC_CHECK(contour::input::win32LockKeysOf(0x3).contains(LockKey::NumLock));
    STATIC_CHECK(contour::input::win32LockKeysOf(0).none());
}

TEST_CASE("every keystroke reaches the Win32 input method", "[input][platform][win32]")
{
    CHECK(win32Platform()->inputMethodRouteFor(0) == InputMethodRoute::ToInputMethod);
    CHECK(win32Platform()->inputMethodRouteFor(MacLeftOption) == InputMethodRoute::ToInputMethod);
}

TEST_CASE("a key lookup decides AltGr as the terminal does", "[input][platform][win32]")
{
    using contour::input::chordOfKeyPress;

    // A chrome binding on Ctrl+Alt+[ .
    auto mappings = contour::config::InputMappings {};
    mappings.charMappings.push_back(
        contour::config::CharInputMapping { .modes {},
                                            .modifiers = Modifiers { Modifier::Control, Modifier::Alt },
                                            .input = U'[',
                                            .binding = { contour::actions::OpenCommandPalette {} } });
    auto const lookUp = [&](std::u32string_view text) {
        auto const chord = chordOfKeyPress(*win32Platform(), '[', CtrlAlt, text);
        return contour::config::apply(mappings.charMappings, U'[', chord, uint8_t { 0 }) != nullptr;
    };

    // Swedish AltGr+8 is '[': Ctrl+Alt is the layout level, so the chord binding does not match.
    CHECK_FALSE(lookUp(U"["));
    // A genuine Ctrl+Alt+[ yields a control character, or nothing.
    CHECK(lookUp(U"\x1b"));
    CHECK(lookUp(U""));
}
