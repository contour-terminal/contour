// SPDX-License-Identifier: Apache-2.0
//
// RegisterHotKey on a real Windows desktop, with a chord nothing binds (Control+Alt+Shift+F19). Native
// Windows platform only; the key table is HotkeyKeys_test.cpp's, on every OS.

#ifdef _WIN32

    #include <contour/config/HotkeyChord.hpp>
    #include <contour/platform/GlobalHotkey.hpp>
    #include <contour/platform/Win32GlobalHotkey.hpp>

    #include <QtCore/QCoreApplication>
    #include <QtGui/QGuiApplication>

    #include <catch2/catch_test_macros.hpp>

    #include <optional>
    #include <string>
    #include <vector>

    #include <Windows.h>

using contour::config::HotkeyChord;
using contour::platform::HotkeyError;
using contour::platform::HotkeyResult;
using contour::platform::Win32GlobalHotkey;
using vtbackend::Key;
using vtbackend::Modifier;
using vtbackend::Modifiers;

namespace
{

auto const UnboundChord =
    HotkeyChord { .modifiers = Modifiers { Modifier::Control, Modifier::Alt, Modifier::Shift },
                  .key = Key::F19 };

/// @return Whether the native Windows platform plugin is running.
[[nodiscard]] bool onNativeWindows()
{
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

/// Posts the message RegisterHotKey makes the OS post, and lets the event loop deliver it.
void deliverHotkey(int id)
{
    REQUIRE(PostThreadMessageW(GetCurrentThreadId(), WM_HOTKEY, static_cast<WPARAM>(id), 0));
    QCoreApplication::processEvents();
}

} // namespace

TEST_CASE("Win32GlobalHotkey registers an unbound chord", "[native][win32][globalhotkey]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto hotkey = Win32GlobalHotkey {};
    auto settled = std::optional<HotkeyResult> {};
    auto const result = hotkey.registerHotkey(
        UnboundChord, [](std::string const&) {}, [&settled](HotkeyResult answer) { settled = answer; });
    CHECK(result.has_value());
    REQUIRE(settled.has_value());
    CHECK(settled->has_value());
    CHECK(hotkey.registeredId() != 0);
}

TEST_CASE("Win32GlobalHotkey calls back when WM_HOTKEY arrives", "[native][win32][globalhotkey]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto hotkey = Win32GlobalHotkey {};
    auto presses = 0;
    REQUIRE(
        hotkey.registerHotkey(UnboundChord, [&presses](std::string const&) { ++presses; }, {}).has_value());

    deliverHotkey(hotkey.registeredId() + 1000); // another hot key's id
    CHECK(presses == 0);
    deliverHotkey(hotkey.registeredId());
    CHECK(presses == 1);

    hotkey.unregisterHotkey();
    deliverHotkey(hotkey.registeredId());
    CHECK(presses == 1);
}

TEST_CASE("Win32GlobalHotkey reports a taken chord", "[native][win32][globalhotkey]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto first = Win32GlobalHotkey {};
    REQUIRE(first.registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());

    auto second = Win32GlobalHotkey {};
    auto const taken = second.registerHotkey(UnboundChord, [](std::string const&) {}, {});
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == HotkeyError::AlreadyTaken);

    first.unregisterHotkey();
    CHECK(second.registerHotkey(UnboundChord, [](std::string const&) {}, {}).has_value());
}

TEST_CASE("Win32GlobalHotkey refuses a key without a row", "[native][win32][globalhotkey]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto hotkey = Win32GlobalHotkey {};
    auto const result = hotkey.registerHotkey(
        HotkeyChord { .modifiers = Modifier::Control, .key = Key::F35 }, [](std::string const&) {}, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == HotkeyError::UnsupportedKey);
}

TEST_CASE("Win32GlobalHotkey is unsupported off the native Windows platform", "[globalhotkey][offscreen]")
{
    if (onNativeWindows())
        SKIP("Covers the other platform plugins.");
    auto hotkey = Win32GlobalHotkey {};
    auto const result = hotkey.registerHotkey(UnboundChord, [](std::string const&) {}, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == HotkeyError::Unsupported);
}

#endif
