// SPDX-License-Identifier: Apache-2.0
//
// The macOS normalizer, on any host: the Option rule per side, ⌘ as the application modifier, and which
// keystrokes skip the input method.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/session/KeyCharacterizationRows.hpp>

#include <QtCore/Qt>

#include <catch2/catch_test_macros.hpp>

namespace
{
using contour::config::KeyboardPlatformKind;
using contour::config::OptionAsAlt;
using contour::config::OptionKeySide;
using contour::input::AppModifier;
using contour::input::InputMethodRoute;
using contour::input::NativeKeyEvent;
using vtbackend::Key;
using vtbackend::Modifier;
using namespace contour::test::keybits;

/// @return The Mac normalizer under @p setting, reading key codes as a US ANSI keyboard does.
[[nodiscard]] std::unique_ptr<contour::input::KeyboardPlatform> macPlatform(OptionAsAlt setting)
{
    static auto const layout = contour::input::makeMacUsAnsiKeyboardLayout();
    return contour::input::makeKeyboardPlatform(
        KeyboardPlatformKind::Mac, { .optionAsAlt = setting }, *layout);
}

constexpr auto MacKeyE = uint32_t { 0x0e }; // kVK_ANSI_E
} // namespace

TEST_CASE("optionKeySideOf reads macOS's device-dependent Option bits", "[input][platform][mac]")
{
    using contour::input::optionKeySideOf;

    STATIC_CHECK(optionKeySideOf(0x20) == OptionKeySide::Left);
    STATIC_CHECK(optionKeySideOf(0x40) == OptionKeySide::Right);
    STATIC_CHECK(optionKeySideOf(0x60) == OptionKeySide::Both);
    // The device-independent Option flag says Option is held, not which one.
    STATIC_CHECK(optionKeySideOf(0x80000) == OptionKeySide::None);
    STATIC_CHECK(optionKeySideOf(0) == OptionKeySide::None);
    // The other device bits around them are not Option keys.
    STATIC_CHECK(optionKeySideOf(0x80000 | 0x20 | 0x01 | 0x08) == OptionKeySide::Left);
    STATIC_CHECK(optionKeySideOf(0x80000 | 0x40 | 0x02 | 0x10) == OptionKeySide::Right);
}

TEST_CASE("Option as Alt is per side", "[input][platform][mac]")
{
    for (auto const setting: { OptionAsAlt::Off, OptionAsAlt::Left, OptionAsAlt::Right, OptionAsAlt::Both })
    {
        for (auto const [side, bits]: { std::pair { OptionKeySide::Left, MacLeftOption },
                                        std::pair { OptionKeySide::Right, MacRightOption } })
        {
            INFO("setting " << static_cast<int>(setting) << ", side " << static_cast<int>(side));
            auto const event =
                macPlatform(setting)->normalize(NativeKeyEvent { .qtKey = Qt::Key_E,
                                                                 .text = U"é",
                                                                 .qtModifiers = Alt,
                                                                 .nativeModifiers = bits,
                                                                 .nativeVirtualKey = MacKeyE });
            auto const isAlt =
                contour::config::optionKeyRole(setting, side) == contour::config::OptionKeyRole::Alt;
            CHECK(event.modifiers.chord.test(Modifier::Alt) == isAlt);
            // Alt sends the key's own letter; a composing Option key sends what it composed.
            CHECK(event.text == (isAlt ? U"e" : U"é"));
            CHECK(event.altSide == side);
        }
    }
}

TEST_CASE("an unnamed Option key stays Alt on keys that type nothing", "[input][platform][mac]")
{
    auto const platform = macPlatform(OptionAsAlt::Off);

    auto const left =
        platform->normalize({ .qtKey = Qt::Key_Left, .qtModifiers = Alt, .nativeModifiers = MacLeftOption });
    CHECK(left.key == Key::LeftArrow);
    CHECK(left.modifiers.chord == vtbackend::Modifiers { Modifier::Alt });

    auto const enter = platform->normalize(
        { .qtKey = Qt::Key_Return, .text = U"\r", .qtModifiers = Alt, .nativeModifiers = MacLeftOption });
    CHECK(enter.key == Key::Enter);
    CHECK(enter.modifiers.chord == vtbackend::Modifiers { Modifier::Alt });

    // And on a key that composes, it composes: Option+1 types ¡, with no Alt left in the chord.
    auto const inverted = platform->normalize(
        { .qtKey = Qt::Key_1, .text = U"¡", .qtModifiers = Alt, .nativeModifiers = MacLeftOption });
    CHECK(inverted.text == U"¡");
    CHECK(inverted.modifiers.chord.none());
}

TEST_CASE("a named Option key's dead key sends its letter as Alt", "[input][platform][mac]")
{
    // A dead key types nothing: the letter is the Qt key's, in the case Shift and CapsLock say.
    auto const platform = macPlatform(OptionAsAlt::Left);
    auto const dead = platform->normalize({ .qtKey = Qt::Key_E,
                                            .qtModifiers = Alt,
                                            .nativeModifiers = MacLeftOption,
                                            .nativeVirtualKey = MacKeyE });
    CHECK(dead.text == U"e");
    CHECK(dead.modifiers.chord == vtbackend::Modifiers { Modifier::Alt });

    auto const capsLocked = platform->normalize({ .qtKey = Qt::Key_E,
                                                  .qtModifiers = Alt,
                                                  .nativeModifiers = MacLeftOption | MacCapsLock,
                                                  .nativeVirtualKey = MacKeyE });
    CHECK(capsLocked.text == U"E");
}

TEST_CASE("Command is the application modifier", "[input][platform][mac]")
{
    auto const platform = macPlatform(OptionAsAlt::Off);

    auto const command = platform->normalize(
        { .qtKey = Qt::Key_C, .text = U"c", .qtModifiers = Meta, .nativeVirtualKey = 0x08 });
    CHECK(command.appModifier == AppModifier::Held);
    CHECK(command.modifiers.chord == vtbackend::Modifiers { Modifier::Super });

    auto const control = platform->normalize(
        { .qtKey = Qt::Key_C, .text = U"\x03", .qtModifiers = Ctrl, .nativeVirtualKey = 0x08 });
    CHECK(control.appModifier == AppModifier::Absent);
}

TEST_CASE("the input method is bypassed only for an Option key acting as Alt", "[input][platform][mac]")
{
    auto const left = macPlatform(OptionAsAlt::Left);
    CHECK(left->inputMethodRouteFor(MacLeftOption) == InputMethodRoute::Bypass);
    CHECK(left->inputMethodRouteFor(MacRightOption) == InputMethodRoute::ToInputMethod);
    CHECK(left->inputMethodRouteFor(0) == InputMethodRoute::ToInputMethod);
    // Option without side bits says Option is held, not which: nothing named is known to be down.
    CHECK(left->inputMethodRouteFor(MacOption) == InputMethodRoute::ToInputMethod);

    CHECK(macPlatform(OptionAsAlt::Off)->inputMethodRouteFor(MacLeftOption)
          == InputMethodRoute::ToInputMethod);
    CHECK(macPlatform(OptionAsAlt::Both)->inputMethodRouteFor(MacRightOption) == InputMethodRoute::Bypass);

    // No other platform has a keystroke to keep from its input method.
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
        CHECK(contour::input::makeKeyboardPlatform(
                  kind, { .optionAsAlt = OptionAsAlt::Both }, *contour::input::makeXkbKeyboardLayout(nullptr))
                  ->inputMethodRouteFor(MacLeftOption)
              == InputMethodRoute::ToInputMethod);
}
