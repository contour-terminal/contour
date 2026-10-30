// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/HotkeyKeys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using contour::platform::hotkeyKeyRow;
using contour::platform::HotkeyKeyRows;
using contour::platform::HotkeyModifierRows;
using contour::platform::xdgTrigger;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

TEST_CASE("hotkeyKeys.everyRowIsDistinct", "[hotkeykeys]")
{
    auto xdgNames = std::set<std::string_view> {};
    auto macCodes = std::set<std::uint16_t> {};
    auto windowsCodes = std::set<std::uint16_t> {};
    for (auto const& row: HotkeyKeyRows)
    {
        CHECK(xdgNames.insert(row.xdgName).second);
        CHECK(macCodes.insert(row.macVirtualKey).second);
        CHECK(windowsCodes.insert(row.windowsVirtualKey).second);
        CHECK(hotkeyKeyRow(row.key) == &row);
    }
    CHECK(xdgNames.size() == HotkeyKeyRows.size());
}

TEST_CASE("hotkeyKeys.everyModifierRowIsDistinct", "[hotkeykeys]")
{
    auto macMasks = std::set<std::uint32_t> {};
    auto windowsMasks = std::set<std::uint32_t> {};
    auto xdgNames = std::set<std::string_view> {};
    for (auto const& row: HotkeyModifierRows)
    {
        CHECK(macMasks.insert(row.macMask).second);
        CHECK(windowsMasks.insert(row.windowsMask).second);
        CHECK(xdgNames.insert(row.xdgName).second);
    }
}

TEST_CASE("hotkeyKeys.lettersMatchInEitherCase", "[hotkeykeys]")
{
    CHECK(hotkeyKeyRow(U'a') != nullptr);
    CHECK(hotkeyKeyRow(U'a') == hotkeyKeyRow(U'A'));
    CHECK(hotkeyKeyRow(U'é') == nullptr);
    CHECK(hotkeyKeyRow(Key::F35) == nullptr);
    CHECK(hotkeyKeyRow(Key::Backspace) == nullptr);
}

TEST_CASE("hotkeyKeys.platformCodesOfKnownKeys", "[hotkeykeys]")
{
    // Spot checks against HIToolbox's Events.h and WinUser.h, so a mistyped row cannot hide in the table.
    auto const* const a = hotkeyKeyRow(U'a');
    REQUIRE(a != nullptr);
    CHECK(a->macVirtualKey == 0x00);
    CHECK(a->windowsVirtualKey == 0x41);

    auto const* const grave = hotkeyKeyRow(U'`');
    REQUIRE(grave != nullptr);
    CHECK(grave->macVirtualKey == 0x32);
    CHECK(grave->windowsVirtualKey == 0xC0);

    auto const* const f12 = hotkeyKeyRow(Key::F12);
    REQUIRE(f12 != nullptr);
    CHECK(f12->macVirtualKey == 0x6F);
    CHECK(f12->windowsVirtualKey == 0x7B);

    auto const* const f19 = hotkeyKeyRow(Key::F19);
    REQUIRE(f19 != nullptr);
    CHECK(f19->macVirtualKey == 0x50);
    CHECK(f19->windowsVirtualKey == 0x82);
}

TEST_CASE("hotkeyKeys.xdgTrigger", "[hotkeykeys]")
{
    CHECK(xdgTrigger({ .modifiers = Modifiers { Modifier::Control, Modifier::Alt }, .key = U'T' })
          == "CTRL+ALT+t");
    CHECK(xdgTrigger({ .modifiers = Modifier::Super, .key = Key::F12 }) == "LOGO+F12");
    CHECK(xdgTrigger({ .modifiers = Modifier::Control, .key = U'`' }) == "CTRL+grave");
    CHECK(xdgTrigger({ .modifiers = Modifier::Control, .key = U'é' }).empty());
}
