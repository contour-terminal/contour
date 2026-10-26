// SPDX-License-Identifier: Apache-2.0
//
// Offscreen checks of the chrome snapshot scene: every script builds as Main.qml over mocks, a scene
// gives the application palette back, and the bundled chrome fonts register. The pictures themselves
// need a display: the last case renders them, on the display route only, and only when
// CONTOUR_SNAPSHOT_DIR names where they go.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/ChromeSnapshotScene.hpp>
#include <contour/test/ChromeSnapshotWriter.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/Environment.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QPointF>
#include <QtGui/QColor>
#include <QtGui/QFontInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPalette>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ranges>

#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QTest>

namespace
{
/// How many items named @p name beneath @p root, in the visual tree, are visible.
[[nodiscard]] int visibleItemsNamed(QQuickItem const& root, QString const& name)
{
    return static_cast<int>(
        std::ranges::count_if(contour::test::snapshot::ChromeSnapshotScene::itemsNamed(root, name),
                              [](QQuickItem const* item) { return item->isVisible(); }));
}
} // namespace

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
        // Review focus 5: Native2026 hides a one-tab strip, Classic does not -- except on the macOS band,
        // which always shows (1c D1). The band table decides, from the row's tab shape and the script's
        // frame and mode.
        using contour::window::TabStripGate;
        auto const band = contour::window::resolveTitleBand(
            contour::window::TitleBandRow {
                .style = variant.row,
                .shape =
                    contour::config::ChromeTokenRows[static_cast<std::size_t>(variant.row)].layout.tabShape,
                .density = row.density,
            },
            {
                .frame = row.frame == SnapshotFrame::NativeFrame ? contour::window::WindowFrame::NativeFrame
                                                                 : contour::window::WindowFrame::ClientBand,
                .mode = row.mode == SnapshotWindowMode::FullScreenRevealed
                            ? contour::window::WindowMode::FullScreen
                            : contour::window::WindowMode::Windowed,
                .strip = contour::config::isTabBarShown(look.tabBarVisibility, row.tabCount)
                             ? TabStripGate::Shown
                             : TabStripGate::Hidden,
                .position = contour::config::TabBarPosition::Top,
                .notice =
                    contour::window::hostNoticeOf(contour::window::hostChipPresence(row.hostLabel),
                                                  ChromeSnapshotScene::hostChipStateFor(row.connection)),
                .tabs =
                    row.tabCount == 1 ? contour::window::OpenTabs::One : contour::window::OpenTabs::Several,
            });
        CHECK(titleBar->isVisible() == (band.band != contour::window::BarPresence::Hidden));
        // The macOS row's windowed band is drawn at the height of the title bar it covers (S2): the one its
        // surface grows -- 40 pt under the compact toolbar, Minimal's bare band the bare 32 pt (P3).
        if (variant.row == contour::config::ChromeStyle::MacOS && row.mode == SnapshotWindowMode::Windowed
            && row.frame == SnapshotFrame::ClientBand && titleBar->isVisible())
        {
            REQUIRE(scene.measuredTitleBar().has_value());
            CHECK(titleBar->height() == scene.measuredTitleBar()->height);
        }
        // A split is drawn as cards, with headers -- or, under Minimal, as bare lines with none until the
        // held modifier reveals them over the panes.
        auto const& content = *scene.window().contentItem();
        if (row.panes == PaneShape::SplitThree)
        {
            auto const panes =
                static_cast<int>(scene.root().findChildren<contour::test::StubContourTerminal*>().size());
            CHECK(visibleItemsNamed(content, QStringLiteral("paneHeader")) == panes);
            CHECK(visibleItemsNamed(content, QStringLiteral("paneCardSurface"))
                  == (row.density == contour::config::ChromeDensity::Minimal ? 0 : panes));
        }
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
            case SnapshotScript::Settings:
                CHECK(scene.subject().objectName() == QStringLiteral("settingsWindow"));
                CHECK(scene.subject().isVisible());
                CHECK(scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsPage")) != nullptr);
                CHECK(scene.root().findChild<QObject*>(QStringLiteral("settingsPageLoader")) == nullptr);
                break;
            case SnapshotScript::RemoteReconnecting: {
                auto const* overlay =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("reconnectingOverlay"));
                REQUIRE(overlay != nullptr);
                CHECK(overlay->isVisible());
                auto const* pill = overlay->findChild<QQuickItem*>(QStringLiteral("reconnectingPill"));
                REQUIRE(pill != nullptr);
                CHECK(pill->isVisible());
                break;
            }
            case SnapshotScript::Toast: {
                // The toast is a visual child of the content item, not a QObject child of the root.
                auto const items = scene.window().contentItem()->childItems();
                auto const toast = std::ranges::find_if(items, [](QQuickItem const* item) {
                    return item->objectName() == QStringLiteral("toast");
                });
                REQUIRE(toast != items.end());
                auto const* text = (*toast)->findChild<QObject*>(QStringLiteral("toastText"));
                REQUIRE(text != nullptr);
                CHECK(text->property("text").toString() == QStringLiteral("Copied 3 lines"));
                break;
            }
            case SnapshotScript::KitDialog: {
                auto const* dialog = scene.overlay();
                REQUIRE(dialog != nullptr);
                CHECK(dialog->objectName() == QStringLiteral("kitDialog"));
                CHECK(dialog->property("opened").toBool());
                break;
            }
            case SnapshotScript::HostingBanner: {
                auto const* bar = scene.root().findChild<QQuickItem*>(QStringLiteral("hostingNoticeBar"));
                REQUIRE(bar != nullptr);
                CHECK(bar->isVisible());
                break;
            }
            case SnapshotScript::NewDefaultsBanner: {
                auto const* bar = scene.root().findChild<QQuickItem*>(QStringLiteral("newDefaultsBanner"));
                REQUIRE(bar != nullptr);
                CHECK(bar->isVisible());
                break;
            }
            case SnapshotScript::TabMenu: {
                auto const* menu = scene.root().findChild<QObject*>(QStringLiteral("tabContextMenu"));
                REQUIRE(menu != nullptr);
                CHECK(menu->property("opened").toBool());
                CHECK(visibleItemsNamed(*scene.window().contentItem(), QStringLiteral("menuSwatch")) > 0);
                break;
            }
            case SnapshotScript::About: {
                auto const* dialog = scene.root().findChild<QObject*>(QStringLiteral("aboutDialog"));
                REQUIRE(dialog != nullptr);
                CHECK(dialog->property("opened").toBool());
                break;
            }
            case SnapshotScript::TabBadges: {
                auto* tabList = scene.root().findChild<QObject*>(QStringLiteral("tabList"));
                REQUIRE(tabList != nullptr);
                CHECK(tabList->property("count").toInt() == row.tabCount);
                // Every kind but None shows in its own tab's slot.
                CHECK(visibleItemsNamed(*scene.window().contentItem(), QStringLiteral("tabBadgeSlot"))
                      == static_cast<int>(contour::window::TabBadgeKindCount) - 1);
                // In the chrome's own font, a badge costs a short title nothing: "Tab 2" is never elided.
                for (auto const index: std::views::iota(0, row.tabCount))
                {
                    QQuickItem* tab = nullptr;
                    QMetaObject::invokeMethod(
                        tabList, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, tab), Q_ARG(int, index));
                    REQUIRE(tab != nullptr);
                    auto const* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
                    REQUIRE(label != nullptr);
                    CAPTURE(index);
                    CHECK_FALSE(label->property("truncated").toBool());
                }
                break;
            }
            case SnapshotScript::TabOverflow: {
                auto const* tabList = scene.root().findChild<QObject*>(QStringLiteral("tabList"));
                REQUIRE(tabList != nullptr);
                CHECK(tabList->property("count").toInt() == row.tabCount);
                CHECK(tabList->property("currentIndex").toInt() == row.activeTab);
                break;
            }
            case SnapshotScript::TabColorFlyout: {
                auto const* flyout = scene.openedTabColorFlyout();
                REQUIRE(flyout != nullptr);
                CHECK(flyout->objectName() == QStringLiteral("tabColorFlyout"));
                CHECK(flyout->property("tabIndex").toInt() == 1); // the second tab's
                break;
            }
            case SnapshotScript::RemoteConnected:
            case SnapshotScript::RemoteDisconnected: {
                auto const* chip = scene.root().findChild<QQuickItem*>(QStringLiteral("hostChip"));
                REQUIRE(chip != nullptr);
                CHECK(chip->isVisible() == titleBar->isVisible());
                auto const* status = chip->findChild<QObject*>(QStringLiteral("hostChipStatus"));
                REQUIRE(status != nullptr);
                CHECK(status->property("text").toString().isEmpty()
                      == (variant.script == SnapshotScript::RemoteConnected));
                // The chip clears the leading window controls (macOS: the traffic lights' inset).
                auto const* leading =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
                REQUIRE(leading != nullptr);
                if (chip->isVisible())
                    CHECK(chip->mapToItem(titleBar, QPointF(0, 0)).x() >= leading->width());
                if (chip->isVisible() && variant.row == contour::config::ChromeStyle::MacOS)
                    CHECK(leading->width() > 0);
                break;
            }
            case SnapshotScript::DaemonDrift: {
                auto const* chip = scene.root().findChild<QQuickItem*>(QStringLiteral("daemonDriftBadge"));
                REQUIRE(chip != nullptr);
                CHECK(chip->isVisible());
                break;
            }
            case SnapshotScript::NativeTitleBar: {
                auto const* leading =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
                REQUIRE(leading != nullptr);
                CHECK(leading->width() == 0);
                break;
            }
            case SnapshotScript::FullScreenReveal: {
                CHECK(titleBar->property("revealed").toBool());
                // Revealed below AppKit's strip, which holds the traffic lights, the band's leading corner is
                // free.
                auto const* leading =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
                REQUIRE(leading != nullptr);
                CHECK(leading->width() == 0);
                break;
            }
            case SnapshotScript::BandSolid: {
                // Reduce Transparency: the band is the solid tint, opaque wherever it is drawn.
                auto const* background =
                    scene.root().findChild<QQuickItem*>(QStringLiteral("bandBackground"));
                REQUIRE(background != nullptr);
                if (variant.row == contour::config::ChromeStyle::MacOS)
                    CHECK(background->property("color").value<QColor>().alphaF() == 1.0);
                break;
            }
            case SnapshotScript::MacOSHost: {
                // On a macOS host the Terminal row's band grows by whole cells until it holds the native
                // title bar (S3); the macOS row's is the title bar's height, checked above. The Terminal
                // row's material.titleBand is None, so its surface grows nothing: no compact toolbar, the
                // bare 32-pt title bar, and the traffic lights where a bare title bar puts them.
                REQUIRE(variantHost(variant) == contour::config::HostPlatform::MacOS);
                auto const* win = scene.root().property("win").value<QObject*>();
                REQUIRE(win != nullptr);
                if (variant.row != contour::config::ChromeStyle::Terminal)
                {
                    CHECK(win->property("nativeControlsInset").toReal()
                          == ChromeSnapshotScene::SnapshotTrafficLightInset);
                    break;
                }
                auto const* context = QQmlEngine::contextForObject(&scene.root());
                REQUIRE(context != nullptr);
                auto const* style = context->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
                REQUIRE(style != nullptr);
                auto const cell = style->property("cellHeight").toReal();
                REQUIRE(cell > 0);
                CHECK(titleBar->height()
                      == cell * std::ceil(ChromeSnapshotScene::SnapshotBareTitleBarHeight / cell));
                CHECK(titleBar->height() >= ChromeSnapshotScene::SnapshotBareTitleBarHeight);
                CHECK(win->property("nativeControlsInset").toReal()
                      == ChromeSnapshotScene::SnapshotBareTrafficLightInset);
                break;
            }
            case SnapshotScript::SplitMinimal: {
                // The held modifier reveals the headers over the panes, which a bare line divides.
                auto const* header = scene.root().findChild<QQuickItem*>(QStringLiteral("paneHeader"));
                REQUIRE(header != nullptr);
                CHECK(header->property("policy").toMap().value(QStringLiteral("placement")).toString()
                      == QStringLiteral("overlay"));
                CHECK(visibleItemsNamed(content, QStringLiteral("splitLine"))
                          + visibleItemsNamed(content, QStringLiteral("splitLineGlyphs"))
                      >= 2);
                break;
            }
            case SnapshotScript::SplitHeaderHover: {
                // The active header shows its actions; the gap beside it is engaged, its grabber shown
                // wherever the row draws one (the Terminal row's is the engaged line of box glyphs).
                auto const items = [&](QString const& name) {
                    return ChromeSnapshotScene::itemsNamed(content, name);
                };
                CHECK(std::ranges::count_if(
                          items(QStringLiteral("splitHandle")),
                          [](QQuickItem const* handle) { return handle->property("engaged").toBool(); })
                      == 1);
                CHECK(std::ranges::count_if(items(QStringLiteral("paneHeaderActions")),
                                            [](QQuickItem const* actions) { return actions->opacity() == 1; })
                      == 1);
                auto const grabberDrawn =
                    contour::config::ChromeTokenRows[static_cast<std::size_t>(variant.row)].size.grabber > 0;
                CHECK(visibleItemsNamed(content, QStringLiteral("splitGrabber")) == (grabberDrawn ? 1 : 0));
                break;
            }
            case SnapshotScript::PaneExited: {
                // The last pane ended: its footer says how, below its cells; no other pane has one.
                CHECK(visibleItemsNamed(content, QStringLiteral("exitedFooter")) == 1);
                auto const messages =
                    ChromeSnapshotScene::itemsNamed(content, QStringLiteral("exitedFooterMessage"));
                CHECK(std::ranges::count_if(messages,
                                            [](QQuickItem const* message) {
                                                return message->isVisible()
                                                       && message->property("text").toString()
                                                              == QString::fromUtf8(SnapshotExitMessage);
                                            })
                      == 1);
                break;
            }
            case SnapshotScript::SingleTabMinimal:
                // One tab under Minimal: macOS keeps a bare band over the bare 32-pt title bar, in the
                // terminal's background, wherever the strip is not asked for; the Terminal row hides its bar.
                // Under Classic's tab_bar_visibility Always the strip holds the one tab, and the band its
                // ordinary height.
                if (variant.row == contour::config::ChromeStyle::MacOS
                    && band.centre == contour::window::BarCentre::Empty)
                {
                    CHECK(titleBar->isVisible());
                    CHECK(titleBar->height() == ChromeSnapshotScene::SnapshotBareTitleBarHeight);
                }
                if (variant.row == contour::config::ChromeStyle::Terminal)
                    CHECK_FALSE(titleBar->isVisible());
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
    INFO("Quick Controls style: " << controls << " (set " << contour::test::ControlsStyleVariable << ")");
    REQUIRE(controls == contour::test::controlsStyleFor(environment));
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
        REQUIRE(QTest::qWaitForWindowExposed(&scene.subject(), 3000));
        auto const image = scene.grab();
        CHECK(scene.diagnostics().isEmpty());
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == scene.subject().size() * scene.subject().devicePixelRatio());
        auto const facts = SnapshotFacts {
            .platform = QGuiApplication::platformName().toStdString(),
            .devicePixelRatio = scene.subject().devicePixelRatio(),
            .chromeFontFamily = QFontInfo(scene.chromeFont()).family().toStdString(),
            .controlsStyle = controls,
            .qtVersion = qVersion(),
        };
        CHECK(writer->write(variant, image, facts).has_value());
    }
}
