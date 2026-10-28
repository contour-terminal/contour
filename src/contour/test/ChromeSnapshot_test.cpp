// SPDX-License-Identifier: Apache-2.0
//
// Offscreen checks of the chrome snapshot scene: every script builds as Main.qml over mocks, a scene
// gives the application palette back, and the bundled chrome fonts register. The pictures themselves
// need a display: the last case renders them, on the display route only, and only when
// CONTOUR_SNAPSHOT_DIR names where they go.

#include <contour/command/BlockMenu.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/session/SearchStatus.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/ChromeSnapshotScene.hpp>
#include <contour/test/ChromeSnapshotWriter.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TitleBandLayout.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/Environment.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSizeF>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QFont>
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
#include <limits>
#include <ranges>

#include <QtQuickControls2/QQuickStyle>
#include <QtTest/QTest>

namespace
{
/// How many items named @p name beneath @p root, in the visual tree, are visible.
[[nodiscard]] int visibleItemsNamed(QQuickItem const& root, QString const& name)
{
    return static_cast<int>(std::ranges::count_if(contour::test::itemsNamed(root, name),
                                                  [](QQuickItem const* item) { return item->isVisible(); }));
}
/// @return The tab index of every tab beneath @p root that draws its focus ring (or focus fill), in order.
[[nodiscard]] std::vector<int> ringedTabs(QQuickItem const& root)
{
    auto shown = std::vector<int> {};
    for (auto const* ring: contour::test::itemsNamed(root, QStringLiteral("focusRing")))
        if (auto const* tab = ring->parentItem();
            ring->isVisible() && tab != nullptr && tab->property("tabIndex").isValid())
            shown.push_back(tab->property("tabIndex").toInt());
    return shown;
}

/// Role @p role of the palette model's row @p row.
[[nodiscard]] QVariant paletteRole(contour::window::CommandPaletteModel const& model,
                                   int row,
                                   contour::window::CommandPaletteModel::Roles role)
{
    return model.data(model.index(row, 0), static_cast<int>(role));
}

/// The first group header the scene's palette draws: the first row that names a group.
[[nodiscard]] QString firstPaletteGroupHeader(contour::test::snapshot::ChromeSnapshotScene const& scene)
{
    auto const& model = scene.paletteModel();
    for (auto const row: std::views::iota(0, model.rowCount()))
        if (auto title = paletteRole(model, row, contour::window::CommandPaletteModel::Roles::GroupTitleRole)
                             .toString();
            !title.isEmpty())
            return title;
    return {};
}
} // namespace

namespace
{
/// @return The dialogs @p scene's script left open: the kit dialog it raised, and the window's own.
[[nodiscard]] std::vector<QObject const*> openedDialogs(
    contour::test::snapshot::ChromeSnapshotScene const& scene)
{
    auto dialogs = std::vector<QObject const*> {};
    if (auto const* overlay = scene.overlay(); overlay != nullptr && overlay->property("opened").toBool())
        dialogs.push_back(overlay);
    for (auto const* name: { "aboutDialog", "saveLayoutDialog", "permissionDialog" })
        if (auto const* dialog = scene.root().findChild<QObject*>(QString::fromLatin1(name));
            dialog != nullptr && dialog->property("opened").toBool())
            dialogs.push_back(dialog);
    return dialogs;
}

/// Checks that the title bar of @p scene is shown, and drawn as high, as the band table says for @p variant.
/// @return The band the table resolved.
[[nodiscard]] contour::window::TitleBand checkTitleBand(
    contour::test::snapshot::ChromeSnapshotScene const& scene,
    contour::test::snapshot::SnapshotVariant const& variant,
    QQuickItem const& titleBar)
{
    using namespace contour::test::snapshot;
    auto const& row = scriptRow(variant.script);
    auto const look = snapshotLook(variant.edition, variant.appearance);
    // Review focus 5: Native2026 hides a one-tab strip, Classic does not -- except on the macOS band,
    // which always shows (1c D1). The band table decides, from the row's tab shape and the script's
    // frame and mode.
    using contour::window::TabStripGate;
    auto const band = contour::window::resolveTitleBand(
        contour::window::TitleBandRow {
            .shape = contour::config::ChromeTokenRows[static_cast<std::size_t>(variant.row)].layout.tabShape,
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
            .notice = contour::window::hostNoticeOf(contour::window::hostChipPresence(row.hostLabel),
                                                    ChromeSnapshotScene::hostChipStateFor(row.connection)),
        });
    CHECK(titleBar.isVisible() == (band.band != contour::window::BarPresence::Hidden));
    // The macOS row's windowed band is drawn at the height of the title bar it covers (S2): the one its
    // surface grows -- 40 pt under the compact toolbar, Minimal's bare band the bare 32 pt (P3).
    // Only on a macOS host: elsewhere there is no title bar under the band to measure.
    if (variant.row == contour::config::ChromeStyle::MacOS && row.mode == SnapshotWindowMode::Windowed
        && row.frame == SnapshotFrame::ClientBand && titleBar.isVisible()
        && variantHost(variant) == contour::config::HostPlatform::MacOS)
    {
        REQUIRE(scene.measuredTitleBar().has_value());
        CHECK(titleBar.height() == scene.measuredTitleBar()->height);
    }
    return band;
}

/// Checks that @p scene draws the split of @p variant as cards with headers, or as bare lines under Minimal.
void checkSplitCards(contour::test::snapshot::ChromeSnapshotScene const& scene,
                     contour::test::snapshot::SnapshotVariant const& variant)
{
    using namespace contour::test::snapshot;
    auto const& row = scriptRow(variant.script);
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
}

/// Checks what a script that shows the sample command block drew: the block menu, the ⋯ button, and the
/// sticky header in a pane header or in the title band.
/// @param scene The scene the script ran in.
/// @param row The script's row.
/// @param content The window's content item.
void checkBlockScript(contour::test::snapshot::ChromeSnapshotScene const& scene,
                      contour::test::snapshot::SnapshotScriptRow const& row,
                      QQuickItem const& content)
{
    using namespace contour::test::snapshot;
    switch (row.script)
    {
        case SnapshotScript::BlockMenu: {
            // The terminal's menu is open on the block's: its four header rows, a separator and five
            // commands.
            auto const* menu = scene.root().findChild<QObject*>(QStringLiteral("terminalContextMenu"));
            REQUIRE(menu != nullptr);
            CHECK(menu->property("opened").toBool());
            // Below the band that rests above the content: it does not cover the centred tab title.
            auto const* const band = scene.root().findChild<QQuickItem*>(QStringLiteral("titleBar"));
            if (band != nullptr && band->isVisible())
                CHECK(menu->property("y").toReal() >= band->y() + band->height());
            CHECK(menu->property("count").toInt()
                  == static_cast<int>(SnapshotBlockMenuHeader.size() + 1
                                      + contour::command::BlockMenuRowCount));
            break;
        }
        case SnapshotScript::BlockActions:
            // The pointer is on the block's strip: its ⋯ button shows, beside nothing but the strip.
            CHECK(visibleItemsNamed(content, QStringLiteral("blockActionsButton")) == 1);
            break;
        case SnapshotScript::StickyPaneHeader: {
            // The active pane's header names the failed block; the other panes' headers keep their names.
            auto const stickies = contour::test::itemsNamed(content, QStringLiteral("paneHeaderSticky"));
            REQUIRE(stickies.size() == paneCount(row.panes));
            CHECK(std::ranges::count_if(stickies, [](QQuickItem const* each) { return each->isVisible(); })
                  == 1);
            auto const shown =
                std::ranges::find_if(stickies, [](QQuickItem const* each) { return each->isVisible(); });
            REQUIRE(shown != stickies.end());
            CHECK((*shown)->property("text").toString().startsWith(
                QString::fromUtf8(SnapshotStickyFailed.glyph)));
            CHECK(visibleItemsNamed(content, QStringLiteral("paneHeaderJump")) == 1);
            break;
        }
        case SnapshotScript::StickyTitleBand: {
            // Where the band shows the title, it is the succeeded block's header; the pane has no header.
            CHECK(visibleItemsNamed(content, QStringLiteral("paneHeaderSticky")) == 0);
            auto const* title = scene.root().findChild<QQuickItem*>(QStringLiteral("bandTitle"));
            REQUIRE(title != nullptr);
            if (title->isVisible())
                CHECK(title->property("text").toString().startsWith(
                    QString::fromUtf8(SnapshotStickySucceeded.glyph)));
            break;
        }
        default: break;
    }
}
} // namespace

TEST_CASE("A sample block's sticky header is shown somewhere on every row (offscreen)",
          "[contour][gui][qml][snapshot][stickyheader]")
{
    using namespace contour::test::snapshot;
    using contour::window::StickyHeaderHost;
    for (auto const row: { contour::config::ChromeStyle::MacOS,
                           contour::config::ChromeStyle::Fluent,
                           contour::config::ChromeStyle::ContourLinux,
                           contour::config::ChromeStyle::Terminal })
    {
        auto const variant = SnapshotVariant { .script = SnapshotScript::StickyTitleBand,
                                               .row = row,
                                               .edition = SnapshotEditions.front(),
                                               .appearance = vtbackend::ColorPreference::Dark };
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();

        // The card decides where the lone pane's header lives; the band shows the title or it does not.
        auto const& hosts = scene.activeSession().stickyHosts;
        REQUIRE_FALSE(hosts.empty());
        auto const host = static_cast<StickyHeaderHost>(hosts.back());
        auto const* title = scene.root().findChild<QQuickItem*>(QStringLiteral("bandTitle"));
        auto const bandShows = title != nullptr && title->isVisible()
                               && title->property("text").toString().startsWith(
                                   QString::fromUtf8(SnapshotStickySucceeded.glyph));
        CHECK((host == StickyHeaderHost::TitleBand) == bandShows);

        // Where no band shows it, the grid draws it, so the chrome carries nothing: it is never withheld
        // from the grid and shown nowhere.
        CHECK(scene.activeSession().stickyHeader().isEmpty() == (host == StickyHeaderHost::Grid));
        CHECK(host != StickyHeaderHost::PaneHeader);
    }
}

TEST_CASE("The block actions button and its glyph stay inside the terminal on every row (offscreen)",
          "[contour][gui][qml][snapshot][blockactions]")
{
    using namespace contour::test::snapshot;
    for (auto const row: { contour::config::ChromeStyle::MacOS,
                           contour::config::ChromeStyle::Fluent,
                           contour::config::ChromeStyle::ContourLinux,
                           contour::config::ChromeStyle::Terminal })
    {
        auto const variant = SnapshotVariant { .script = SnapshotScript::BlockActions,
                                               .row = row,
                                               .edition = SnapshotEditions.front(),
                                               .appearance = vtbackend::ColorPreference::Dark };
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();

        auto const buttons =
            contour::test::itemsNamed(*scene.window().contentItem(), QStringLiteral("blockActionsButton"));
        auto const shown =
            std::ranges::find_if(buttons, [](QQuickItem const* each) { return each->isVisible(); });
        REQUIRE(shown != buttons.end());
        auto* const button = *shown;
        auto const* const terminal = button->parentItem();
        auto const* const glyph = button->findChild<QQuickItem*>(QStringLiteral("blockActionsGlyph"));
        REQUIRE(glyph != nullptr);

        // The button is inside the pane, and the glyph between the button's sides: the same inset on every
        // row, so none is cut at the pane's edge.
        auto const bounds = QRectF { 0, 0, terminal->width(), terminal->height() };
        auto const buttonRect =
            button->mapRectToItem(terminal, QRectF { 0, 0, button->width(), button->height() });
        CHECK(bounds.contains(buttonRect));
        // Less a rounding slack: a glyph exactly one hairline in is inside, though its mapped edge may land a
        // few ulps short of it (seen on aarch64).
        auto const hairline = (1.0 / std::max(1.0, scene.window().devicePixelRatio())) - 1e-6;
        auto const glyphRectIn = [&](QRectF const& host) {
            auto const glyphRect = glyph->mapRectToItem(
                terminal, QRectF { 0, 0, glyph->implicitWidth(), glyph->implicitHeight() });
            CHECK(glyphRect.left() - host.left() >= hairline);
            CHECK(host.right() - glyphRect.right() >= hairline);
            // Centred exactly: a glyph rounded onto whole pixels sat half a pixel nearer one side whenever
            // the room it left was odd, half a hairline from the edge on the Terminal row (seen on Windows,
            // whose font left an odd cell).
            CHECK(std::abs((glyphRect.left() - host.left()) - (host.right() - glyphRect.right())) < 1e-6);
            return glyphRect;
        };
        CHECK(glyphRectIn(buttonRect).left() >= bounds.left() + hairline);

        // One pixel wider, the room left beside the glyph changes parity, whichever this host's font left.
        button->setWidth(button->width() + 1);
        glyphRectIn(button->mapRectToItem(terminal, QRectF { 0, 0, button->width(), button->height() }));
    }
}

namespace
{
/// Checks that @p scene's Settings window shows the page @p row names, built.
/// @param scene A scene of a Settings window script.
/// @param row   The script's row.
void checkSettingsScene(contour::test::snapshot::ChromeSnapshotScene const& scene,
                        contour::test::snapshot::SnapshotScriptRow const& row)
{
    using namespace contour::test::snapshot;
    CHECK(scene.subject().objectName() == QStringLiteral("settingsWindow"));
    CHECK(scene.subject().isVisible());
    auto const* settingsPage = scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsPage"));
    REQUIRE(settingsPage != nullptr);
    CHECK(scene.root().findChild<QObject*>(QStringLiteral("settingsPageLoader")) == nullptr);
    // The window is on the page the script names, and the page it shows is built.
    CHECK(settingsPage->property("currentPage").toString().toStdString() == settingsPageId(row.settingsPage));
    auto const shows = [&](char const* objectName) {
        return scene.subject().findChild<QQuickItem*>(QString::fromLatin1(objectName)) != nullptr;
    };
    switch (row.settingsPage)
    {
        case SnapshotSettingsPage::None: FAIL("a Settings script names a page"); break;
        case SnapshotSettingsPage::General:
        case SnapshotSettingsPage::Appearance:
        case SnapshotSettingsPage::Keyboard: CHECK(shows("settingsSectionPage")); break;
        case SnapshotSettingsPage::ShellIntegration: {
            CHECK(shows("settingsSectionPage"));
            // The Install group, with a row for each shell, and the cut-short one's line to delete.
            auto const* group = scene.subject().findChild<QQuickItem*>(QStringLiteral("shellInstallGroup"));
            REQUIRE(group != nullptr);
            CHECK(group->isVisible());
            auto const& page = *settingsPage;
            for (auto const& shell: contour::config::ShellRows)
                CHECK(contour::test::itemsNamed(page,
                                                QStringLiteral("shellInstallRow:")
                                                    + QString::fromUtf8(shell.name.data(), shell.name.size()))
                          .size()
                      == 1);
            auto const cut = contour::test::itemsNamed(page, QStringLiteral("shellInstallDeleteLine"));
            CHECK(std::ranges::count_if(cut, [](QQuickItem const* item) { return item->isVisible(); }) == 1);
            break;
        }
        case SnapshotSettingsPage::Profile: CHECK(shows("profilePage")); break;
        case SnapshotSettingsPage::ProfileReadOnly:
            CHECK(shows("profilePage"));
            // contour.yml's own profile: the banner and its Duplicate to Edit.
            CHECK(scene.subject().findChild<QQuickItem*>(QStringLiteral("readOnlyBanner"))->isVisible());
            break;
        case SnapshotSettingsPage::ColorSchemes: CHECK(shows("colorSchemesPage")); break;
        case SnapshotSettingsPage::LinkPatterns: CHECK(shows("linkPatternsPage")); break;
        case SnapshotSettingsPage::StatusLine: CHECK(shows("statusLinePage")); break;
    }
    // The picture is taken once the page's fade-in ran out, or it catches the page half transparent: the
    // dimmed pages of the first display pass.
    auto const* loader = scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsPageLoader"));
    REQUIRE(loader != nullptr);
    CHECK(loader->opacity() == 1.0);
    // The sidebar's own contents (its search field above all) stay inside the sidebar, which ends where the
    // page's header begins: nothing of it lies under the title.
    auto const* sidebar = scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsSidebar"));
    auto const* header = scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsHeader"));
    auto const* search = scene.subject().findChild<QQuickItem*>(QStringLiteral("settingsSearchField"));
    REQUIRE(sidebar != nullptr);
    REQUIRE(header != nullptr);
    REQUIRE(search != nullptr);
    auto const sceneRect = [](QQuickItem const& item) {
        return item.mapRectToScene(QRectF { 0, 0, item.width(), item.height() });
    };
    CHECK(sceneRect(*sidebar).right() <= sceneRect(*header).left() + 0.5);
    CHECK(sceneRect(*sidebar).contains(sceneRect(*search)));
    // `work` is the editable profile, `main` the one contour.yml defines.
    CHECK(scene.subject().findChild<QQuickItem*>(QStringLiteral("readOnlyBanner"))->isVisible()
          == (row.settingsPage == SnapshotSettingsPage::ProfileReadOnly));
}

/// Checks that @p content shows exactly one shell integration card, saying @p stage's title.
void checkShellCard(QQuickItem const& content, contour::test::snapshot::SnapshotShellCard stage)
{
    using contour::test::snapshot::SnapshotShellCard;
    auto const isVisible = [](QQuickItem const* item) {
        return item->isVisible();
    };
    auto const cards = contour::test::itemsNamed(content, QStringLiteral("shellIntegrationCard"));
    REQUIRE(std::ranges::count_if(cards, isVisible) == 1);
    auto const& words = contour::window::CardStageRows[std::to_underlying(
        stage == SnapshotShellCard::Offer ? contour::window::CardStage::Offer
                                          : contour::window::CardStage::Confirm)];
    auto const* title = (*std::ranges::find_if(cards, isVisible))
                            ->findChild<QObject*>(QStringLiteral("shellIntegrationTitle"));
    REQUIRE(title != nullptr);
    CHECK(title->property("text").toString() == QString::fromUtf8(words.title));
}
} // namespace

namespace
{
/// Checks the Windows host's caption scripts on @p variant's row: under caption-hover the OS-hovered
/// maximise button is lit (the Terminal row's cells reveal their glyphs instead of the dot), and under
/// inactive-window the close glyph is dimmed (the Terminal row's cells keep their dots).
/// @param scene The shown scene.
/// @param variant Its picture; its script is CaptionHover or InactiveWindow.
void checkWindowsCaption(contour::test::snapshot::ChromeSnapshotScene const& scene,
                         contour::test::snapshot::SnapshotVariant const& variant)
{
    using namespace contour::test::snapshot;
    REQUIRE(variantHost(variant) == contour::config::HostPlatform::Windows);
    auto const& content = *scene.window().contentItem();
    auto const* context = QQmlEngine::contextForObject(&scene.root());
    REQUIRE(context != nullptr);
    auto const* style = context->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const dot = style->property("trafficLightGlyph").toString();
    if (variant.row == contour::config::ChromeStyle::Terminal)
    {
        // R-n: on a Windows host the Terminal row's controls are scheme-inked cells.
        auto const cells = contour::test::itemsNamed(content, QStringLiteral("windowControlCell_maximize"));
        REQUIRE(cells.size() == 1);
        auto const text = cells.front()->property("text").toString();
        CHECK((text == dot) == (variant.script == SnapshotScript::InactiveWindow));
        return;
    }
    if (variant.script == SnapshotScript::CaptionHover)
    {
        auto const fills = contour::test::itemsNamed(content, QStringLiteral("windowControlFill_maximize"));
        REQUIRE(fills.size() == 1);
        CHECK(fills.front()->isVisible());
        CHECK(fills.front()->property("color").value<QColor>().alpha() > 0);
        return;
    }
    CHECK_FALSE(scene.window().isActive());
    auto const glyphs = contour::test::itemsNamed(content, QStringLiteral("windowControlGlyph_close"));
    REQUIRE(glyphs.size() == 1);
    auto const* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    // By value: the binding hands back the colour in another spec than the role's.
    CHECK(glyphs.front()->property("color").value<QColor>().rgba()
          == colors->property("captionGlyphInactive").value<QColor>().rgba());
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
        auto* titleBar = scene.root().findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        auto const band = checkTitleBand(scene, variant, *titleBar);
        auto const& content = *scene.window().contentItem();
        checkSplitCards(scene, variant);
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
                // No history in the scene: the first group is the table's first, and the profiles are one
                // drill-in row.
                CHECK(firstPaletteGroupHeader(scene) == QStringLiteral("Pane"));
                auto const& model = scene.paletteModel();
                CHECK(std::ranges::any_of(std::views::iota(0, model.rowCount()), [&](int row) {
                    return paletteRole(model, row, contour::window::CommandPaletteModel::Roles::KindRole)
                               .toInt()
                           == model.drillInKind();
                }));
                break;
            }
            case SnapshotScript::PaletteFiltered: {
                auto const* palette = scene.root().findChild<QObject*>(QStringLiteral("commandPalette"));
                REQUIRE(palette != nullptr);
                CHECK(palette->property("opened").toBool());
                CHECK(firstPaletteGroupHeader(scene) == QStringLiteral("Pane"));
                auto const& model = scene.paletteModel();
                CHECK(std::ranges::any_of(std::views::iota(0, model.rowCount()), [&](int row) {
                    return paletteRole(model, row, contour::window::CommandPaletteModel::Roles::TitleRole)
                               .toString()
                           == QStringLiteral("Launch Layout · split-dev");
                }));
                break;
            }
            case SnapshotScript::PaletteDrilledIn: {
                // The scene waited for the level (drillInPalette); the palette is still up over it.
                auto const* palette = scene.root().findChild<QObject*>(QStringLiteral("commandPalette"));
                REQUIRE(palette != nullptr);
                CHECK(palette->property("opened").toBool());
                auto const& model = scene.paletteModel();
                CHECK(model.levelTitle() == QStringLiteral("Change Profile"));
                auto const* list = scene.root().findChild<QObject*>(QStringLiteral("commandPaletteList"));
                REQUIRE(list != nullptr);
                CHECK(list->property("count").toInt() == scene.paletteProfileCount());
                break;
            }
            case SnapshotScript::Settings:
            case SnapshotScript::SettingsAppearance:
            case SnapshotScript::SettingsKeyboard:
            case SnapshotScript::SettingsShellIntegration:
            case SnapshotScript::SettingsProfile:
            case SnapshotScript::SettingsProfileReadOnly:
            case SnapshotScript::SettingsColorSchemes:
            case SnapshotScript::SettingsLinkPatterns:
            case SnapshotScript::SettingsStatusLine: checkSettingsScene(scene, row); break;
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
                // Posted through the window's notices: Main.qml's toast shows it, in the band where the band
                // holds it (the macOS row's always does).
                auto const* toast = scene.root().findChild<QQuickItem*>(QStringLiteral("noticeToast"));
                REQUIRE(toast != nullptr);
                CHECK(toast->property("shown").toBool());
                // Fully shown: the picture is taken once its fade-in ran out.
                CHECK(toast->opacity() == 1.0);
                CHECK(toast->property("text").toString() == QStringLiteral("Copied 3 lines"));
                if (variant.row == contour::config::ChromeStyle::MacOS)
                {
                    auto const laidOut = toast->parentItem()->mapRectToScene(
                        QRectF { toast->position(), QSizeF { toast->width(), toast->height() } });
                    CHECK(titleBar->mapRectToScene(QRectF { 0, 0, titleBar->width(), titleBar->height() })
                              .contains(laidOut.center()));
                }
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
                    return contour::test::itemsNamed(content, name);
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
                    contour::test::itemsNamed(content, QStringLiteral("exitedFooterMessage"));
                CHECK(std::ranges::count_if(messages,
                                            [](QQuickItem const* message) {
                                                return message->isVisible()
                                                       && message->property("text").toString()
                                                              == QString::fromUtf8(SnapshotExitMessage);
                                            })
                      == 1);
                break;
            }
            case SnapshotScript::PaneInterrupted: {
                // The last pane's shell was restored over a stopped command: its footer names it, with Run
                // Again and Dismiss; no other pane has one.
                CHECK(visibleItemsNamed(content, QStringLiteral("exitedFooter")) == 1);
                auto const messages =
                    contour::test::itemsNamed(content, QStringLiteral("exitedFooterMessage"));
                auto const expected = QString::fromStdString(contour::window::interruptedFooterMessage(
                    vtbackend::CommandLineText { std::string { SnapshotInterruptedCommand } }));
                CHECK(std::ranges::count_if(messages,
                                            [&](QQuickItem const* message) {
                                                return message->isVisible()
                                                       && message->property("text").toString() == expected;
                                            })
                      == 1);
                break;
            }
            case SnapshotScript::SingleTabMinimal:
                // One tab under Minimal: macOS keeps a bare band over the bare 32-pt title bar, in the
                // terminal's background, wherever the strip is not asked for. Under Classic's
                // tab_bar_visibility Always the strip holds the one tab, and the band its ordinary height;
                // every other row shows its bar as the gate says (the band table, checked above).
                if (variant.row == contour::config::ChromeStyle::MacOS
                    && band.centre == contour::window::BarCentre::Empty)
                {
                    CHECK(titleBar->isVisible());
                    CHECK(titleBar->height() == ChromeSnapshotScene::SnapshotBareTitleBarHeight);
                }
                break;
            case SnapshotScript::FindBar:
            case SnapshotScript::FindBarSplit:
            case SnapshotScript::FindBarInvalid: {
                // The active pane's bar stands open, its count saying what the search found; on the
                // Terminal row a split's bar sits in its pane header's row.
                auto* bar = static_cast<QObject*>(nullptr);
                for (auto* candidate: scene.root().findChildren<QObject*>(QStringLiteral("searchBar")))
                    if (candidate->property("opened").toBool())
                        bar = candidate;
                REQUIRE(bar != nullptr);
                auto* content = bar->property("contentItem").value<QQuickItem*>();
                REQUIRE(content != nullptr);
                auto const counts = contour::test::itemsNamed(*content, QStringLiteral("searchBarCount"));
                REQUIRE(counts.size() == 1);
                CHECK(counts.front()->property("text").toString()
                      == (row.find == SnapshotFind::Invalid ? QStringLiteral("Invalid pattern")
                                                            : QStringLiteral("2 of 5")));
                // Both searches run as a regex (ChromeSnapshotScene::searchIn): its toggle alone is lit.
                for (auto const& toggleRow: contour::session::SearchToggleRows)
                {
                    CAPTURE(toggleRow.objectName);
                    auto const toggles =
                        contour::test::itemsNamed(*content, QString::fromUtf8(toggleRow.objectName));
                    REQUIRE(toggles.size() == 1);
                    CHECK(toggles.front()->property("checked").toBool()
                          == (toggleRow.toggle == contour::session::SearchToggle::Regex));
                }
                auto const* parent = bar->property("parent").value<QQuickItem*>();
                REQUIRE(parent != nullptr);
                auto const inHeaderRow = variant.row == contour::config::ChromeStyle::Terminal
                                         && row.panes == PaneShape::SplitThree;
                CHECK((parent->objectName() == QStringLiteral("paneHeaderFindSlot")) == inHeaderRow);
                break;
            }
            case SnapshotScript::SizeIndicator: {
                // Each pane of the split shows its own size, "columns × lines", centred on it.
                auto const pills = contour::test::itemsNamed(content, QStringLiteral("sizeIndicator"));
                REQUIRE(pills.size() == paneCount(row.panes));
                auto texts = QStringList {};
                for (auto const* pill: pills)
                {
                    CHECK(QTest::qWaitFor([pill] { return pill->property("shown").toBool(); }));
                    auto const text = pill->property("text").toString();
                    CHECK(text.contains(QStringLiteral(" × ")));
                    texts << text;
                }
                // The split's panes are not all one size, and each pill reads its own.
                texts.removeDuplicates();
                CHECK(texts.size() > 1);
                break;
            }
            case SnapshotScript::LinkPill:
            case SnapshotScript::LinkPillSplit: {
                // The active pane names the link's target and how to open it: over the grid's blank cells on
                // one pane, in the pane header where a split gives it one.
                auto const pills = contour::test::itemsNamed(content, QStringLiteral("linkPill"));
                auto const shown = [](QQuickItem const* pill) {
                    return pill->property("shown").toBool();
                };
                REQUIRE(QTest::qWaitFor([&] { return std::ranges::count_if(pills, shown) == 1; }));
                auto const* pill = *std::ranges::find_if(pills, shown);
                CHECK(pill->property("text").toString() == QString::fromUtf8(SnapshotLinkTarget));
                CHECK_FALSE(pill->property("detail").toString().isEmpty());
                REQUIRE(pill->parentItem() != nullptr);
                auto const inHeader =
                    pill->parentItem()->objectName() == QStringLiteral("paneHeaderLinkSlot");
                CHECK(inHeader == (row.panes == PaneShape::SplitThree));
                break;
            }
            case SnapshotScript::ScrollbarOverlay: {
                // Scrolled back: the overlay bar shows, off the bottom, with the failure's and the running
                // command's ticks.
                auto const bars = contour::test::itemsNamed(content, QStringLiteral("verticalScrollBar"));
                REQUIRE(bars.size() == 1);
                auto const* bar = bars.front();
                CHECK(QTest::qWaitFor([bar] { return bar->isVisible() && bar->opacity() == 1.0; }));
                CHECK(bar->property("position").toReal()
                      < 1.0 - bar->property("size").toReal() - std::numeric_limits<double>::epsilon());
                CHECK(visibleItemsNamed(*bar, QStringLiteral("scrollbarMark")) == 2);
                break;
            }
            case SnapshotScript::SecureInput:
                // The active pane's header and both tabs carry the lock, wherever the row draws a header.
                CHECK(visibleItemsNamed(content, QStringLiteral("paneHeaderLock")) == 1);
                CHECK(visibleItemsNamed(content, QStringLiteral("tabLock")) == row.tabCount);
                break;
            case SnapshotScript::SecureInputBand: {
                // The lock on the tab where the edition shows the tab well, else as the band's chip where a
                // band is shown (a Flat row hides it with the strip, and the lock with it).
                auto const wellShown =
                    contour::config::isTabBarShown(scene.look().tabBarVisibility, row.tabCount);
                auto const* band = scene.root().findChild<QQuickItem*>(QStringLiteral("bandBackground"));
                auto const bandShown = band != nullptr && band->isVisible();
                CHECK(visibleItemsNamed(content, QStringLiteral("tabLock")) == (wellShown ? 1 : 0));
                CHECK(visibleItemsNamed(content, QStringLiteral("secureInputChip"))
                      == (!wellShown && bandShown ? 1 : 0));
                break;
            }
            case SnapshotScript::KeyboardFocus: {
                // Keyboard focus is on the second tab, and on it alone: the ring (or, where cells cannot draw
                // one, the focus fill) is drawn there and on no other tab.
                CHECK(ringedTabs(content) == std::vector<int> { SnapshotFocusedTab });
                break;
            }
            case SnapshotScript::BlockMenu:
            case SnapshotScript::BlockActions:
            case SnapshotScript::StickyPaneHeader:
            case SnapshotScript::StickyTitleBand: checkBlockScript(scene, row, content); break;
            case SnapshotScript::RestoreSummary: {
                // The same toast, posted as the first restored window's notices post it.
                auto const* toast = scene.root().findChild<QQuickItem*>(QStringLiteral("noticeToast"));
                REQUIRE(toast != nullptr);
                CHECK(toast->property("shown").toBool());
                CHECK(toast->opacity() == 1.0);
                CHECK(toast->property("text").toString() == QStringLiteral("Restored 2 windows, 7 tabs"));
                break;
            }
            case SnapshotScript::ShellIntegrationOffer:
            case SnapshotScript::ShellIntegrationConfirm: checkShellCard(content, row.shellCard); break;
            case SnapshotScript::SingleTab: break;
            case SnapshotScript::PermissionPaste: {
                auto const* dialog = scene.root().findChild<QObject*>(QStringLiteral("permissionDialog"));
                REQUIRE(dialog != nullptr);
                CHECK(dialog->property("opened").toBool());
                CHECK(dialog->property("acceptText").toString() == QStringLiteral("Paste"));
                break;
            }
            case SnapshotScript::PermissionConsent: {
                auto const* dialog = scene.root().findChild<QObject*>(QStringLiteral("permissionDialog"));
                REQUIRE(dialog != nullptr);
                CHECK(dialog->property("opened").toBool());
                auto const* dialogContent = dialog->property("contentItem").value<QQuickItem*>();
                REQUIRE(dialogContent != nullptr);
                auto const lines =
                    contour::test::itemsNamed(*dialogContent, QStringLiteral("permissionLine"));
                CHECK(std::ranges::count_if(
                          lines,
                          [](QQuickItem const* line) { return line->property("font").value<QFont>().bold(); })
                      == 1);
                break;
            }
            case SnapshotScript::SaveLayout:
            case SnapshotScript::SaveLayoutTaken: {
                auto const* dialog = scene.root().findChild<QObject*>(QStringLiteral("saveLayoutDialog"));
                REQUIRE(dialog != nullptr);
                CHECK(dialog->property("opened").toBool());
                auto const* dialogContent = dialog->property("contentItem").value<QQuickItem*>();
                REQUIRE(dialogContent != nullptr);
                auto const taken =
                    contour::test::itemsNamed(*dialogContent, QStringLiteral("saveLayoutTaken"));
                REQUIRE(taken.size() == 1);
                CHECK(taken.front()->isVisible() == (variant.script == SnapshotScript::SaveLayoutTaken));
                break;
            }
            case SnapshotScript::ConfigReloadBanner: {
                auto const* banner = scene.root().findChild<QQuickItem*>(QStringLiteral("noticeBanner"));
                REQUIRE(banner != nullptr);
                CHECK(banner->isVisible());
                break;
            }
            case SnapshotScript::CaptionHover:
            case SnapshotScript::InactiveWindow: checkWindowsCaption(scene, variant); break;
        }

        // A sheet hangs below the band, never over the tab strip: every dialog the script opened.
        if (variant.row == contour::config::ChromeStyle::MacOS && titleBar->isVisible())
            for (auto const* dialog: openedDialogs(scene))
            {
                INFO("dialog: " << dialog->objectName().toStdString());
                auto const* surface = dialog->property("background").value<QQuickItem*>();
                REQUIRE(surface != nullptr);
                CHECK(surface->mapToScene(QPointF(0, 0)).y()
                      >= titleBar->mapToScene(QPointF(0, titleBar->height())).y() - 0.5);
            }
    }
}

TEST_CASE("The Windows caption scripts on the rows this process draws (offscreen)",
          "[contour][gui][qml][snapshot]")
{
    // The Fluent row draws only under FluentWinUI3, so this is what covers it in that pass; the main case
    // above covers the macOS and Terminal rows in every pass.
    using namespace contour::test::snapshot;
    auto const rows = rowsRenderableUnder(QQuickStyle::name().toStdString());
    REQUIRE_FALSE(rows.empty());
    for (auto const& variant: snapshotMatrix(rows))
    {
        if (variant.script != SnapshotScript::CaptionHover
            && variant.script != SnapshotScript::InactiveWindow)
            continue;
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();
        CHECK(scene.diagnostics().isEmpty());
        checkWindowsCaption(scene, variant);
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
