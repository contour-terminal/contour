// SPDX-License-Identifier: Apache-2.0
//
// The Win32 settings reader and its broadcast watcher, against the running system. No case changes a system
// setting. Native Windows platform only; what the readings mean is WindowsSettings_test.cpp's, on every OS.

#ifdef _WIN32

    #include <contour/platform/AccessibilityPreferenceSource.hpp>
    #include <contour/platform/PlatformAppearance.hpp>
    #include <contour/platform/QtPlatformPlugin.hpp>
    #include <contour/platform/Win32Settings.hpp>
    #include <contour/platform/WindowsSettings.hpp>

    #include <QtCore/QCoreApplication>
    #include <QtGui/QWindow>

    #include <catch2/catch_test_macros.hpp>

    #include <vector>

    #include <Windows.h>

    #include <QtTest/QTest>

using contour::platform::ClientAreaAnimation;
using contour::platform::HighContrast;
using contour::platform::SettingTopic;

TEST_CASE("the reader agrees with the system", "[native][win32][settings]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const readings = contour::platform::Win32SettingsReader {}.read();
    CHECK(readings.osBuild.has_value());

    auto contrast = HIGHCONTRASTW {};
    contrast.cbSize = static_cast<UINT>(sizeof(contrast));
    REQUIRE(SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize, &contrast, 0));
    CHECK(readings.highContrast
          == ((contrast.dwFlags & HCF_HIGHCONTRASTON) != 0 ? HighContrast::On : HighContrast::Off));

    auto animation = BOOL { TRUE };
    REQUIRE(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0));
    CHECK(readings.animation == (animation ? ClientAreaAnimation::On : ClientAreaAnimation::Off));
}

TEST_CASE("the factories build the Windows sources", "[native][win32][settings]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto const appearance = contour::platform::makePlatformAppearance({});
    auto const accessibility = contour::platform::makeAccessibilityPreferenceSource({});
    CHECK(dynamic_cast<contour::platform::Win32PlatformAppearance const*>(appearance.get()) != nullptr);
    CHECK(dynamic_cast<contour::platform::Win32AccessibilityPreferenceSource const*>(accessibility.get())
          != nullptr);
    // The build is the one reading every runner has: the appearance answers from the system, not the
    // defaults.
    CHECK(appearance->current().materials != contour::config::MaterialSupport::Unknown);
}

TEST_CASE("a broadcast reaches the watcher", "[native][win32][settings]")
{
    if (!contour::platform::onNativeWindowsPlatform())
        SKIP("Requires the native Windows platform.");
    auto topics = std::vector<SettingTopic> {};
    auto watcher =
        contour::platform::Win32SettingsWatcher { [&](SettingTopic topic) { topics.push_back(topic); } };

    auto window = QWindow {};
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto* hwnd = reinterpret_cast<HWND>(window.winId());

    SendMessageW(hwnd, WM_DWMCOLORIZATIONCOLORCHANGED, 0, 0);
    REQUIRE(topics.size() == 1);
    CHECK(topics.front() == SettingTopic::Appearance);

    // An area no source follows passes by, and the message still reaches the window.
    SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"));
    CHECK(topics.size() == 1);

    SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
    REQUIRE(topics.size() == 2);
    CHECK(topics.back() == SettingTopic::Both);
}

#endif
