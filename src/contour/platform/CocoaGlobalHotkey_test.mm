// SPDX-License-Identifier: Apache-2.0
//
// The Carbon hot key, registered with a chord nothing binds (Control+Alt+Shift+F19). Registering reaches
// the OS's hot key table but changes nothing a user can see, so the default suite runs it.

#include <contour/config/HotkeyChord.hpp>
#include <contour/platform/GlobalHotkey.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using contour::config::HotkeyChord;
using contour::platform::HotkeyError;
using contour::platform::HotkeyResult;
using contour::platform::makeGlobalHotkey;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{
auto const UnboundChord =
    HotkeyChord { .modifiers = Modifiers { Modifier::Control, Modifier::Alt, Modifier::Shift },
                  .key = Key::F19 };
} // namespace

TEST_CASE("CocoaGlobalHotkey.registersAnUnboundChord", "[globalhotkey][cocoa]")
{
    auto const hotkey = makeGlobalHotkey();
    auto settled = std::optional<HotkeyResult> {};
    auto const result = hotkey->registerHotkey(
        UnboundChord, [](std::string const&) {}, [&settled](HotkeyResult answer) { settled = answer; });
    CHECK(result.has_value());
    REQUIRE(settled.has_value());
    CHECK(settled->has_value());
    hotkey->unregisterHotkey();
    hotkey->unregisterHotkey();
}

TEST_CASE("CocoaGlobalHotkey.aSecondInstanceFindsTheChordTaken", "[globalhotkey][cocoa]")
{
    auto const first = makeGlobalHotkey();
    REQUIRE(first->registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());

    auto const second = makeGlobalHotkey();
    auto const taken = second->registerHotkey(UnboundChord, [](std::string const&) {}, {});
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == HotkeyError::AlreadyTaken);

    first->unregisterHotkey();
    CHECK(second->registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());
}

TEST_CASE("CocoaGlobalHotkey.destroyingItReleasesTheChord", "[globalhotkey][cocoa]")
{
    {
        auto const first = makeGlobalHotkey();
        REQUIRE(first->registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());
    }
    auto const second = makeGlobalHotkey();
    CHECK(second->registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());
}

TEST_CASE("CocoaGlobalHotkey.refusesAKeyWithoutARow", "[globalhotkey][cocoa]")
{
    auto const hotkey = makeGlobalHotkey();
    auto const result = hotkey->registerHotkey(
        HotkeyChord { .modifiers = Modifier::Control, .key = Key::F35 }, [](std::string const&) {}, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == HotkeyError::UnsupportedKey);
}
