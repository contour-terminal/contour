// SPDX-License-Identifier: Apache-2.0
//
// The libxkbcommon-backed keymap. Built on Linux only, because that is where the adapter is.

#include <contour/input/XkbKeymap.hpp>
#include <contour/input/XkbLiveKeymap.hpp>

#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

namespace
{
constexpr auto KeycodeOfKey3 = uint32_t { 12 };  // evdev KEY_3 (4) + 8
constexpr auto KeysymThree = uint32_t { 0x33 };  // XK_3
constexpr auto KeysymSharpS = uint32_t { 0xDF }; // XK_ssharp
} // namespace

TEST_CASE("a keymap built from names resolves the US '3' key", "[input][xkb][live]")
{
    auto const keymap =
        contour::input::makeXkbKeymapFromNames({ .rules = "evdev", .model = "pc105", .layout = "us" });
    if (!keymap)
        SKIP("no XKB data installed");

    CHECK(keymap->levelZeroKeysymOf(KeycodeOfKey3, 0) == KeysymThree);
    // A keycode no key has.
    CHECK(keymap->levelZeroKeysymOf(0, 0) == 0);
}

TEST_CASE("a keymap built from names follows its layout, not the US one", "[input][xkb][live]")
{
    auto const keymap =
        contour::input::makeXkbKeymapFromNames({ .rules = "evdev", .model = "pc105", .layout = "de" });
    if (!keymap)
        SKIP("no XKB data installed");

    // On a German keyboard the key right of '0' types ß at level 0, where the US one types '-'.
    constexpr auto KeycodeRightOfZero = uint32_t { 20 };
    CHECK(keymap->levelZeroKeysymOf(KeycodeRightOfZero, 0) == KeysymSharpS);
}

TEST_CASE("a keymap with two layouts answers for the group asked", "[input][xkb][live]")
{
    auto const keymap =
        contour::input::makeXkbKeymapFromNames({ .rules = "evdev", .model = "pc105", .layout = "us,de" });
    if (!keymap)
        SKIP("no XKB data installed");

    constexpr auto KeycodeRightOfZero = uint32_t { 20 };
    constexpr auto KeysymMinus = uint32_t { 0x2D };
    CHECK(keymap->levelZeroKeysymOf(KeycodeRightOfZero, 0) == KeysymMinus);
    CHECK(keymap->levelZeroKeysymOf(KeycodeRightOfZero, 1) == KeysymSharpS);
    // A group beyond the last selects the last.
    CHECK(keymap->levelZeroKeysymOf(KeycodeRightOfZero, 3) == KeysymSharpS);
}

TEST_CASE("the live keymap resolves the '3' key under a display", "[input][xkb][live]")
{
    if (qGuiApp == nullptr || qGuiApp->platformName() != QStringLiteral("xcb"))
    {
        // Offscreen, or no Qt application at all: no keymap is reachable, and that is the answer.
        CHECK(contour::input::makeLiveXkbKeymap() == nullptr);
        SKIP("no display");
    }

    auto const keymap = contour::input::makeLiveXkbKeymap();
    REQUIRE(keymap);
    // Whatever layout the developer runs, the digit row types something at level 0.
    CHECK(keymap->levelZeroKeysymOf(KeycodeOfKey3, 0) != 0);
}
