// SPDX-License-Identifier: Apache-2.0
//
// What each KeyboardPlatform makes of a native key event. The Xkb expectations are what the key path
// handed the session before the normalizers existed, row by row of the characterization table.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>

#include <QtCore/Qt>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
using contour::config::KeyboardPlatformKind;
using vtbackend::Key;
using vtbackend::LockKey;
using vtbackend::LockKeys;
using vtbackend::Modifier;
using vtbackend::Modifiers;

/// What a characterization row's event normalizes to.
struct ExpectedTranslation
{
    std::string_view row;        ///< The characterization row's name.
    std::optional<Key> key {};   ///< The named key; nullopt for a character event.
    std::u32string_view text {}; ///< The characters, for a character event.
    Modifiers chord {};
    LockKeys locks {};
    char32_t unshiftedKey = 0;
};

constexpr auto Ctrl = Modifiers { Modifier::Control };
constexpr auto Shift = Modifiers { Modifier::Shift };
constexpr auto Alt = Modifiers { Modifier::Alt };
constexpr auto CtrlShift = Modifiers { Modifier::Control, Modifier::Shift };
constexpr auto CapsLock = LockKeys { LockKey::CapsLock };
constexpr auto NumLock = LockKeys { LockKey::NumLock };

/// What the Linux key path passed to TerminalSession::sendKeyEvent/sendCharEvent for each Xkb row.
// clang-format off
constexpr auto XkbTranslations = std::array {
    ExpectedTranslation { .row = "xkb: printable ASCII", .text = U"x" },
    ExpectedTranslation { .row = "xkb: Shift+letter", .text = U"A", .chord = Shift },
    ExpectedTranslation { .row = "xkb: CapsLock letter under kitty report-all", .text = U"A",
                          .locks = CapsLock, .unshiftedKey = U'A' },
    // The Ctrl+ASCII rule: the Qt key's uppercase label, not the control character the platform typed.
    ExpectedTranslation { .row = "xkb: Ctrl+C with a selection is a binding", .text = U"C",
                          .chord = Ctrl },
    ExpectedTranslation { .row = "xkb: Ctrl+letter", .text = U"C", .chord = Ctrl },
    ExpectedTranslation { .row = "xkb: Ctrl+Shift+letter is a binding", .text = U"P",
                          .chord = CtrlShift },
    ExpectedTranslation { .row = "xkb: Alt+letter", .text = U"b", .chord = Alt },
    ExpectedTranslation { .row = "xkb: keypad digit with NumLock", .key = Key::Numpad_5, .locks = NumLock },
    ExpectedTranslation { .row = "xkb: keypad End without NumLock", .key = Key::End },
    ExpectedTranslation { .row = "xkb: keypad Enter", .key = Key::Numpad_Enter, .locks = NumLock },
    ExpectedTranslation { .row = "xkb: Backtab", .key = Key::Tab, .chord = Shift },
    ExpectedTranslation { .row = "xkb: F1", .key = Key::F1 },
    ExpectedTranslation { .row = "xkb: F12", .key = Key::F12 },
    ExpectedTranslation { .row = "xkb: Shift+Up is a binding", .key = Key::UpArrow, .chord = Shift },
    ExpectedTranslation { .row = "xkb: Ctrl+Left", .key = Key::LeftArrow, .chord = Ctrl },
    ExpectedTranslation { .row = "xkb: Ctrl+Up", .key = Key::UpArrow, .chord = Ctrl },
    ExpectedTranslation { .row = "xkb: Alt+Left", .key = Key::LeftArrow, .chord = Alt },
    ExpectedTranslation { .row = "xkb: Escape", .key = Key::Escape },
    ExpectedTranslation { .row = "xkb: Backspace", .key = Key::Backspace },
    // Text-less: the Qt key → character table names it.
    ExpectedTranslation { .row = "xkb: Ctrl+Space", .text = U" ", .chord = Ctrl },
    ExpectedTranslation { .row = "xkb: Enter", .key = Key::Enter },
    ExpectedTranslation { .row = "xkb: Alt+Enter is a binding", .key = Key::Enter, .chord = Alt },
    ExpectedTranslation { .row = "xkb: Shift+Enter", .key = Key::Enter, .chord = Shift },
    ExpectedTranslation { .row = "xkb: Ctrl+U under kitty disambiguate", .text = U"U",
                          .chord = Ctrl, .unshiftedKey = U'u' },
    ExpectedTranslation { .row = "xkb: Ctrl+U under kitty report-all", .text = U"U",
                          .chord = Ctrl, .unshiftedKey = U'u' },
    // The legacy Cyrillic keysym is read through the keysym table.
    ExpectedTranslation { .row = "xkb: Cyrillic letter under kitty disambiguate", .text = U"ж",
                          .unshiftedKey = U'ж' },
    ExpectedTranslation { .row = "xkb: Cyrillic letter under kitty report-all", .text = U"ж",
                          .unshiftedKey = U'ж' },
    // The keymap's level 0 of keycode 12, not the shifted keysym the event reports.
    ExpectedTranslation { .row = "xkb: Shift+3 under kitty report-all", .text = U"#", .chord = Shift,
                          .unshiftedKey = U'3' },
};
// clang-format on

/// @return The layout the rows are read with: X11 keysyms, with a keymap that knows the number row's '3'.
[[nodiscard]] contour::input::KeyboardLayout const& xkbLayout()
{
    static auto const rows =
        std::array { contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 } };
    static auto const layout =
        contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(rows));
    return *layout;
}

} // namespace

TEST_CASE("the Xkb platform reproduces today's Linux translation", "[input][platform]")
{
    auto const platform = contour::input::makeKeyboardPlatform(KeyboardPlatformKind::Xkb, {}, xkbLayout());

    for (auto const& row: contour::test::keyCharacterizationRows())
    {
        if (row.kind != KeyboardPlatformKind::Xkb)
            continue;
        INFO(row.name);
        auto const expected = std::ranges::find(XkbTranslations, row.name, &ExpectedTranslation::row);
        // Every Xkb row is stated here, so a new one cannot slip past this test.
        REQUIRE(expected != XkbTranslations.end());

        auto const event = platform->normalize(row.event);
        CHECK(event.key == expected->key);
        CHECK(event.text == expected->text);
        CHECK(event.modifiers.chord == expected->chord);
        CHECK(event.modifiers.locks == expected->locks);
        CHECK(event.identity.unshiftedKey == expected->unshiftedKey);
        CHECK(event.appModifier == contour::input::AppModifier::Absent);
        CHECK(event.type == row.event.type);
    }
}

TEST_CASE("an event the platform cannot translate is reported unhandled", "[input][platform]")
{
    for (auto const kind:
         { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32, KeyboardPlatformKind::Mac })
    {
        auto const platform = contour::input::makeKeyboardPlatform(kind, {}, xkbLayout());
        auto const event = platform->normalize({ .qtKey = Qt::Key_unknown });
        CHECK(!event.key);
        CHECK(event.text.empty());
        CHECK(event.isUnhandled());
    }
}

TEST_CASE("an input method commit is text, unmodified", "[input][platform]")
{
    auto const platform = contour::input::makeKeyboardPlatform(KeyboardPlatformKind::Xkb, {}, xkbLayout());
    auto const event = platform->normalize({ .qtKey = 0,
                                             .text = U"日本",
                                             .qtModifiers = contour::test::keybits::Shift,
                                             .nativeVirtualKey = 0x41,
                                             .origin = contour::input::KeyEventOrigin::InputMethodCommit });
    CHECK(!event.key);
    CHECK(event.text == U"日本");
    CHECK(event.modifiers.chord.none());
    CHECK(event.identity.unshiftedKey == 0);
    CHECK(event.origin == contour::input::KeyEventOrigin::InputMethodCommit);
}

TEST_CASE("an input method commit carries the lock keys its capture sampled", "[input][platform]")
{
    using contour::input::KeyEventOrigin;
    using namespace contour::test::keybits;
    auto const commitWith = [](KeyboardPlatformKind kind, uint32_t nativeModifiers) {
        return contour::input::makeKeyboardPlatform(kind, {}, xkbLayout())
            ->normalize({ .text = U"x",
                          .nativeModifiers = nativeModifiers,
                          .origin = KeyEventOrigin::InputMethodCommit });
    };
    auto const xkb = commitWith(KeyboardPlatformKind::Xkb, XcbCapsLock | XcbNumLock);
    CHECK(xkb.modifiers.locks == LockKeys { LockKey::CapsLock, LockKey::NumLock });
    CHECK(xkb.modifiers.chord.none());
    CHECK(commitWith(KeyboardPlatformKind::Mac, MacCapsLock).modifiers.locks == CapsLock);
    CHECK(commitWith(KeyboardPlatformKind::Win32,
                     contour::input::Win32CapsLockBit | contour::input::Win32NumLockBit)
              .modifiers.locks
          == LockKeys { LockKey::CapsLock, LockKey::NumLock });
}

TEST_CASE("a release keeps its type through the platform", "[input][platform]")
{
    auto const platform = contour::input::makeKeyboardPlatform(KeyboardPlatformKind::Xkb, {}, xkbLayout());
    auto const event =
        platform->normalize({ .qtKey = Qt::Key_Up, .type = vtbackend::KeyboardEventType::Release });
    CHECK(event.key == Key::UpArrow);
    CHECK(event.type == vtbackend::KeyboardEventType::Release);
}
