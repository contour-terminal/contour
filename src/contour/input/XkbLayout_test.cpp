// SPDX-License-Identifier: Apache-2.0
//
// The X11 and Wayland key layout: the keysym table behind it, and the keymap that names a key's unshifted
// character.

#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyboardLayout.hpp>
#include <contour/input/KeyboardPlatform.hpp>
#include <contour/input/KeysymTable.hpp>
#include <contour/input/XkbKeymap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <utility>

TEST_CASE("the static keysym table decodes legacy keysyms", "[input][xkb]")
{
    using contour::input::keysymToCodepoint;

    STATIC_CHECK(keysymToCodepoint(0x06C1) == U'а'); // XK_Cyrillic_a
    STATIC_CHECK(keysymToCodepoint(0x07E1) == U'α'); // XK_Greek_alpha
    STATIC_CHECK(keysymToCodepoint(0x01000430) == U'а');
    STATIC_CHECK(keysymToCodepoint(0x0061) == U'a');
    STATIC_CHECK(keysymToCodepoint(0x00E9) == U'é'); // XK_eacute, Latin-1
    STATIC_CHECK(keysymToCodepoint(0x01A3) == U'Ł'); // XK_Lstroke, Latin-2
    STATIC_CHECK(keysymToCodepoint(0x20AC) == U'€'); // XK_EuroSign
    // Keysyms that name no character.
    STATIC_CHECK(keysymToCodepoint(0xFFFF) == 0);     // XK_Delete
    STATIC_CHECK(keysymToCodepoint(0xFF51) == 0);     // XK_Left
    STATIC_CHECK(keysymToCodepoint(0x1008FF11) == 0); // XF86AudioLowerVolume
    STATIC_CHECK(keysymToCodepoint(0) == 0);
}

TEST_CASE("the keymap answers the unshifted key, not the shifted one", "[input][xkb]")
{
    auto const rows =
        std::array { contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 } };
    auto const layout = contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(rows));

    // Shift+3 reports the keysym XK_numbersign, and the keycode of the key it came from.
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x23, .scanCode = 12 }) == U'3');
    // A keycode the keymap does not know falls back to the event's keysym.
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x61, .scanCode = 38 }) == U'a');
    // The VK code is a Windows notion.
    CHECK(layout->win32VirtualKeyOf({ .virtualKey = 0x23, .scanCode = 12 }) == 0);
}

TEST_CASE("without a keymap the layout falls back to the keysym table", "[input][xkb]")
{
    auto const layout = contour::input::makeXkbKeyboardLayout(nullptr);

    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x06C1 }) == U'а');
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x0100'0446 }) == U'ц');
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0xFFFF }) == 0);
}

TEST_CASE("the event's group selects the keymap's layout", "[input][xkb]")
{
    // Keycode 12 types '3' in group 0 and Cyrillic 'а' in group 1.
    auto const rows = std::array {
        contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 },
        contour::input::FixedXkbKey { .keycode = 12, .group = 1, .keysym = 0x06C1 },
    };
    auto const layout = contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(rows));

    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x23, .scanCode = 12, .group = 0 }) == U'3');
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x23, .scanCode = 12, .group = 1 }) == U'а');
    // A group the key has no row for answers as its last one does.
    CHECK(layout->unshiftedKeyOf({ .virtualKey = 0x23, .scanCode = 12, .group = 2 }) == U'а');
}

TEST_CASE("the Xkb platform decodes the group from the event's native state", "[input][xkb]")
{
    using contour::config::KeyboardPlatformKind;
    constexpr auto GroupOne = uint32_t { 1 } << 13; // X11 core state bits 13-14
    STATIC_CHECK(contour::input::xcbGroupOf(0) == 0);
    STATIC_CHECK(contour::input::xcbGroupOf(GroupOne) == 1);
    STATIC_CHECK(contour::input::xcbGroupOf(0x3 << 13) == 3);
    // The lock bits are not the group.
    STATIC_CHECK(contour::input::xcbGroupOf(0x02 | 0x10) == 0);

    auto const rows = std::array {
        contour::input::FixedXkbKey { .keycode = 12, .group = 0, .keysym = 0x33 },
        contour::input::FixedXkbKey { .keycode = 12, .group = 1, .keysym = 0x06C1 },
    };
    auto const layout = contour::input::makeXkbKeyboardLayout(contour::input::makeFixedXkbKeymap(rows));
    auto const platform = contour::input::makeKeyboardPlatform(KeyboardPlatformKind::Xkb, {}, *layout);

    auto event = contour::input::NativeKeyEvent {
        .qtKey = '#', .text = U"#", .qtModifiers = 0x02000000, .nativeVirtualKey = 0x23, .nativeScanCode = 12
    };
    CHECK(platform->normalize(event).identity.unshiftedKey == U'3');
    event.nativeModifiers = GroupOne;
    CHECK(platform->normalize(event).identity.unshiftedKey == U'а');
}
