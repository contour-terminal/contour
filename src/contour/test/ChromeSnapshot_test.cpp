// SPDX-License-Identifier: Apache-2.0
//
// Offscreen checks of the chrome snapshot scene: every script builds as Main.qml over mocks, a scene
// gives the application palette back, and the bundled chrome fonts register. The pictures themselves
// need a display and are rendered elsewhere; these prove the scenes they are taken of.

#include <contour/config/Config.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/ChromeSnapshotScene.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/SettingsController.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>

TEST_CASE("Every snapshot script builds on the macOS and Terminal rows (offscreen)",
          "[contour][gui][qml][snapshot]")
{
    using namespace contour::test::snapshot;
    for (auto const& variant: snapshotMatrix(
             std::array { contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal }))
    {
        if (variant.appearance != vtbackend::ColorPreference::Dark)
            continue; // one appearance is enough to prove the structure
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();
        CHECK(scene.diagnostics().isEmpty());
        auto const& row = scriptRow(variant.script);
        auto const look = snapshotLook(variant.edition, variant.appearance);
        auto* titleBar = scene.root().findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        // Review focus 5: Native2026 hides a one-tab strip, Classic does not.
        CHECK(titleBar->isVisible() == contour::config::isTabBarShown(look.tabBarVisibility, row.tabCount));
        switch (variant.script)
        {
            case SnapshotScript::ThreeTabs:
                CHECK(scene.root().findChild<QObject*>(QStringLiteral("tabList"))->property("count").toInt()
                      == 3);
                break;
            case SnapshotScript::SplitThree:
                CHECK(scene.root().findChildren<contour::test::StubContourTerminal*>().size() == 3);
                break;
            case SnapshotScript::Palette:
                CHECK(scene.root()
                          .findChild<QObject*>(QStringLiteral("commandPalette"))
                          ->property("opened")
                          .toBool());
                break;
            case SnapshotScript::Settings:
                CHECK(scene.root()
                          .findChild<QObject*>(QStringLiteral("settingsPageLoader"))
                          ->property("item")
                          .value<QObject*>()
                      != nullptr);
                break;
            case SnapshotScript::RemoteReconnecting:
                CHECK(
                    scene.root().findChild<QQuickItem*>(QStringLiteral("reconnectingOverlay"))->isVisible());
                break;
            case SnapshotScript::SingleTab: break;
        }
    }
}

TEST_CASE("A snapshot scene gives the application palette back (offscreen)", "[contour][gui][qml][snapshot]")
{
    using namespace contour::test::snapshot;
    auto const before = QGuiApplication::palette();
    {
        auto scene = ChromeSnapshotScene { { SnapshotScript::SingleTab,
                                             contour::config::ChromeStyle::ContourLinux,
                                             contour::config::DefaultsEdition::Native2026,
                                             vtbackend::ColorPreference::Light } };
        scene.show();
        CHECK(QGuiApplication::palette().color(QPalette::Window)
              == QColor(0xef, 0xef, 0xef)); // buildThemePalette(Light)
    }
    CHECK(QGuiApplication::palette() == before); // review focus 1
}

TEST_CASE("The bundled chrome fonts register with Qt, and a missing directory says so (offscreen)",
          "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto const families = registerBundledChromeFonts(core::platform::NativeFileSystem::instance(),
                                                     bundledChromeFontDirectory());
    REQUIRE(families.has_value());
    CHECK(families->contains(QStringLiteral("JetBrains Mono")));

    auto const nothing = core::platform::testing::InMemoryFileSystem {};
    auto const missing = registerBundledChromeFonts(nothing, "/nonexistent/fonts");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == SnapshotFontError::DirectoryMissing);

    // A directory whose only `.ttf` is not a font loads nothing, and says so rather than an empty list.
    auto const noFont = core::platform::testing::InMemoryFileSystem {
        { .path = "/fonts", .isDirectory = true },
        { .path = "/fonts/broken.ttf", .content = "not a font" },
        { .path = "/fonts/README.md", .content = "# fonts" },
    };
    auto const unloaded = registerBundledChromeFonts(noFont, "/fonts");
    REQUIRE_FALSE(unloaded.has_value());
    CHECK(unloaded.error() == SnapshotFontError::NoFontLoaded);
}
