// SPDX-License-Identifier: Apache-2.0
//
// Offscreen checks of the chrome snapshot scene: every script builds as Main.qml over mocks, a scene
// gives the application palette back, and the bundled chrome fonts register. The pictures themselves
// need a display: the last case renders them, on the display route only, and only when
// CONTOUR_SNAPSHOT_DIR names where they go.

#include <contour/config/Config.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/ChromeSnapshotScene.hpp>
#include <contour/test/ChromeSnapshotWriter.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/SettingsController.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/Environment.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtGui/QColor>
#include <QtGui/QFontInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPalette>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>

#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QTest>

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
            case SnapshotScript::ThreeTabs: {
                auto const* tabList = scene.root().findChild<QObject*>(QStringLiteral("tabList"));
                REQUIRE(tabList != nullptr);
                CHECK(tabList->property("count").toInt() == 3);
                break;
            }
            case SnapshotScript::SplitThree:
                CHECK(scene.root().findChildren<contour::test::StubContourTerminal*>().size() == 3);
                break;
            case SnapshotScript::Palette: {
                auto const* palette = scene.root().findChild<QObject*>(QStringLiteral("commandPalette"));
                REQUIRE(palette != nullptr);
                CHECK(palette->property("opened").toBool());
                break;
            }
            case SnapshotScript::Settings: {
                auto const* loader = scene.root().findChild<QObject*>(QStringLiteral("settingsPageLoader"));
                REQUIRE(loader != nullptr);
                CHECK(loader->property("item").value<QObject*>() != nullptr);
                break;
            }
            case SnapshotScript::RemoteReconnecting: {
                auto const* overlay =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("reconnectingOverlay"));
                REQUIRE(overlay != nullptr);
                CHECK(overlay->isVisible());
                break;
            }
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

TEST_CASE("The snapshot writer writes a PNG and its manifest line", "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto const variant = SnapshotVariant { SnapshotScript::Palette,
                                           contour::config::ChromeStyle::Terminal,
                                           contour::config::DefaultsEdition::Native2026,
                                           vtbackend::ColorPreference::Dark };
    auto const facts = SnapshotFacts { "offscreen", 1.0, "JetBrains Mono", "Fusion", "6.11.1" };
    auto image = QImage(QSize(4, 4), QImage::Format_ARGB32);
    image.fill(Qt::red);
    auto const expectedManifest = std::string(ManifestHeader) + manifestLine(variant, facts);

    auto writer = ChromeSnapshotWriter::open(files, "/out");
    REQUIRE(writer.has_value());
    auto const written = writer->write(variant, image, facts);
    REQUIRE(written.has_value());
    CHECK(*written == std::filesystem::path("/out/palette.terminal.native2026.dark.png"));
    CHECK(files.readFile(*written).value().starts_with("\x89PNG"));
    CHECK(files.readFile("/out/manifest.tsv").value() == expectedManifest);

    // Review focus 4: the second pass opens the same directory and keeps the first pass's lines.
    auto again = ChromeSnapshotWriter::open(files, "/out");
    REQUIRE(again.has_value());
    CHECK(files.readFile("/out/manifest.tsv").value() == expectedManifest);

    CHECK(writer->write(variant, QImage {}, facts).error() == SnapshotWriteError::EncodeFailed);
    CHECK(files.readFile("/out/manifest.tsv").value() == expectedManifest);
}

TEST_CASE("The snapshot writer refuses a directory it cannot use", "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto files = core::platform::testing::InMemoryFileSystem { core::platform::testing::FileEntry {
        .path = "/out",
        .content = "not a directory",
        .perms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        .isExecutable = false,
        .isDirectory = false,
        .isSymlink = false } };
    CHECK(ChromeSnapshotWriter::open(files, "/out").error() == SnapshotWriteError::DirectoryUnavailable);
}

TEST_CASE("Chrome snapshots: every script on every row this process draws", "[display][snapshot]")
{
    using namespace contour::test::snapshot;
    auto const environment = core::LiveEnvironment {};
    if (!contour::test::displayTestsEnabled(environment))
        SKIP("renders through a real compositor: run under scripts/run-chrome-snapshots.sh");
    auto const directory = snapshotDirectory(environment);
    if (!directory)
        SKIP("set " << SnapshotDirectoryVariable << " to the directory the snapshots go to");

    // The style test_main.cpp was asked for must be the one in effect, or a pass renders the wrong rows.
    auto const controls = QQuickStyle::name().toStdString();
    INFO("Quick Controls style: " << controls << " (set " << ControlsStyleVariable << ")");
    REQUIRE(controls == controlsStyleFor(environment));
    auto const rows = rowsRenderableUnder(controls);
    REQUIRE_FALSE(rows.empty());

    auto const& files = core::platform::NativeFileSystem::instance();
    REQUIRE(registerBundledChromeFonts(files, bundledChromeFontDirectory()).has_value());
    auto writer = ChromeSnapshotWriter::open(files, *directory);
    REQUIRE(writer.has_value());

    for (auto const& variant: snapshotMatrix(rows))
    {
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();
        REQUIRE(QTest::qWaitForWindowExposed(&scene.window(), 3000));
        auto const image = scene.grab();
        CHECK(scene.diagnostics().isEmpty());
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == SnapshotWindowSize * scene.window().devicePixelRatio());
        auto const facts = SnapshotFacts {
            .platform = QGuiApplication::platformName().toStdString(),
            .devicePixelRatio = scene.window().devicePixelRatio(),
            .chromeFontFamily = QFontInfo(scene.chromeFont()).family().toStdString(),
            .controlsStyle = controls,
            .qtVersion = qVersion(),
        };
        CHECK(writer->write(variant, image, facts).has_value());
    }
}
