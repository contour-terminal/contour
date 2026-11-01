// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end load test for Main.qml — the startup contract between the root
// ApplicationWindow and its WindowController:
//
//   * the window is NOT visible after the QML loads (visible:false at declaration; only the
//     controller's showInitial() maps it, AFTER the window has been sized from real metrics);
//   * Component.onCompleted runs the documented startup sequence in order:
//     createWindowController -> bindWindow(appWindow) -> createNewTab -> showInitial;
//   * the declared chrome (win.chromeHeight) equals the title bar's effective height;
//   * the whole load emits zero QML diagnostics (TypeErrors, binding loops, ...).
//
// The controller is a mock playing BOTH the manager (`terminalSessions` context property) and the
// per-window controller (createWindowController returns itself), mirroring the production split
// without PTYs or displays. activeTabRootPane stays null, so the pane tree (which would need a real
// TerminalDisplay) never instantiates — exactly the pre-first-session state.

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/test/TestEnvironment.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/HeaderBarLayout.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/StickyHeaderPlacement.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <core/Environment.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMimeData>
#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QKeyEvent>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickItemGrabResult>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vtworkspace/PaneLayout.hpp>

using contour::test::loadMainWindow;
using contour::test::MockMainController;
using contour::test::waitForPopupClosed;
using contour::test::waitForPopupOpened;

namespace
{
/// The band's height on @p engine's chrome row: the row's own token (UiStyleProvider::bandHeight).
[[nodiscard]] int bandHeightOf(QQmlEngine& engine)
{
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    return static_cast<int>(style->property("bandHeight").toReal());
}
} // namespace

TEST_CASE("Main.qml startup: sized-before-shown ordering and declared chrome (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    // 1. Startup order: mint the controller, bind THIS window, check for a torn-off tab to adopt
    //    (none here -> false), then a daemon window to adopt (not attached -> false), then a startup
    //    layout (none -> false), create the first tab (so the window can be sized from real metrics),
    //    and only then map it.
    CHECK(controller.calls
          == QStringList { QStringLiteral("createWindowController"),
                           QStringLiteral("bindWindow"),
                           QStringLiteral("consumePendingTransplant"),
                           QStringLiteral("consumeAttachWindow"),
                           QStringLiteral("consumeDefaultLayout"),
                           QStringLiteral("createNewTab"),
                           QStringLiteral("showInitial") });
    CHECK(controller.boundWindow == root.get());

    // 2. Visible-only-after-size: nothing in the QML sets visible; the (mocked, inert) showInitial
    //    is the only mapper, so the window must still be hidden after a full load.
    CHECK_FALSE(root->property("visible").toBool());

    // 3. Declared chrome: with the custom title bar shown, win.chromeHeight tracks the TitleBar's
    //    effective height (the row's band height, its implicitHeight).
    CHECK(controller.chromeHeight() == bandHeightOf(engine));
}

TEST_CASE("Main.qml tab strip: visibility gate collapses the chrome when hidden (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // When win.tabBarShouldShow is false (tab_bar_visibility: Never, or Multiple with a single tab),
    // the TitleBar collapses: its effectiveHeight -> 0 -> the declared chromeHeight -> 0, and the strip
    // item is not visible. When true, the strip occupies its implicitHeight, the row's band height (as the
    // startup test already pins). Driving the mock property proves the real Main.qml binding reacts.
    SECTION("hidden -> zero chrome, strip invisible")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.setTabBarShouldShow(false);
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        CHECK(controller.chromeHeight() == 0);
        auto* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        CHECK_FALSE(titleBar->isVisible());
    }

    SECTION("shown -> full chrome, strip visible")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.setTabBarShouldShow(true);
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        CHECK(controller.chromeHeight() == bandHeightOf(engine));
        auto* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        CHECK(titleBar->isVisible());
    }
}

TEST_CASE("Main.qml tab strip: position flips the title-bar / content stacking (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // The strip is pinned to the top OR bottom edge via conditional anchors; the content area fills the
    // opposite side. chromeHeight is position-invariant (the row's band height either way). We read the
    // laid-out y/height off the live items to prove the stacking flipped.
    auto const readGeometry = [](QObject& root, qreal& titleBarY, qreal& titleBarH, qreal& contentY) {
        auto* titleBar = root.findChild<QQuickItem*>(QStringLiteral("titleBar"));
        auto* content = root.findChild<QQuickItem*>(QStringLiteral("content"));
        REQUIRE(titleBar != nullptr);
        REQUIRE(content != nullptr);
        titleBarY = titleBar->y();
        titleBarH = titleBar->height();
        contentY = content->y();
    };

    SECTION("Top (default): title bar at y=0, content below it")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.setTabBarPosition(0);
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        qreal titleBarY = -1;
        qreal titleBarH = -1;
        qreal contentY = -1;
        readGeometry(*root, titleBarY, titleBarH, contentY);
        CHECK(titleBarY == 0.0);
        CHECK(contentY == titleBarH); // content starts right below the strip
        CHECK(controller.chromeHeight() == bandHeightOf(engine));
    }

    SECTION("Bottom: content at y=0, title bar below the content")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.setTabBarPosition(1);
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        qreal titleBarY = -1;
        qreal titleBarH = -1;
        qreal contentY = -1;
        readGeometry(*root, titleBarY, titleBarH, contentY);
        CHECK(contentY == 0.0);
        auto* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
        REQUIRE(content != nullptr);
        CHECK(titleBarY == content->height()); // strip sits just past the content's bottom edge
        CHECK(controller.chromeHeight() == bandHeightOf(engine));
    }
}

TEST_CASE("Main.qml window attention: each request reaches what acts on it (offscreen)",
          "[contour][gui][qml][mainwindow][notification]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* trayIcon = root->findChild<QObject*>(QStringLiteral("trayIcon"));
    REQUIRE(trayIcon != nullptr);

    SECTION("a notification goes to the tray")
    {
        emit controller.notificationRequested(QStringLiteral("make finished"), QStringLiteral("took 12s"));
        QCoreApplication::processEvents();

        // Where the platform's tray shows messages, the icon carries this one; where it has none
        // (offscreen, usually), the window says so instead. Either way the tray path ran.
        if (trayIcon->property("supportsMessages").toBool())
        {
            CHECK(trayIcon->property("visible").toBool());
            auto const* timer = root->findChild<QObject*>(QStringLiteral("trayIconTimer"));
            REQUIRE(timer != nullptr);
            CHECK(timer->property("running").toBool());
        }
        else
        {
            CHECK(warnings.count([](QString const& message) {
                return message.contains(QStringLiteral("Notification system not supported"));
            }) == 1);
        }
    }

    SECTION("a bell goes to the window's bell, kept until multimedia is ready")
    {
        emit controller.bellRequested(QStringLiteral("qrc:/contour/bell.wav"), 0.5);
        QCoreApplication::processEvents();

        auto const* bell = root->findChild<QObject*>(QStringLiteral("windowBell"));
        REQUIRE(bell != nullptr);
        CHECK(bell->property("pendingSource").toString() == QStringLiteral("qrc:/contour/bell.wav"));
        CHECK(bell->property("pendingVolume").toDouble() == 0.5);
    }

    SECTION("a click on a tray message reaches the window")
    {
        REQUIRE(QMetaObject::invokeMethod(trayIcon, "messageClicked"));
        QCoreApplication::processEvents();
        CHECK(controller.trayClickCount == 1);
    }

    SECTION("an alert runs")
    {
        emit controller.alertRequested();
        QCoreApplication::processEvents();
    }

    for (auto const& w: warnings.messages())
        UNSCOPED_INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml window bell: a bell rung before multimedia is ready plays once it is (offscreen)",
          "[contour][gui][qml][mainwindow][notification]")
{
    // A stand-in for BellSound.qml that counts its plays, so the deferral is observable without
    // QtMultimedia probing the host's audio drivers.
    QTemporaryDir const playerDir;
    REQUIRE(playerDir.isValid());
    auto const playerPath = playerDir.filePath(QStringLiteral("CountingBell.qml"));
    {
        QFile player(playerPath);
        REQUIRE(player.open(QIODevice::WriteOnly | QIODevice::Text));
        player.write("import QtQuick\n"
                     "Item {\n"
                     "    objectName: \"countingBell\"\n"
                     "    property string source: \"\"\n"
                     "    property int playCount: 0\n"
                     "    property real lastVolume: -1\n"
                     "    function play(volume) { playCount += 1; lastVolume = volume; }\n"
                     "}\n");
    }

    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* bell = root->findChild<QObject*>(QStringLiteral("windowBell"));
    REQUIRE(bell != nullptr);
    bell->setProperty("playerSource", QUrl::fromLocalFile(playerPath));

    emit controller.bellRequested(QStringLiteral("qrc:/contour/bell.wav"), 0.5);
    QCoreApplication::processEvents();
    CHECK(bell->property("pendingVolume").toDouble() == 0.5);
    CHECK(root->findChild<QObject*>(QStringLiteral("countingBell")) == nullptr); // nothing loaded yet

    controller.setMultimediaReady(true);
    QCoreApplication::processEvents();

    auto const* player = root->findChild<QObject*>(QStringLiteral("countingBell"));
    REQUIRE(player != nullptr);
    CHECK(player->property("playCount").toInt() == 1);
    CHECK(player->property("source").toString() == QStringLiteral("qrc:/contour/bell.wav"));
    CHECK(player->property("lastVolume").toDouble() == 0.5);
    CHECK(bell->property("pendingVolume").toDouble() < 0);

    // Once ready, a bell plays at once.
    emit controller.bellRequested(QStringLiteral("qrc:/contour/bell.wav"), 0.25);
    CHECK(player->property("playCount").toInt() == 2);
    CHECK(player->property("lastVolume").toDouble() == 0.25);

    for (auto const& w: warnings.messages())
        UNSCOPED_INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml tray icon: it leaves the tray once its message is over (offscreen)",
          "[contour][gui][qml][mainwindow][notification]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* trayIcon = root->findChild<QObject*>(QStringLiteral("trayIcon"));
    REQUIRE(trayIcon != nullptr);

    SECTION("the icon stays exactly as long as the message it shows")
    {
        auto const* timer = root->findChild<QObject*>(QStringLiteral("trayIconTimer"));
        REQUIRE(timer != nullptr);
        CHECK(timer->property("interval").toInt() == root->property("trayMessageTimeout").toInt());
        CHECK(root->property("trayMessageTimeout").toInt() > 0);
    }

    SECTION("the user coming back to the window makes the message moot")
    {
        trayIcon->setProperty("visible", true);
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        REQUIRE(window != nullptr);
        window->show();
        window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(window));
        CHECK_FALSE(trayIcon->property("visible").toBool());
    }
}

TEST_CASE("Main.qml tooltips: every attached tooltip shows its text as written (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // Every tooltip in the application is an attached one, and an engine shares one instance among them,
    // built by the style in use: Fusion's by default, whose text is AutoText. A link's URI holding
    // `<img src=...>`, a tab title or a command line would render as rich text, the image fetched on hover.
    // This binary pins Fusion, as `ui_style: native` does.
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    // Read through an Item, as the attached property must be: the window itself is none.
    auto* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
    REQUIRE(content != nullptr);
    auto expression = QQmlExpression(
        qmlContext(content), content, QStringLiteral("ToolTip.toolTip.contentItem.textFormat"));
    auto const textFormat = expression.evaluate();
    INFO("error: " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    CHECK(textFormat.toInt() == Qt::PlainText);
}

TEST_CASE("Main.qml has no settings page or settings tab (offscreen)", "[contour][gui][qml][mainwindow]")
{
    // Settings is a window of its own (SettingsWindow.qml): the terminal window neither swaps its
    // content area for the page nor pins a tab for it in the strip.
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    CHECK(root->findChild<QObject*>(QStringLiteral("settingsPageLoader")) == nullptr);
    CHECK(root->findChild<QObject*>(QStringLiteral("settingsTab")) == nullptr);
}

namespace
{
/// The hosting notice the tests set and expect, built on use rather than as a static QString.
QString hostingNotice()
{
    return QStringLiteral("Contour could not reach its daemon (refused). This window runs its "
                          "shells itself; they end when it closes.");
}
} // namespace

TEST_CASE(
    "Main.qml hosting notice: a Banner that pushes the content down and gives the room back (offscreen)",
    "[contour][gui][qml][mainwindow][hosting]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* bar = root->findChild<QQuickItem*>(QStringLiteral("hostingNoticeBar"));
    auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
    REQUIRE(bar != nullptr);
    REQUIRE(titleBar != nullptr);
    REQUIRE(content != nullptr);

    // Nothing to say while the sessions run where the launch wanted them.
    CHECK_FALSE(bar->isVisible());
    CHECK(content->y() == titleBar->height());
    auto const contentY = content->y();
    auto const contentHeight = content->height();

    controller.setHostingNotice(hostingNotice());
    QCoreApplication::processEvents();
    CHECK(bar->isVisible());
    CHECK(bar->height() > 0);
    auto const* text = bar->findChild<QObject*>(QStringLiteral("bannerText"));
    REQUIRE(text != nullptr);
    CHECK(text->property("text").toString() == hostingNotice());
    CHECK(content->y() == titleBar->height() + bar->height());
    CHECK(content->height() == contentHeight - bar->height());

    // Dismissed through the banner's own button: the controller is told, the bar goes, and the
    // content gets back exactly the room it gave.
    auto* dismiss = bar->findChild<QObject*>(QStringLiteral("bannerDismiss"));
    REQUIRE(dismiss != nullptr);
    REQUIRE(QMetaObject::invokeMethod(dismiss, "clicked"));
    QCoreApplication::processEvents();
    CHECK(controller.hostingNotice().isEmpty());
    CHECK_FALSE(bar->isVisible());
    CHECK(content->y() == contentY);
    CHECK(content->height() == contentHeight);

    for (auto const& w: warnings.messages())
        UNSCOPED_INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml hosting notice: with the tab bar at the bottom the banner is at the top (offscreen)",
          "[contour][gui][qml][mainwindow][hosting]")
{
    QQmlEngine engine;
    MockMainController controller;
    controller.setTabBarPosition(1);
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto* bar = root->findChild<QQuickItem*>(QStringLiteral("hostingNoticeBar"));
    auto const* stack = root->findChild<QQuickItem*>(QStringLiteral("bannerStack"));
    auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
    auto const* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(bar != nullptr);
    REQUIRE(stack != nullptr);
    REQUIRE(titleBar != nullptr);
    REQUIRE(content != nullptr);
    REQUIRE(window != nullptr);

    controller.setHostingNotice(hostingNotice());
    QCoreApplication::processEvents();
    REQUIRE(bar->isVisible());
    CHECK(stack->y() == 0);
    CHECK(content->y() == bar->height());
    CHECK(titleBar->y() + titleBar->height() == window->height());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml hosting notice: the Terminal row's banner is the scheme's palette[0] (offscreen)",
          "[contour][gui][qml][mainwindow][hosting]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;

    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Bar)] =
        vtbackend::RGBColor { 0x2A2D35u };
    auto root = loadMainWindow(engine, controller, warnings, setup);

    controller.setHostingNotice(hostingNotice());
    QCoreApplication::processEvents();
    auto const* bar = root->findChild<QQuickItem*>(QStringLiteral("hostingNoticeBar"));
    REQUIRE(bar != nullptr);
    REQUIRE(bar->isVisible());
    auto const* background = bar->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("color").value<QColor>() == QColor(0x2A, 0x2D, 0x35));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml banner stack: dismissing the lower banner, then the upper, gives back all the room "
          "(offscreen)",
          "[contour][gui][qml][mainwindow][hosting][newdefaults]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::MockNewDefaultsNotice notice { contour::config::NewDefaultsOffer::ExistingConfig };
    notice.setOffered(false);
    engine.rootContext()->setContextProperty(QStringLiteral("newDefaults"), &notice);
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto const* stack = root->findChild<QQuickItem*>(QStringLiteral("bannerStack"));
    auto const* hosting = root->findChild<QQuickItem*>(QStringLiteral("hostingNoticeBar"));
    auto const* newDefaults = root->findChild<QQuickItem*>(QStringLiteral("newDefaultsBanner"));
    auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
    REQUIRE(stack != nullptr);
    REQUIRE(hosting != nullptr);
    REQUIRE(newDefaults != nullptr);
    REQUIRE(content != nullptr);
    CHECK(stack->height() == 0);
    auto const contentY = content->y();
    auto const contentHeight = content->height();
    auto const chromeHeight = controller.chromeHeight();

    controller.setHostingNotice(hostingNotice());
    notice.setOffered(true);
    QCoreApplication::processEvents();
    REQUIRE(hosting->isVisible());
    REQUIRE(newDefaults->isVisible());
    CHECK(stack->height() == hosting->height() + newDefaults->height());
    CHECK(content->y() == contentY + stack->height());
    // The window's geometry counts the banners as chrome, as it does the tab strip.
    CHECK(controller.chromeHeight() == chromeHeight + static_cast<int>(stack->height()));

    // Keep Mine first: the notice withdraws its offer, as the real one does once remembered.
    auto* keepMine = newDefaults->findChild<QObject*>(QStringLiteral("bannerDismiss"));
    REQUIRE(keepMine != nullptr);
    REQUIRE(QMetaObject::invokeMethod(keepMine, "clicked"));
    notice.setOffered(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(newDefaults->isVisible());
    CHECK(stack->height() == hosting->height());

    // Then the hosting banner: the stack collapses entirely, rather than keeping the height of the
    // banner that stood above the one already gone.
    auto* dismiss = hosting->findChild<QObject*>(QStringLiteral("bannerDismiss"));
    REQUIRE(dismiss != nullptr);
    REQUIRE(QMetaObject::invokeMethod(dismiss, "clicked"));
    QCoreApplication::processEvents();
    CHECK_FALSE(hosting->isVisible());
    CHECK(stack->height() == 0);
    CHECK(content->y() == contentY);
    CHECK(content->height() == contentHeight);
    CHECK(controller.chromeHeight() == chromeHeight);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml new-defaults banner: offered, with its copy, answering the notice (offscreen)",
          "[contour][gui][qml][mainwindow][newdefaults]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::MockNewDefaultsNotice notice { contour::config::NewDefaultsOffer::ExistingConfig };
    engine.rootContext()->setContextProperty(QStringLiteral("newDefaults"), &notice);
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto const* bar = root->findChild<QQuickItem*>(QStringLiteral("newDefaultsBanner"));
    REQUIRE(bar != nullptr);
    CHECK(bar->isVisible());
    auto const* text = bar->findChild<QObject*>(QStringLiteral("bannerText"));
    auto* action = bar->findChild<QObject*>(QStringLiteral("bannerAction"));
    auto* dismiss = bar->findChild<QObject*>(QStringLiteral("bannerDismiss"));
    REQUIRE(text != nullptr);
    REQUIRE(action != nullptr);
    REQUIRE(dismiss != nullptr);
    CHECK(text->property("text").toString() == QStringLiteral("Contour has new defaults"));
    CHECK(action->property("text").toString() == QStringLiteral("Use New"));
    CHECK(dismiss->property("text").toString() == QStringLiteral("Keep Mine"));

    REQUIRE(QMetaObject::invokeMethod(action, "clicked"));
    REQUIRE(QMetaObject::invokeMethod(dismiss, "clicked"));
    QCoreApplication::processEvents();
    CHECK(notice.calls == QStringList { QStringLiteral("useNew"), QStringLiteral("keepMine") });

    // The notice withdrawing its offer takes the banner away.
    notice.setOffered(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(bar->isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml new-defaults banner: hidden without a notice (offscreen)",
          "[contour][gui][qml][mainwindow][newdefaults]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    auto const* bar = root->findChild<QQuickItem*>(QStringLiteral("newDefaultsBanner"));
    REQUIRE(bar != nullptr);
    CHECK_FALSE(bar->isVisible());
    for (auto const& w: warnings.messages())
        UNSCOPED_INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml new-defaults banner: the Terminal row's banner is the scheme's Bar (offscreen)",
          "[contour][gui][qml][mainwindow][newdefaults]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::MockNewDefaultsNotice notice { contour::config::NewDefaultsOffer::SettingsOverride };
    engine.rootContext()->setContextProperty(QStringLiteral("newDefaults"), &notice);
    contour::test::QmlMessageCapture warnings;

    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Bar)] =
        vtbackend::RGBColor { 0x2A2D35u };
    auto root = loadMainWindow(engine, controller, warnings, setup);

    auto const* bar = root->findChild<QQuickItem*>(QStringLiteral("newDefaultsBanner"));
    REQUIRE(bar != nullptr);
    REQUIRE(bar->isVisible());
    auto const* background = bar->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("color").value<QColor>() == QColor(0x2A, 0x2D, 0x35));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml loads under every chrome row (offscreen)", "[contour][gui][qml][mainwindow][chromerow]")
{
    for (auto const& [row, host]: contour::test::snapshot::SnapshotRowHosts)
    {
        auto const name = contour::config::chromeStyleName(row);
        INFO("row: " << name);
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto const windowControls =
            contour::config::resolveWindowControlStyle(contour::config::WindowControlStyle::Auto, host);
        auto const chrome = contour::test::ChromeSetup { .row = row,
                                                         .font = QGuiApplication::font(),
                                                         .windowControls = windowControls };
        auto root = loadMainWindow(engine, controller, warnings, chrome);
        auto const* style = engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
        REQUIRE(style != nullptr);
        CHECK(style->property("chromeStyle").toString() == QString::fromLatin1(name));
        // The row's host decides its window controls: traffic lights on the leading edge on macOS, buttons
        // on the trailing edge on Windows, the round Linux buttons in the fallback layout (trailing) on any
        // other desktop -- drawn as cells on the Terminal row.
        auto const* controls = engine.rootContext()->contextProperty("windowControls").value<QObject*>();
        REQUIRE(controls != nullptr);
        auto const isMac = host == contour::config::HostPlatform::MacOS;
        auto const isWindows = host == contour::config::HostPlatform::Windows;
        auto const dots = isMac || row == contour::config::ChromeStyle::Terminal;
        auto const shape = isWindows ? "button" : "round";
        CHECK(controls->property("presentation").toString() == (dots ? "trafficLight" : shape));
        auto const leading = controls->property("leadingButtons").toList();
        auto const trailing = controls->property("trailingButtons").toList();
        CHECK(leading.size() == (isMac ? 3 : 0));
        CHECK(trailing.size() == (isMac ? 0 : 3));
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("titleBar")) != nullptr);
    }
}

namespace
{
using contour::config::ChromeStyle;
using contour::test::MainWindowScene;
using contour::test::snapshot::TabDecor;

/// The setup a chrome row is loaded under, with the window controls its host shows.
/// @param row    The chrome row.
/// @param scheme The terminal scheme the Terminal row paints with; the default palette's otherwise.
[[nodiscard]] contour::test::ChromeSetup rowSetup(
    ChromeStyle row,
    contour::config::ChromeSchemeColors scheme =
        contour::config::chromeSchemeColorsOf(vtbackend::ColorPalette {}))
{
    return contour::test::ChromeSetup {
        .row = row,
        .font = QGuiApplication::font(),
        .windowControls = contour::config::resolveWindowControlStyle(
            contour::config::WindowControlStyle::Auto, contour::test::snapshot::rowHost(row).host),
        .accessibility = {},
        .appearance = {},
        .scheme = scheme,
    };
}

/// The chrome scheme of the bundled colour scheme @p name.
[[nodiscard]] contour::config::ChromeSchemeColors bundledScheme(std::string_view name)
{
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(name, palette));
    return contour::config::chromeSchemeColorsOf(palette);
}

/// Shows Main.qml's window at @p width and waits until it is exposed, so pointer and hover cases work.
void showWindow(QObject& root, int width = 1024)
{
    auto* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    window->resize(width, 640);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window));
    // The pointer rests where an earlier case left it, which may be over a tab: park it in the content,
    // so no tab starts out hovered.
    // Two moves, because a move to where the pointer already is sends nothing.
    QTest::mouseMove(window, QPoint(1, window->height() / 2));
    QTest::mouseMove(window, QPoint(window->width() / 2, window->height() / 2));
    QCoreApplication::processEvents();
}

/// The chrome style's sub-object @p name (`size`, `radii`, `colors`, ...).
[[nodiscard]] QObject& styleGroup(QQmlEngine& engine, char const* name)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto* group = style->property(name).value<QObject*>();
    REQUIRE(group != nullptr);
    return *group;
}

/// The chrome style itself.
[[nodiscard]] QObject& chromeStyleOf(QQmlEngine& engine)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    return *style;
}

/// The realized tab delegates of the strip, by index; an entry is null for a delegate out of view.
[[nodiscard]] std::vector<QQuickItem*> tabDelegates(QObject& root)
{
    auto* list = root.findChild<QQuickItem*>(QStringLiteral("tabList"));
    REQUIRE(list != nullptr);
    QMetaObject::invokeMethod(list, "forceLayout");
    auto items = std::vector<QQuickItem*> {};
    for (auto const index: std::views::iota(0, list->property("count").toInt()))
    {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
        items.push_back(item);
    }
    return items;
}

/// The child of @p item named @p name, failing the test when there is none.
[[nodiscard]] QQuickItem& part(QQuickItem const& item, QString const& name)
{
    auto* child = item.findChild<QQuickItem*>(name);
    INFO("part: " << name.toStdString());
    REQUIRE(child != nullptr);
    return *child;
}

/// @return @p color as the RGB the contrast helpers take.
[[nodiscard]] vtbackend::RGBColor rgbOf(QColor const& color)
{
    return vtbackend::RGBColor { static_cast<uint8_t>(color.red()),
                                 static_cast<uint8_t>(color.green()),
                                 static_cast<uint8_t>(color.blue()) };
}

/// @return @p top composited over the opaque @p ground: what a translucent label paints.
[[nodiscard]] QColor over(QColor const& top, QColor const& ground)
{
    auto const alpha = top.alphaF();
    auto const mix = [alpha](float a, float b) {
        return (a * alpha) + (b * (1.0F - alpha));
    };
    return QColor::fromRgbF(
        mix(top.redF(), ground.redF()), mix(top.greenF(), ground.greenF()), mix(top.blueF(), ground.blueF()));
}

/// @return The WCAG contrast of @p a against @p b, opaque colours both.
[[nodiscard]] double contrastOf(QColor const& a, QColor const& b)
{
    return vtbackend::contrastRatio(rgbOf(a), rgbOf(b));
}

/// Evaluates @p expression in @p item's own QML context, failing the test on an error.
[[nodiscard]] QVariant evaluateIn(QQuickItem& item, QString const& expression)
{
    auto evaluation = QQmlExpression(qmlContext(&item), &item, expression);
    auto value = evaluation.evaluate();
    INFO("expression: " << expression.toStdString()
                        << " error: " << evaluation.error().toString().toStdString());
    REQUIRE_FALSE(evaluation.hasError());
    return value;
}
} // namespace

TEST_CASE("the macOS row draws the well and pills at the canvas tokens (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);

    auto const& size = styleGroup(engine, "size");
    auto const& radii = styleGroup(engine, "radii");

    auto const* well = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    REQUIRE(well != nullptr);
    CHECK(well->isVisible());
    CHECK(well->height() == size.property("tabWell").toReal());
    CHECK(well->property("radius").toReal() == radii.property("tabWell").toReal());

    auto* strip = root->findChild<QQuickItem*>(QStringLiteral("tabStrip"));
    REQUIRE(strip != nullptr);
    auto layout = QVariantMap {};
    REQUIRE(QMetaObject::invokeMethod(&chromeStyleOf(engine),
                                      "tabWellLayout",
                                      Q_RETURN_ARG(QVariantMap, layout),
                                      Q_ARG(qreal, strip->property("availableWidth").toReal()),
                                      Q_ARG(int, 3)));
    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 3);
    for (auto* tab: tabs)
    {
        REQUIRE(tab != nullptr);
        CHECK(tab->height() == size.property("tab").toReal());
        CHECK(part(*tab, QStringLiteral("tabBackground")).property("radius").toReal()
              == radii.property("tab").toReal());
        CHECK(part(*tab, QStringLiteral("tabIcon")).width() == size.property("tabIcon").toReal());
        CHECK(tab->width() == layout.value(QStringLiteral("tabWidth")).toReal());
        CHECK(tab->width() == tabs.front()->width());
    }

    // The close button shows on the active tab, and on an inactive one only while it is hovered.
    CHECK(part(*tabs[0], QStringLiteral("tabCloseButton")).isVisible());
    auto& inactiveClose = part(*tabs[1], QStringLiteral("tabCloseButton"));
    CHECK_FALSE(inactiveClose.isVisible());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto const centre = tabs[1]->mapToScene(QPointF(tabs[1]->width() / 2, tabs[1]->height() / 2));
    QTest::mouseMove(window, centre.toPoint());
    REQUIRE(QTest::qWaitFor([&] { return inactiveClose.isVisible(); }, 1000));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Fluent row takes the win board's geometry (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3, .decor = TabDecor::EveryBadge } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(titleBar != nullptr);
    CHECK(titleBar->height() == 40);

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[0]->implicitHeight() == 34);

    // The mock window asks for no client-frame affordances, so Main.qml loads no window controls here; the
    // width every WindowControls button binds to is the chrome style's.
    CHECK(chromeStyleOf(engine).property("windowControlWidth").toReal() == 46);
    auto const* newTab = root->findChild<QQuickItem*>(QStringLiteral("newTabButton"));
    REQUIRE(newTab != nullptr);
    CHECK(newTab->width() == 36);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a long title elides inside the pill (offscreen)", "[contour][gui][qml][mainwindow][tabwell]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2, .decor = TabDecor::LongTitle } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 2);
    auto& tab = *tabs[0];
    auto const& label = part(tab, QStringLiteral("tabLabel"));
    CHECK(label.property("truncated").toBool());
    CHECK(tab.width() <= chromeStyleOf(engine).property("maxTabWidth").toReal());
    auto const fullTitle = QString(contour::test::DecorLongTitleLength, QChar(u'w'));
    CHECK(evaluateIn(tab, QStringLiteral("Accessible.name")).toString() == fullTitle);

    auto const* dragRegion = root->findChild<QQuickItem*>(QStringLiteral("dragRegion"));
    REQUIRE(dragRegion != nullptr);
    CHECK(dragRegion->width() >= styleGroup(engine, "space").property("dragReserve").toReal());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("twenty tabs go icon-only and keep the active one in view (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    constexpr auto TabCount = 20;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = TabCount, .activeTab = TabCount - 1 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root, 900);

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == static_cast<std::size_t>(TabCount));
    auto realized = 0;
    for (auto* tab: tabs | std::views::filter([](QQuickItem const* item) { return item != nullptr; }))
    {
        ++realized;
        CHECK_FALSE(part(*tab, QStringLiteral("tabLabel")).isVisible());
        CHECK(tab->width() == tabs.back()->width());
    }
    CHECK(realized > 0);

    auto const* list = root->findChild<QQuickItem*>(QStringLiteral("tabList"));
    REQUIRE(list != nullptr);
    auto const* active = tabs.back();
    REQUIRE(active != nullptr);
    auto const contentX = list->property("contentX").toReal();
    CHECK(active->x() >= contentX);
    CHECK(active->x() + active->width() <= contentX + list->width());

    auto const* newTab = root->findChild<QQuickItem*>(QStringLiteral("newTabButton"));
    REQUIRE(newTab != nullptr);
    CHECK(newTab->isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a coloured pill is tinted at 20 % and its icon at 100 % (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    for (auto const appearance: contour::test::snapshot::SnapshotAppearances)
    {
        INFO("appearance: " << contour::test::snapshot::appearanceName(appearance));
        auto const scoped = contour::test::ScopedChromeAppearance { appearance };
        QQmlEngine engine;
        // Row 0 wears the colour; the second tab is the active one, so the coloured pill is at rest.
        MockMainController controller { MainWindowScene {
            .tabCount = 2, .decor = TabDecor::EveryBadge, .activeTab = 1 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        showWindow(*root);
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(window));

        auto const tabs = tabDelegates(*root);
        REQUIRE(tabs.size() == 2);
        auto& tab = *tabs[0];
        // At rest: hover reaches the scene on a later frame than the pointer move that ended it.
        REQUIRE(QTest::qWaitFor([&] { return !tab.property("closeShown").toBool(); }, 1000));
        auto const color = contour::platform::toQColor(contour::test::DecorTabColor);
        auto const expected =
            evaluateIn(
                tab,
                QStringLiteral("Qt.tint(Qt.tint(systemPalette.window, chromeStyle.colors.tabWellFill), "
                               "Qt.rgba(%1, %2, %3, chromeStyle.tint.tab))")
                    .arg(color.redF())
                    .arg(color.greenF())
                    .arg(color.blueF()))
                .value<QColor>();
        auto const fill = part(tab, QStringLiteral("tabBackground")).property("color").value<QColor>();
        INFO("fill " << fill.name(QColor::HexArgb).toStdString() << " expected "
                     << expected.name(QColor::HexArgb).toStdString());
        CHECK(fill == expected);
        CHECK(part(tab, QStringLiteral("tabIcon")).property("color").value<QColor>() == color);

        // The label on the tint keeps 4.5:1, measured against the pixels the pill paints.
        auto const& label = part(tab, QStringLiteral("tabLabel"));
        auto const labelColor = label.property("color").value<QColor>();
        CHECK(labelColor.rgba()
              == styleGroup(engine, "colors").property("tabTextTinted").value<QColor>().rgba());
        auto const image = window->grabWindow();
        REQUIRE_FALSE(image.isNull());
        auto const probe =
            tab.mapToScene(QPointF(tab.width() - (styleGroup(engine, "space").property("s4").toReal() / 2),
                                   tab.height() / 2))
            * window->devicePixelRatio();
        auto const painted = image.pixelColor(probe.toPoint());
        INFO("painted: " << painted.name().toStdString() << " expected: " << fill.name().toStdString());
        CHECK(contrastOf(over(labelColor, painted), painted) >= contour::config::MinimumTextContrast);

        // Hovered, the tint lies over the hover fill instead, and the label still reads on it.
        auto const centre = tab.mapToScene(QPointF(tab.width() / 2, tab.height() / 2));
        QTest::mouseMove(window, centre.toPoint());
        REQUIRE(QTest::qWaitFor([&] { return tab.property("closeShown").toBool(); }, 1000));
        auto const hoveredExpected =
            evaluateIn(tab,
                       QStringLiteral(
                           "Qt.tint(Qt.tint(Qt.tint(systemPalette.window, chromeStyle.colors.tabWellFill), "
                           "chromeStyle.colors.tabHoverFill), Qt.rgba(%1, %2, %3, chromeStyle.tint.tab))")
                           .arg(color.redF())
                           .arg(color.greenF())
                           .arg(color.blueF()))
                .value<QColor>();
        auto const hoveredFill = part(tab, QStringLiteral("tabBackground")).property("color").value<QColor>();
        CHECK(hoveredFill.rgba() == hoveredExpected.rgba());
        CHECK(hoveredFill.rgba() != fill.rgba());
        auto const hoveredLabel = label.property("color").value<QColor>();
        CHECK(hoveredLabel.rgba()
              == styleGroup(engine, "colors").property("tabTextTinted").value<QColor>().rgba());
        QCoreApplication::processEvents();
        auto const hoveredImage = window->grabWindow();
        REQUIRE_FALSE(hoveredImage.isNull());
        auto const hoveredPainted = hoveredImage.pixelColor(probe.toPoint());
        INFO("hovered painted: " << hoveredPainted.name().toStdString());
        CHECK(contrastOf(over(hoveredLabel, hoveredPainted), hoveredPainted)
              >= contour::config::MinimumTextContrast);

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a pill's labels dim one level while its window is in the background (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 2);
    auto const& colors = styleGroup(engine, "colors");
    auto const labelOf = [](QQuickItem const& tab) {
        return part(tab, QStringLiteral("tabLabel")).property("color").value<QColor>().rgba();
    };
    auto const role = [&](char const* name) {
        return colors.property(name).value<QColor>().rgba();
    };

    window->requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(window));
    CHECK(labelOf(*tabs[0]) == role("tabTextActive"));
    CHECK(labelOf(*tabs[1]) == role("tabText"));

    // Another window takes the focus: each label goes one level down.
    QQuickWindow other;
    other.resize(200, 200);
    other.show();
    other.requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(&other));
    REQUIRE(QTest::qWaitFor([&] { return labelOf(*tabs[0]) == role("tabText"); }, 1000));
    CHECK(labelOf(*tabs[1]) == role("tabTextDimmed"));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a drop in a scrolled well lands under the pointer (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    constexpr auto TabCount = 30; // more than fit icon-only in 900 pixels, so the well scrolls
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = TabCount, .activeTab = TabCount - 1 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root, 900);

    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* list = root->findChild<QQuickItem*>(QStringLiteral("tabList"));
    REQUIRE(window != nullptr);
    REQUIRE(list != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return list->property("contentX").toReal() > 0; }, 1000)); // scrolled
    // The strip reports where its tabs stand only for a drag over it: none came yet.
    CHECK(controller.tabSpans.empty());

    auto const payload = controller.tabDragPayload(0);
    auto mime = QMimeData {};
    mime.setData(QString::fromLatin1(contour::window::NodeDragMime), payload.toUtf8());
    // Sends the window the events a platform's drag manager would for a node dragged to @p scene.
    auto const dragTo = [&](QPoint scene) {
        auto const send = [&](auto&& event) {
            event.ignore();
            QCoreApplication::sendEvent(window, &event);
            return event.isAccepted();
        };
        auto const actions = Qt::MoveAction;
        std::ignore = send(QDragEnterEvent { scene, actions, &mime, Qt::LeftButton, Qt::NoModifier });
        return send(QDragMoveEvent { scene, actions, &mime, Qt::LeftButton, Qt::NoModifier });
    };
    auto const dropAt = [&](QPoint scene) {
        auto drop = QDropEvent { QPointF { scene }, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier };
        drop.ignore();
        QCoreApplication::sendEvent(window, &drop);
        return drop.isAccepted() && drop.dropAction() == Qt::MoveAction;
    };

    auto const tabs = tabDelegates(*root);
    auto checked = 0;
    for (auto const index: std::views::iota(0, TabCount))
    {
        auto const* tab = tabs[static_cast<std::size_t>(index)];
        if (tab == nullptr)
            continue;
        auto const visibleLeft = tab->mapToItem(list, QPointF(0, 0)).x();
        if (visibleLeft < 0 || visibleLeft + tab->width() > list->width())
            continue; // only tabs fully in the well's view
        ++checked;
        INFO("tab " << index);
        // A pointer just inside the tab's left edge is in the gap before it: the slot for that row.
        auto const gapPoint = tab->mapToScene(QPointF(2, tab->height() / 2)).toPoint();
        controller.gapDrops.clear();
        CHECK(dragTo(gapPoint));
        // The span reported as the drag entered is where the tab stands in the well as scrolled.
        REQUIRE(controller.tabSpans.size() == static_cast<std::size_t>(TabCount));
        auto const& span = controller.tabSpans[static_cast<std::size_t>(index)];
        CHECK(span.left == Catch::Approx(tab->mapToItem(list->parentItem(), QPointF(0, 0)).x()));
        CHECK(span.right - span.left == Catch::Approx(tab->width()));
        CHECK(dropAt(gapPoint));
        REQUIRE(controller.gapDrops.size() == 1);
        CHECK(controller.gapDrops.front() == std::pair { index, payload });
    }
    CHECK(checked > 0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("an icon-only pill renames in the open (offscreen)", "[contour][gui][qml][mainwindow][tabwell]")
{
    constexpr auto TabCount = 20;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = TabCount, .activeTab = TabCount - 1 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root, 900);

    auto const tabs = tabDelegates(*root);
    auto* tab = tabs.back();
    REQUIRE(tab != nullptr);
    REQUIRE(tab->property("iconOnly").toBool());
    // An icon-only pill has no room for a close button, active or not.
    CHECK_FALSE(part(*tab, QStringLiteral("tabCloseButton")).isVisible());

    emit controller.tabTitleEditRequested(TabCount - 1); // what SetTabTitle asks of the active tab
    QQuickItem* field = nullptr;
    REQUIRE(QTest::qWaitFor(
        [&] {
            field = tab->findChild<QQuickItem*>(QStringLiteral("tabRenameField"));
            return field != nullptr;
        },
        1000));
    CHECK(field->isVisible());
    CHECK(field->width() > 0);
    CHECK(field->width() >= tab->width() - (2 * styleGroup(engine, "space").property("s1").toReal()) - 1);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row draws one cell-high tabs with glyph badges (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    using contour::config::SchemeSlot;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = contour::test::DecorTabCount,
                                                      .decor = TabDecor::EveryBadge } };
    contour::test::QmlMessageCapture warnings;
    auto const scheme = bundledScheme("contour-light");
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal, scheme));
    showWindow(*root);

    auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(titleBar != nullptr);
    CHECK(titleBar->height() == chromeStyleOf(engine).property("cellHeight").toReal());

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == static_cast<std::size_t>(contour::test::DecorTabCount));
    for (auto const index: std::views::iota(std::size_t { 0 }, tabs.size() - 1))
        CHECK(part(*tabs[index], QStringLiteral("tabSeparator")).property("text").toString() == "│");
    for (auto* tab: tabs)
        CHECK_FALSE(part(*tab, QStringLiteral("tabIcon")).isVisible());

    auto const& cell = part(*tabs[0], QStringLiteral("tabColorCell"));
    CHECK(cell.isVisible());
    CHECK(cell.property("text").toString() == "▌");
    CHECK(cell.property("color").value<QColor>()
          == contour::platform::toQColor(contour::test::DecorTabColor));

    auto const& colors = styleGroup(engine, "colors");
    auto const activeFill = colors.property("tabActiveFill").value<QColor>();
    auto const& activeLabel = part(*tabs[0], QStringLiteral("tabLabel"));
    CHECK(part(*tabs[0], QStringLiteral("tabBackground")).property("color").value<QColor>() == activeFill);
    CHECK(contrastOf(over(activeLabel.property("color").value<QColor>(), activeFill), activeFill)
          >= contour::config::MinimumTextContrast);

    auto const glyphs = std::array { QStringLiteral("•"), QStringLiteral("◑"), QStringLiteral("!"),
                                     QStringLiteral("✓"), QStringLiteral("✕"), QStringLiteral("♪") };
    static_assert(glyphs.size() == contour::test::DecorBadges.size());
    auto const well = colors.property("tabWellFill").value<QColor>();
    auto const bar = scheme.bySlot[static_cast<size_t>(SchemeSlot::Bar)];
    auto const floor =
        std::min(contour::config::MinimumTextContrast,
                 vtbackend::contrastRatio(scheme.bySlot[static_cast<size_t>(SchemeSlot::BarInk)], bar));
    for (auto const index: std::views::iota(std::size_t { 0 }, glyphs.size()))
    {
        INFO("badge row " << index + 1);
        auto const& glyph = part(*tabs[index + 1], QStringLiteral("tabBadgeGlyph"));
        CHECK(glyph.isVisible());
        CHECK(glyph.property("text").toString() == glyphs[index]);
        // The badge costs its tab the badge's cells, never the title.
        auto const& badgedLabel = part(*tabs[index + 1], QStringLiteral("tabLabel"));
        CHECK_FALSE(badgedLabel.property("truncated").toBool());
        CHECK(contrastOf(over(glyph.property("color").value<QColor>(), well), well) >= floor);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row sets the active tab apart where its fill is the bar's (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    // one-dark's selection falls back to the bar itself, so the active tab's fill is the well's: the
    // label's weight is what still tells it from the others.
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(
        engine, controller, warnings, rowSetup(ChromeStyle::Terminal, bundledScheme("one-dark")));
    showWindow(*root);

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 2);
    auto const fillOf = [](QQuickItem const& tab) {
        return part(tab, QStringLiteral("tabBackground")).property("color").value<QColor>();
    };
    auto const boldOf = [](QQuickItem const& tab) {
        return part(tab, QStringLiteral("tabLabel")).property("font").value<QFont>().bold();
    };
    CHECK(fillOf(*tabs[0]) == fillOf(*tabs[1])); // the premise: the fill alone says nothing here
    CHECK(boldOf(*tabs[0]));
    CHECK_FALSE(boldOf(*tabs[1]));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml remote chrome: a window attached to a host names it in its chip (offscreen)",
          "[contour][gui][qml][mainwindow][window][hosts]")
{
    SECTION("a remote window")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.attachedHost = QStringLiteral("home");
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        // No session has a title yet, so the title is the host alone (remoteWindowTitle).
        CHECK(root->property("title").toString() == QStringLiteral("home"));
        // The chip replaces the strip's old host badge.
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("hostBadge")) == nullptr);
        auto* chip = root->findChild<QQuickItem*>(QStringLiteral("hostChip"));
        REQUIRE(chip != nullptr);
        CHECK(chip->isVisible());
        auto const* label = chip->findChild<QObject*>(QStringLiteral("hostChipLabel"));
        auto const* status = chip->findChild<QObject*>(QStringLiteral("hostChipStatus"));
        auto const* dot = chip->findChild<QObject*>(QStringLiteral("hostChipDot"));
        REQUIRE(label != nullptr);
        REQUIRE(status != nullptr);
        REQUIRE(dot != nullptr);
        CHECK(label->property("text").toString() == QStringLiteral("home"));

        auto const* style = engine.rootContext()
                                ->contextProperty(QStringLiteral("chromeStyle"))
                                .value<contour::window::UiStyleProvider*>();
        REQUIRE(style != nullptr);
        for (auto const index: std::views::iota(std::size_t { 0 }, contour::window::HostChipStateCount))
        {
            auto const state = static_cast<contour::window::HostChipState>(index);
            auto const& look = contour::window::hostChipLook(state);
            INFO("state: " << look.accessibleState);
            controller.setHostChipState(static_cast<int>(state));
            QCoreApplication::processEvents();
            CHECK(status->property("text").toString() == QString::fromUtf8(look.status));
            CHECK(dot->property("color").value<QColor>() == style->statusInk(static_cast<int>(look.dot)));
            CHECK(evaluateIn(*chip, QStringLiteral("Accessible.name")).toString()
                  == QStringLiteral("Attached to home, %1").arg(QString::fromUtf8(look.accessibleState)));
        }
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("a local window")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        CHECK(root->property("title").toString().isEmpty());
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("hostBadge")) == nullptr);
        auto const* chip = root->findChild<QQuickItem*>(QStringLiteral("hostChip"));
        REQUIRE(chip != nullptr);
        CHECK_FALSE(chip->isVisible());
        CHECK(chip->width() == 0);
    }
}

namespace
{
using contour::config::TabBarVisibility;

/// Whether @p item lies beneath @p ancestor in the visual tree.
[[nodiscard]] bool isInside(QQuickItem const* item, QQuickItem const* ancestor)
{
    for (auto const* parent = item; parent != nullptr; parent = parent->parentItem())
        if (parent == ancestor)
            return true;
    return false;
}

/// The item named @p name, failing the test when there is none.
[[nodiscard]] QQuickItem& named(QObject& root, QString const& name)
{
    auto* item = root.findChild<QQuickItem*>(name);
    INFO("item: " << name.toStdString());
    REQUIRE(item != nullptr);
    return *item;
}

/// Whether an item named @p name lies beneath @p root in the visual tree.
[[nodiscard]] bool isInVisualTree(QQuickItem const& root, QString const& name)
{
    return std::ranges::any_of(root.childItems(), [&](QQuickItem const* child) {
        return child->objectName() == name || isInVisualTree(*child, name);
    });
}

/// The centre of @p item, in its window's coordinates.
[[nodiscard]] QPoint centreOf(QQuickItem const& item)
{
    return item.mapToScene(QPointF(item.width() / 2, item.height() / 2)).toPoint();
}
} // namespace

TEST_CASE("the band's title is the sticky command header while it lives there (offscreen)",
          "[contour][gui][qml][mainwindow][band][blocks]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    auto& title = named(*root, QStringLiteral("bandTitle"));
    auto& jump = named(*root, QStringLiteral("bandJump"));
    REQUIRE(title.isVisible());
    CHECK(title.property("text").toString() == QStringLiteral("Tab 1"));
    CHECK_FALSE(jump.isVisible());

    auto const header = [](contour::window::StickyHeaderHost host) {
        return QVariantMap { { QStringLiteral("glyph"), QStringLiteral("✓") },
                             { QStringLiteral("command"), QStringLiteral("cmake --build") },
                             { QStringLiteral("detail"), QStringLiteral("2m 14s") },
                             { QStringLiteral("outcome"), 0 },
                             { QStringLiteral("statusRole"), 0 },
                             { QStringLiteral("host"), static_cast<int>(host) },
                             { QStringLiteral("jumpLabel"), QStringLiteral("Jump to Command ↑") } };
    };

    // A header that lives in a pane header is that pane's: the band keeps the tab's title.
    controller.setActiveStickyHeader(header(contour::window::StickyHeaderHost::PaneHeader));
    QCoreApplication::processEvents();
    CHECK(title.property("text").toString() == QStringLiteral("Tab 1"));
    CHECK_FALSE(jump.isVisible());

    controller.setActiveStickyHeader(header(contour::window::StickyHeaderHost::TitleBand));
    QCoreApplication::processEvents();
    CHECK(title.property("text").toString() == QStringLiteral("✓ cmake --build · 2m 14s"));
    // Announced by what it says and how it ended, not by its duration: that ticks while the block runs.
    CHECK(evaluateIn(title, QStringLiteral("Accessible.name")).toString()
          == QStringLiteral("✓ cmake --build"));
    auto ticking = header(contour::window::StickyHeaderHost::TitleBand);
    ticking[QStringLiteral("detail")] = QStringLiteral("2m 15s");
    controller.setActiveStickyHeader(ticking);
    QCoreApplication::processEvents();
    CHECK(title.property("text").toString() == QStringLiteral("✓ cmake --build · 2m 15s"));
    CHECK(evaluateIn(title, QStringLiteral("Accessible.name")).toString()
          == QStringLiteral("✓ cmake --build"));
    REQUIRE(jump.isVisible());
    CHECK(jump.property("text").toString() == QStringLiteral("Jump to Command ↑"));

    showWindow(*root);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(jump));
    QCoreApplication::processEvents();
    CHECK(controller.stickyJumps == 1);

    controller.setActiveStickyHeader({});
    QCoreApplication::processEvents();
    CHECK(title.property("text").toString() == QStringLiteral("Tab 1"));
    CHECK_FALSE(jump.isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// The item named @p name in @p root's window, searched through the visual tree (a Repeater's delegates
/// have no QObject parent); nullptr when there is none.
[[nodiscard]] QQuickItem* visualItem(QQuickItem const& root, QString const& name)
{
    for (auto* child: root.childItems())
    {
        if (child->objectName() == name)
            return child;
        if (auto* found = visualItem(*child, name); found != nullptr)
            return found;
    }
    return nullptr;
}

/// The item named @p name in the window @p root, failing the test when there is none.
[[nodiscard]] QQuickItem& shown(QObject& root, QString const& name)
{
    auto* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    auto* item = visualItem(*window->contentItem(), name);
    INFO("item: " << name.toStdString());
    REQUIRE(item != nullptr);
    return *item;
}

/// Whether an item named @p name is in the window @p root and visible.
[[nodiscard]] bool isShown(QObject& root, QString const& name)
{
    auto* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    auto const* item = visualItem(*window->contentItem(), name);
    return item != nullptr && item->isVisible() && item->width() > 0;
}

/// The left edge of @p item in scene coordinates.
[[nodiscard]] qreal sceneLeft(QQuickItem const& item)
{
    return item.mapToScene(QPointF(0, 0)).x();
}

/// Whether @p items stand left to right in that order.
[[nodiscard]] bool inOrder(std::initializer_list<QQuickItem const*> items)
{
    auto previous = -std::numeric_limits<qreal>::infinity();
    for (auto const* item: items)
    {
        auto const left = sceneLeft(*item);
        INFO("item: " << item->objectName().toStdString() << " at " << left);
        if (left <= previous)
            return false;
        previous = left;
    }
    return true;
}

/// The ContourLinux row with the Linux window controls placed by @p layout.
[[nodiscard]] contour::test::ChromeSetup headerSetup(contour::config::WindowButtonLayout layout)
{
    auto setup = rowSetup(ChromeStyle::ContourLinux);
    setup.buttonLayout = std::move(layout);
    return setup;
}

using enum contour::config::WindowControlKind;
using contour::config::WindowButtonLayout;
using contour::test::ClientFrameAffordances;
} // namespace

TEST_CASE("every tab of a strip stands at one height and a pill centres in the bar (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // A horizontal ListView puts each delegate it positions back at y 0, so a tab that centred itself in a
    // list taller than it sat at the list's top: the inactive pills of the ContourLinux header bar stood
    // above the active one.
    for (auto const index: std::views::iota(size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        auto const row = static_cast<ChromeStyle>(index);
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto const& titleBar = named(*root, QStringLiteral("titleBar"));
        auto const tabs = tabDelegates(*root);
        REQUIRE(tabs.size() == 3);
        auto const centreY = [&](QQuickItem const& item) {
            return item.mapToItem(&titleBar, QPointF(0, item.height() / 2)).y();
        };
        for (auto const* tab: tabs)
        {
            REQUIRE(tab != nullptr);
            INFO("tab: " << tab->property("tabIndex").toInt());
            CHECK(centreY(*tab) == centreY(*tabs.front()));
            // Within half a pixel: an anchor centres on whole pixels.
            CHECK(std::abs(centreY(part(*tab, QStringLiteral("tabLabel"))) - centreY(*tab)) <= 0.5);
        }
        if (chromeStyleOf(engine).property("layout").value<QObject*>()->property("isPill").toBool())
            CHECK(std::abs(centreY(*tabs.front()) - (titleBar.height() / 2)) <= 0.5);
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a tab's close mark is a symbolic icon on the header bar and a glyph elsewhere (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const index: std::views::iota(size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        auto const row = static_cast<ChromeStyle>(index);
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 2 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto const tabs = tabDelegates(*root);
        REQUIRE(!tabs.empty());
        REQUIRE(tabs.front() != nullptr);
        auto const& close = part(*tabs.front(), QStringLiteral("tabCloseButton"));
        auto const* mark = close.findChild<QQuickItem*>(QStringLiteral("tabCloseMark"));
        REQUIRE(mark != nullptr);
        auto const& layout = *chromeStyleOf(engine).property("layout").value<QObject*>();
        if (layout.property("tabClose").toInt() == layout.property("symbolicIcon").toInt())
        {
            CHECK(mark->property("name").toString() == layout.property("tabCloseIconName").toString());
            CHECK(mark->property("source").toUrl() == QUrl(layout.property("tabCloseIconSource").toString()));
            CHECK(mark->width() > 0);
        }
        else
            CHECK(mark->property("text").toString()
                  == chromeStyleOf(engine).property("closeGlyph").toString());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
    // The header bar's row is the one that draws the icon.
    auto const& linuxRow =
        contour::config::ChromeTokenRows.at(static_cast<size_t>(ChromeStyle::ContourLinux));
    CHECK(linuxRow.layout.tabClose == contour::config::TabCloseMark::SymbolicIcon);
}

TEST_CASE("the header bar is the canvas's (offscreen)", "[contour][gui][qml][mainwindow][headerbar]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3,
                                                      .affordances = ClientFrameAffordances::Drawn } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(
        engine,
        controller,
        warnings,
        headerSetup(WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } }));
    showWindow(*root);

    auto const& size = styleGroup(engine, "size");
    auto const& radii = styleGroup(engine, "radii");
    auto const& space = styleGroup(engine, "space");
    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(titleBar.height() == size.property("titleBar").toReal());

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 3);
    REQUIRE(tabs[0] != nullptr);
    CHECK(tabs[0]->height() == size.property("tab").toReal());
    CHECK(part(*tabs[0], QStringLiteral("tabBackground")).property("radius").toReal()
          == radii.property("tab").toReal());
    CHECK_FALSE(isShown(*root, QStringLiteral("tabWell")));

    auto const& newTab = shown(*root, QStringLiteral("headerNewTab"));
    auto const& separator = shown(*root, QStringLiteral("trailingControlsSeparator"));
    CHECK(inOrder({ &newTab,
                    tabs[0],
                    &shown(*root, QStringLiteral("headerSearch")),
                    &shown(*root, QStringLiteral("headerMainMenu")),
                    &separator,
                    &shown(*root, QStringLiteral("windowControl_minimize")),
                    &shown(*root, QStringLiteral("windowControl_maximize")),
                    &shown(*root, QStringLiteral("windowControl_close")) }));
    CHECK(separator.isVisible());
    CHECK(separator.height() == size.property("controlsSeparator").toReal());
    CHECK_FALSE(isShown(*root, QStringLiteral("leadingControlsSeparator")));
    // The bar's leading margin.
    CHECK(newTab.mapToItem(&titleBar, QPointF(0, 0)).x() == space.property("s4").toReal());
    // 1c's trailing "+" is the header's new-tab item now.
    CHECK_FALSE(isShown(*root, QStringLiteral("newTabButton")));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the header bar's buttons on the left (offscreen)", "[contour][gui][qml][mainwindow][headerbar]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2,
                                                      .affordances = ClientFrameAffordances::Drawn } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(
        engine,
        controller,
        warnings,
        headerSetup(WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} }));
    showWindow(*root);

    auto const& close = shown(*root, QStringLiteral("windowControl_close"));
    auto const& minimize = shown(*root, QStringLiteral("windowControl_minimize"));
    auto const& maximize = shown(*root, QStringLiteral("windowControl_maximize"));
    auto const& separator = shown(*root, QStringLiteral("leadingControlsSeparator"));
    auto const& newTab = shown(*root, QStringLiteral("headerNewTab"));
    CHECK(inOrder({ &close, &minimize, &maximize, &separator, &newTab }));
    CHECK(separator.isVisible());
    CHECK_FALSE(isShown(*root, QStringLiteral("trailingControlsSeparator")));

    // Mirrored, the same order runs right to left.
    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    std::ignore = evaluateIn(
        titleBar, QStringLiteral("LayoutMirroring.childrenInherit = true, LayoutMirroring.enabled = true"));
    QCoreApplication::processEvents();
    REQUIRE(QTest::qWaitFor([&] { return sceneLeft(close) > sceneLeft(maximize); }, 2000));
    CHECK(inOrder({ &newTab, &separator, &maximize, &minimize, &close }));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the header bar under server-side decorations (offscreen)",
          "[contour][gui][qml][mainwindow][headerbar]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene {
        .tabCount = 2, .frame = contour::test::snapshot::SnapshotFrame::NativeFrame } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(
        engine,
        controller,
        warnings,
        headerSetup(WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } }));
    showWindow(*root);

    for (auto const* name: { "windowControl_minimize",
                             "windowControl_maximize",
                             "windowControl_close",
                             "leadingControlsSeparator",
                             "trailingControlsSeparator" })
    {
        INFO("hidden: " << name);
        CHECK_FALSE(isShown(*root, QString::fromLatin1(name)));
    }
    for (auto const* name: { "headerNewTab", "headerSearch", "headerMainMenu" })
    {
        INFO("shown: " << name);
        CHECK(isShown(*root, QString::fromLatin1(name)));
    }
    CHECK(tabDelegates(*root).size() == 2);

    // One tab under Multiple hides the bar (1c's rule).
    QQmlEngine oneEngine;
    MockMainController one { MainWindowScene {
        .tabCount = 1,
        .tabBarVisibility = contour::config::TabBarVisibility::Multiple,
        .frame = contour::test::snapshot::SnapshotFrame::NativeFrame } };
    auto oneRoot =
        loadMainWindow(oneEngine, one, warnings, headerSetup(contour::config::fallbackButtonLayout()));
    CHECK_FALSE(named(*oneRoot, QStringLiteral("titleBar")).isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the header bar with no buttons wanted (offscreen)", "[contour][gui][qml][mainwindow][headerbar]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2,
                                                      .affordances = ClientFrameAffordances::Drawn } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, headerSetup(WindowButtonLayout {}));
    showWindow(*root);

    for (auto const* name: { "windowControl_minimize",
                             "windowControl_maximize",
                             "windowControl_close",
                             "leadingControlsSeparator",
                             "trailingControlsSeparator" })
    {
        INFO("hidden: " << name);
        CHECK_FALSE(isShown(*root, QString::fromLatin1(name)));
    }
    CHECK(tabDelegates(*root).size() == 2);
    CHECK(isShown(*root, QStringLiteral("headerNewTab")));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// The bundled icon files the tables name: every window-control style's icons and restore icon, and each
/// header-bar item's, as resource paths.
[[nodiscard]] QStringList bundledIconsOfTheTables()
{
    auto icons = QStringList {};
    auto const add = [&icons](std::string_view iconName) {
        auto const path = QStringLiteral(":/contour/icons/%1.svg").arg(QLatin1StringView(iconName));
        if (!iconName.empty() && !icons.contains(path))
            icons.append(path);
    };
    for (auto const& tokens: contour::config::detail::WindowControlTokenTable)
    {
        for (auto const iconName: tokens.iconNames)
            add(iconName);
        add(tokens.restoreIconName);
    }
    for (auto const& row: contour::window::HeaderBarItems)
        add(row.iconName);
    // The secure-input lock names its icon in SecureInputLock.qml, not in a table.
    add("lock-symbolic");
    return icons;
}

/// The edge of the square every freedesktop symbolic icon is drawn on, the bundled ones' viewBox included.
constexpr auto SymbolicIconDesignSize = 16;

/// @p item alone, as the scene graph draws it, over a transparent background.
[[nodiscard]] QImage grabbedImage(QQuickItem& item)
{
    auto const result = item.grabToImage();
    REQUIRE(result != nullptr);
    QSignalSpy ready(result.get(), &QQuickItemGrabResult::ready);
    REQUIRE((ready.count() > 0 || ready.wait(2000)));
    return result->image();
}

/// The opaque pixels of @p image (alpha at least half): the icon's own strokes, not their antialiased rim.
[[nodiscard]] std::vector<QColor> opaquePixelsOf(QImage const& image)
{
    auto pixels = std::vector<QColor> {};
    for (auto const y: std::views::iota(0, image.height()))
        for (auto const x: std::views::iota(0, image.width()))
            if (auto const pixel = image.pixelColor(x, y); pixel.alpha() >= 128)
                pixels.push_back(pixel);
    return pixels;
}

/// Whether @p painted is @p ink's hue, to within the rounding of a premultiplied surface.
[[nodiscard]] bool isInk(QColor const& painted, QColor const& ink)
{
    constexpr auto Tolerance = 2;
    return std::abs(painted.red() - ink.red()) <= Tolerance
           && std::abs(painted.green() - ink.green()) <= Tolerance
           && std::abs(painted.blue() - ink.blue()) <= Tolerance;
}
} // namespace

TEST_CASE("the bundled icons load through Qt SVG at their drawn size", "[contour][gui][headerbar]")
{
    // Every icon a table names is a file in resources.qrc, and the resource directory holds no other: an
    // icon renamed in one place but not the other fails here, as does a build without the Qt SVG plugin.
    auto const icons = bundledIconsOfTheTables();
    auto shipped = QDir(QStringLiteral(":/contour/icons")).entryList(QDir::Files);
    for (auto& name: shipped)
        name.prepend(QStringLiteral(":/contour/icons/"));
    auto expected = icons;
    expected.sort();
    shipped.sort();
    CHECK(shipped == expected);

    auto const size =
        contour::config::ChromeTokenRows.at(static_cast<size_t>(ChromeStyle::ContourLinux)).size;
    for (auto const& path: icons)
    {
        INFO("icon: " << path.toStdString());
        auto reader = QImageReader(path);
        CHECK(reader.format() == QByteArrayLiteral("svg"));
        // The design size the symbolic icons are drawn on.
        CHECK(reader.size() == QSize(SymbolicIconDesignSize, SymbolicIconDesignSize));
        for (auto const extent: { size.headerIcon, size.windowButtonGlyph })
        {
            auto scaled = QImageReader(path);
            scaled.setScaledSize(QSize(extent, extent));
            auto const image = scaled.read();
            INFO("extent " << extent << ": " << scaled.errorString().toStdString());
            REQUIRE_FALSE(image.isNull());
            CHECK(image.size() == QSize(extent, extent));
            CHECK_FALSE(opaquePixelsOf(image).empty());
        }
    }
}

TEST_CASE("the header bar's icons are drawn in the window buttons' ink on every theme (offscreen)",
          "[contour][gui][qml][mainwindow][headerbar]")
{
    // The icon files are drawn in one dark grey; what shows is the WindowButtonGlyph role, on the light and
    // the dark chrome, with and without a contrast theme -- the role the glyph contrast test holds to 3:1.
    using contour::platform::ContrastTheme;
    using vtbackend::ColorPreference;
    for (auto const appearance: { ColorPreference::Light, ColorPreference::Dark })
        for (auto const contrast: { ContrastTheme::None, ContrastTheme::Active })
        {
            INFO("appearance " << static_cast<int>(appearance) << ", contrast "
                               << static_cast<int>(contrast));
            auto const scoped = contour::test::ScopedChromeAppearance(appearance);
            QQmlEngine engine;
            MockMainController controller { MainWindowScene {
                .tabCount = 2, .affordances = ClientFrameAffordances::Drawn } };
            contour::test::QmlMessageCapture warnings;
            auto setup =
                headerSetup(WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } });
            setup.appearance.contrastTheme = contrast;
            auto root = loadMainWindow(engine, controller, warnings, setup);
            showWindow(*root);
            auto* window = qobject_cast<QQuickWindow*>(root.get());
            window->requestActivate();
            REQUIRE(QTest::qWaitForWindowActive(window));

            auto const ink = styleGroup(engine, "colors").property("windowButtonGlyph").value<QColor>();
            auto const& size = styleGroup(engine, "size");
            auto icons = std::vector<std::pair<QString, qreal>> {};
            for (auto const& row: contour::window::HeaderBarItems)
                icons.emplace_back(QString::fromLatin1(row.objectName) + QStringLiteral("Icon"),
                                   size.property("headerIcon").toReal());
            for (auto const* action: { "minimize", "maximize", "close" })
                icons.emplace_back(QStringLiteral("windowControlIcon_") + QLatin1StringView(action),
                                   size.property("windowButtonGlyph").toReal());

            for (auto const& [name, extent]: icons)
            {
                INFO("icon: " << name.toStdString() << ", ink " << ink.name(QColor::HexArgb).toStdString());
                auto& icon = shown(*root, name);
                CHECK(evaluateIn(icon, QStringLiteral("status === Image.Ready")).toBool());
                CHECK(icon.property("color").value<QColor>() == ink);
                CHECK(icon.property("paintedWidth").toReal() == extent);
                CHECK(icon.property("paintedHeight").toReal() == extent);
                auto const pixels = opaquePixelsOf(grabbedImage(icon));
                CHECK_FALSE(pixels.empty());
                for (auto const& pixel: pixels)
                    if (!isInk(pixel, ink))
                    {
                        FAIL_CHECK("painted " << pixel.name().toStdString());
                        break;
                    }
            }

            CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
        }
}

TEST_CASE("clicking the header bar's items (offscreen)", "[contour][gui][qml][mainwindow][headerbar]")
{
    using contour::window::HeaderBarItem;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2,
                                                      .affordances = ClientFrameAffordances::Drawn } };
    contour::test::QmlMessageCapture warnings;
    auto root =
        loadMainWindow(engine, controller, warnings, headerSetup(contour::config::fallbackButtonLayout()));
    showWindow(*root);
    auto* window = qobject_cast<QQuickWindow*>(root.get());

    QTest::mouseClick(window, Qt::LeftButton, {}, centreOf(shown(*root, QStringLiteral("headerSearch"))));
    REQUIRE(QTest::qWaitFor([&] { return !controller.activatedHeaderBarItems.empty(); }, 1000));
    CHECK(controller.activatedHeaderBarItems == std::vector { static_cast<int>(HeaderBarItem::Search) });

    // The main menu's item pops the main menu below it.
    QTest::mouseClick(window, Qt::LeftButton, {}, centreOf(shown(*root, QStringLiteral("headerMainMenu"))));
    REQUIRE(QTest::qWaitFor([&] { return controller.activatedHeaderBarItems.size() == 2; }, 1000));
    CHECK(controller.activatedHeaderBarItems.back() == static_cast<int>(HeaderBarItem::MainMenu));
    auto* menu = root->findChild<QObject*>(QStringLiteral("mainMenu"));
    REQUIRE(menu != nullptr);
    CHECK(QTest::qWaitFor([&] { return menu->property("visible").toBool(); }, 1000));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the band rows have no header bar items (offscreen)", "[contour][gui][qml][mainwindow][headerbar]")
{
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::Terminal })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3,
                                                          .affordances = ClientFrameAffordances::Drawn } };
        contour::test::QmlMessageCapture warnings;
        auto setup = rowSetup(row);
        setup.buttonLayout = WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} };
        auto root = loadMainWindow(engine, controller, warnings, setup);
        showWindow(*root);
        CHECK_FALSE(isShown(*root, QStringLiteral("headerSearch")));
        CHECK_FALSE(isShown(*root, QStringLiteral("headerMainMenu")));
        CHECK_FALSE(isShown(*root, QStringLiteral("headerNewTab")));
        if (row == ChromeStyle::Terminal)
        {
            // The desktop's left-hand layout, as three dot cells before the tabs.
            auto const tabs = tabDelegates(*root);
            REQUIRE(!tabs.empty());
            REQUIRE(tabs[0] != nullptr);
            for (auto const* action: { "close", "minimize", "maximize" })
            {
                INFO("cell: " << action);
                auto const& cell =
                    shown(*root, QStringLiteral("windowControlCell_") + QLatin1StringView(action));
                CHECK(sceneLeft(cell) < sceneLeft(*tabs[0]));
            }
        }
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("with one tab under Multiple, the macOS band shows the centred title (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));

    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(titleBar.isVisible());
    CHECK(titleBar.height() == styleGroup(engine, "size").property("titleBar").toReal());
    CHECK(titleBar.height() == 38);
    auto& title = named(*root, QStringLiteral("bandTitle"));
    CHECK(title.isVisible());
    // The active row's title role: what its tab would be labelled.
    CHECK(title.property("text").toString() == QStringLiteral("Tab 1"));
    auto const* well = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    CHECK((well == nullptr || !well->isVisible()));
    CHECK(controller.chromeHeight() == 38);

    // A second tab: the gate shows the tabs, and the title gives way to the well.
    controller.setTabBarShouldShow(true);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto const* shownWell = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    REQUIRE(shownWell != nullptr);
    CHECK(shownWell->isVisible());
    CHECK_FALSE(title.isVisible());
    CHECK(controller.chromeHeight() == 38);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("with one tab under Multiple, Linux shows no bar, as today (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const row: { ChromeStyle::ContourLinux })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 1,
                                                          .tabBarVisibility = TabBarVisibility::Multiple } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        CHECK(controller.chromeHeight() == 0);
        CHECK_FALSE(named(*root, QStringLiteral("titleBar")).isVisible());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the drift chip sits in the bar, not over the cells (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::ContourLinux })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene {
            .tabCount = 3, .drift = contour::test::snapshot::SnapshotDrift::Shown } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto& chip = named(*root, QStringLiteral("daemonDriftBadge"));
        CHECK(chip.isVisible());
        CHECK(isInside(&chip, &named(*root, QStringLiteral("titleBar"))));
        CHECK_FALSE(isInside(&chip, &named(*root, QStringLiteral("content"))));
        CHECK(chip.height() == styleGroup(engine, "size").property("toolbarChip").toReal());
        // In the band's trailing area: past the drag region.
        auto const& drag = named(*root, QStringLiteral("dragRegion"));
        CHECK(chip.mapToScene(QPointF(0, 0)).x() >= drag.mapToScene(QPointF(drag.width(), 0)).x());

        auto* dialog = chip.findChild<QObject*>(QStringLiteral("daemonDriftConfirm"));
        REQUIRE(dialog != nullptr);
        CHECK(QString::fromLatin1(dialog->metaObject()->className()).startsWith(QStringLiteral("KitDialog")));
        QTest::mouseClick(qobject_cast<QQuickWindow*>(root.get()), Qt::LeftButton, {}, centreOf(chip));
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
        CHECK(dialog->property("heading").toString() == controller.daemonDrift().actionLabel());
        REQUIRE(QMetaObject::invokeMethod(dialog, "accept"));
        CHECK(controller.daemonDrift().isRestarting());

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("with no bar shown, the chip stays at the content's corner")
    {
        QQmlEngine engine;
        MockMainController controller { MainWindowScene {
            .tabCount = 1,
            .tabBarVisibility = TabBarVisibility::Multiple,
            .drift = contour::test::snapshot::SnapshotDrift::Shown } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
        showWindow(*root);

        auto& chip = named(*root, QStringLiteral("daemonDriftBadge"));
        auto& content = named(*root, QStringLiteral("content"));
        CHECK(chip.isVisible());
        CHECK(isInside(&chip, &content));
        CHECK(chip.x() + chip.width() < content.width());
        CHECK(chip.y() + chip.height() < content.height());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("show_title_bar keeps the native frame and a strip (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    constexpr auto Inset = 70.0;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3, .nativeControlsInset = Inset } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);

    auto& leading = named(*root, QStringLiteral("leadingWindowControls"));
    auto& trailing = named(*root, QStringLiteral("trailingWindowControls"));
    // The client band clears the system's traffic lights.
    CHECK(leading.width() == Inset);

    controller.setTitleBarVisible(true);
    QCoreApplication::processEvents();
    REQUIRE(QTest::qWaitFor([&] { return leading.width() == 0; }, 1000));
    CHECK(named(*root, QStringLiteral("titleBar")).isVisible());
    CHECK(named(*root, QStringLiteral("tabWell")).isVisible());
    CHECK(tabDelegates(*root).size() == 3);
    CHECK_FALSE(named(*root, QStringLiteral("bandTitle")).isVisible());
    // The system frame draws the controls: none of ours.
    CHECK_FALSE(leading.property("active").toBool());
    CHECK_FALSE(trailing.property("active").toBool());
    CHECK(trailing.width() == 0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the ContourLinux band rounds the window's top corners only while windowed under CSD (offscreen)",
          "[contour][gui][qml][mainwindow][headerbar][corners]")
{
    using contour::platform::WindowPresentation;
    auto const rowRadius =
        contour::config::ChromeTokenRows.at(static_cast<std::size_t>(ChromeStyle::ContourLinux)).radii.window;
    REQUIRE(rowRadius > 0);
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    controller.setWindowRadius(rowRadius);
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
    showWindow(*root);

    auto const& background = named(*root, QStringLiteral("bandBackground"));
    auto const radii = [&] {
        return std::pair { background.property("topLeftRadius").toReal(),
                           background.property("topRightRadius").toReal() };
    };
    // Windowed, client-side: the canvas's 12 on both top corners, none on the bottom ones.
    CHECK(radii() == std::pair { static_cast<qreal>(rowRadius), static_cast<qreal>(rowRadius) });
    CHECK(background.property("bottomLeftRadius").toReal() == 0);
    CHECK(background.property("bottomRightRadius").toReal() == 0);

    // Maximised: square.
    controller.setPresentation(WindowPresentation::Maximized);
    QCoreApplication::processEvents();
    CHECK(radii() == std::pair { 0.0, 0.0 });

    // Server-side decorations (show_title_bar: true): the window manager's frame, square.
    controller.setPresentation(WindowPresentation::Windowed);
    controller.setTitleBarVisible(true);
    QCoreApplication::processEvents();
    CHECK(radii() == std::pair { 0.0, 0.0 });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
using contour::config::MotionPreference;

/// The macOS row's setup under @p motion.
[[nodiscard]] contour::test::ChromeSetup macOSSetup(MotionPreference motion)
{
    auto setup = rowSetup(ChromeStyle::MacOS);
    setup.accessibility.motion = motion;
    return setup;
}

/// Loads the macOS row with three tabs under a motion preference, takes the window into full screen and waits
/// until the band is concealed: slid above the window, or under Reduce Motion faded out.
struct FullScreenBand
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    std::unique_ptr<QObject> root;
    QQuickWindow* window;
    QQuickItem* titleBar = nullptr;
    MotionPreference motion;

    /// @param motion The motion preference the band moves under.
    /// @param scene  What the window holds.
    explicit FullScreenBand(MotionPreference motion = MotionPreference::Full,
                            MainWindowScene scene = MainWindowScene { .tabCount = 3 }):
        controller { std::move(scene) },
        root { loadMainWindow(engine, controller, warnings, macOSSetup(motion)) },
        window { qobject_cast<QQuickWindow*>(root.get()) },
        motion { motion }
    {
        REQUIRE(window != nullptr);
        window->showFullScreen();
        REQUIRE(QTest::qWaitForWindowExposed(window));
        REQUIRE(QTest::qWaitFor([&] { return window->visibility() == QWindow::FullScreen; }, 2000));
        titleBar = &named(*root, QStringLiteral("titleBar"));
        QTest::mouseMove(window, awayPoint());
        REQUIRE(QTest::qWaitFor([&] { return concealed(); }, 2000));
    }

    ~FullScreenBand()
    {
        if (window != nullptr)
            window->showNormal();
    }

    FullScreenBand(FullScreenBand const&) = delete;
    FullScreenBand& operator=(FullScreenBand const&) = delete;
    FullScreenBand(FullScreenBand&&) = delete;
    FullScreenBand& operator=(FullScreenBand&&) = delete;

    [[nodiscard]] bool revealed() const { return titleBar->property("revealed").toBool(); }

    /// @return Whether the band has finished leaving: slid above the window, or faded out.
    [[nodiscard]] bool concealed() const
    {
        return motion == MotionPreference::Reduced ? titleBar->opacity() == 0
                                                   : titleBar->y() == -titleBar->height();
    }

    /// A point on neither the band nor anything it opens: the middle of the window's left half.
    [[nodiscard]] QPoint awayPoint() const { return { window->width() / 4, window->height() / 2 }; }

    /// Rests the pointer on the reveal edge until the band reveals.
    void revealFromEdge() const
    {
        QTest::mouseMove(window, centreOf(named(*root, QStringLiteral("bandRevealEdge"))));
        REQUIRE(QTest::qWaitFor([&] { return revealed(); }, 2000));
    }

    /// Waits until the band has finished sliding (or fading) in.
    void waitUntilIn() const
    {
        REQUIRE(QTest::qWaitFor([&] { return titleBar->y() == 0 && titleBar->opacity() == 1; }, 2000));
    }

    /// Lets twice the slide's duration pass, long past any hide delay.
    void linger() { QTest::qWait(2 * styleGroup(engine, "motion").property("standard").toInt()); }
};
} // namespace

namespace
{
/// The full-screen reveal on the macOS row under @p motion; @see the two cases below.
void checkFullScreenReveal(MotionPreference motion)
{
    auto band = FullScreenBand { motion };
    auto const reduced = motion == MotionPreference::Reduced;
    auto& titleBar = *band.titleBar;
    auto& content = named(*band.root, QStringLiteral("content"));
    auto const& edge = named(*band.root, QStringLiteral("bandRevealEdge"));
    auto const slideOffset = [&] {
        return titleBar.property("slideOffset").toReal();
    };

    CHECK(edge.isVisible());
    CHECK_FALSE(band.revealed());
    // The band overlays the top rows: it takes nothing from the content, nor from the declared chrome.
    CHECK(content.y() == named(*band.root, QStringLiteral("bannerStack")).y());
    CHECK(content.y() == 0);
    CHECK(band.controller.chromeHeight() == 0);
    // Concealed: slid above the window, or faded out.
    CHECK(slideOffset() == (reduced ? 0 : -titleBar.height()));
    auto const contentY = content.y();
    auto const contentHeight = content.height();

    band.revealFromEdge();
    band.waitUntilIn();
    CHECK(titleBar.isVisible());
    CHECK(slideOffset() == 0);
    CHECK(content.y() == contentY);
    CHECK(content.height() == contentHeight);

    // Down onto the band keeps it; away from both hides it again.
    QTest::mouseMove(band.window, centreOf(titleBar));
    QCoreApplication::processEvents();
    CHECK(band.revealed());
    QTest::mouseMove(band.window, band.awayPoint());
    REQUIRE(QTest::qWaitFor([&] { return !band.revealed(); }, 2000));
    REQUIRE(QTest::qWaitFor([&] { return band.concealed(); }, 2000));
    if (reduced)
    {
        // Faded in place, never slid.
        CHECK(titleBar.y() == 0);
        CHECK(slideOffset() == 0);
    }
    CHECK(content.y() == contentY);
    CHECK(content.height() == contentHeight);
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}
} // namespace

TEST_CASE("full screen reveals the band without resizing the content (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    checkFullScreenReveal(MotionPreference::Full);
}

TEST_CASE("full screen reveals the band by fading it under Reduce Motion (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    checkFullScreenReveal(MotionPreference::Reduced);
}

TEST_CASE("the drift chip reveals with a full-screen band, never over the cells (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    auto band = FullScreenBand { MotionPreference::Full,
                                 MainWindowScene { .tabCount = 3,
                                                   .drift = contour::test::snapshot::SnapshotDrift::Shown } };
    auto& chip = named(*band.root, QStringLiteral("daemonDriftBadge"));
    CHECK(isInside(&chip, band.titleBar));
    CHECK_FALSE(chip.isVisible()); // concealed with the band

    band.revealFromEdge();
    band.waitUntilIn();
    CHECK(chip.isVisible());
    CHECK(isInside(&chip, band.titleBar));
    CHECK_FALSE(isInside(&chip, &named(*band.root, QStringLiteral("content"))));
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// What chromeStyle.bandFill(@p frame, @p mode, @p centre) answers on @p engine.
[[nodiscard]] QColor bandFillOf(QQmlEngine& engine, int frame, int mode, QString const& centre)
{
    auto fill = QColor {};
    REQUIRE(QMetaObject::invokeMethod(&chromeStyleOf(engine),
                                      "bandFill",
                                      Q_RETURN_ARG(QColor, fill),
                                      Q_ARG(int, frame),
                                      Q_ARG(int, mode),
                                      Q_ARG(QString, centre)));
    return fill;
}

/// The colour the band's background paints.
[[nodiscard]] QColor bandBackgroundOf(QObject& root)
{
    return named(root, QStringLiteral("bandBackground")).property("color").value<QColor>();
}

/// The macOS row whose terminal scheme has the chrome palette's tone (preflight F19), so its band paints
/// the tint rather than falling back to the palette.
/// @param transparency The Reduce Transparency setting the chrome is built under.
[[nodiscard]] contour::test::ChromeSetup tintedMacOSSetup(
    contour::config::TransparencyPreference transparency)
{
    auto scheme = contour::config::chromeSchemeColorsOf(vtbackend::ColorPalette {});
    scheme.bySlot[static_cast<std::size_t>(contour::config::SchemeSlot::Background)] =
        vtbackend::RGBColor(0x1C1E24);
    auto setup = rowSetup(ChromeStyle::MacOS, scheme);
    setup.accessibility.transparency = transparency;
    return setup;
}
} // namespace

namespace
{
/// Loads the macOS row under @p transparency and checks its band paints the surface bandFill() names.
void checkBandPaintsSurface(contour::config::TransparencyPreference transparency)
{
    auto const appearance = contour::test::ScopedChromeAppearance { vtbackend::ColorPreference::Dark };
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, tintedMacOSSetup(transparency));
    showWindow(*root);

    auto const& titleBar = named(*root, QStringLiteral("titleBar"));
    auto const centre = titleBar.property("centre").toString();
    auto const fill = bandFillOf(engine, 0, 0, centre);
    CHECK(bandBackgroundOf(*root) == fill);
    // The tint of the scheme, not the palette: the surface table decided it.
    CHECK(fill != QGuiApplication::palette().color(QPalette::Window));
    // Opaque: under Reduce Transparency too (Review Focus 2).
    CHECK(bandBackgroundOf(*root).alphaF() == 1.0);
    // The panes below the band are not the band's to paint: they start under it, as before.
    auto const& content = named(*root, QStringLiteral("content"));
    CHECK(content.y() == titleBar.height());
    CHECK_FALSE(isInside(&content, &titleBar));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
} // namespace

TEST_CASE("the macOS band paints its surface (offscreen)", "[contour][gui][qml][mainwindow][band]")
{
    checkBandPaintsSurface(contour::config::TransparencyPreference::Allowed);
}

TEST_CASE("the band is the tint under Reduce Transparency (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    checkBandPaintsSurface(contour::config::TransparencyPreference::Reduced);
}

TEST_CASE("the band takes the controller's height (offscreen)", "[contour][gui][qml][mainwindow][band]")
{
    constexpr auto Measured = 52.0;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);

    auto const& titleBar = named(*root, QStringLiteral("titleBar"));
    auto const token = chromeStyleOf(engine).property("bandHeight").toReal();
    CHECK(titleBar.height() == token);

    controller.setTitleBandHeight(Measured);
    QCoreApplication::processEvents();
    CHECK(titleBar.height() == Measured);
    CHECK(controller.chromeHeight() == static_cast<int>(Measured));
    CHECK(named(*root, QStringLiteral("content")).y() == Measured);
    // The tabs bar carries no traffic lights: it keeps the token.
    CHECK(named(*root, QStringLiteral("tabsBar")).implicitHeight() == token);

    // Unmeasured again: back to the token.
    controller.setTitleBandHeight(0);
    QCoreApplication::processEvents();
    CHECK(titleBar.height() == token);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// @return The setup @p row is loaded under, in @p density.
[[nodiscard]] contour::test::ChromeSetup densitySetup(ChromeStyle row, contour::config::ChromeDensity density)
{
    auto setup = rowSetup(row);
    setup.density = density;
    return setup;
}

/// The title bar's geometry and the chrome height Main.qml declares.
struct BarGeometry
{
    int chromeHeight = 0;
    bool visible = false;
    QRectF rect {};

    bool operator==(BarGeometry const&) const = default;
};

/// Loads Main.qml on @p row in @p density with @p tabCount tabs under @p visibility.
/// @return The bar's geometry.
[[nodiscard]] BarGeometry barGeometryOf(ChromeStyle row,
                                        contour::config::ChromeDensity density,
                                        int tabCount,
                                        TabBarVisibility visibility = TabBarVisibility::Multiple)
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = tabCount,
                                                      .tabBarVisibility = visibility } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, densitySetup(row, density));
    auto const& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    return BarGeometry { .chromeHeight = controller.chromeHeight(),
                         .visible = titleBar.isVisible(),
                         .rect = QRectF { titleBar.x(), titleBar.y(), titleBar.width(), titleBar.height() } };
}
} // namespace

TEST_CASE("Minimal with one tab is a bare band (offscreen)", "[contour][gui][qml][mainwindow][band][density]")
{
    // A dark chrome over the default (dark) scheme: the tones agree, so the band is the scheme's background.
    auto const appearance = contour::test::ScopedChromeAppearance { vtbackend::ColorPreference::Dark };
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple } };
    contour::test::QmlMessageCapture warnings;
    auto const setup = densitySetup(ChromeStyle::MacOS, contour::config::ChromeDensity::Minimal);
    auto root = loadMainWindow(engine, controller, warnings, setup);

    // The mock measures nothing (titleBandHeight 0), so the bar is the bare-band token: AppKit's bare title
    // bar on macOS (ruling P3: 32 pt; 28 on Fluent and ContourLinux).
    auto const bareBand = styleGroup(engine, "size").property("bareBand").toReal();
    CHECK(bareBand == 32);
    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(titleBar.isVisible());
    CHECK(titleBar.height() == bareBand);
    CHECK(controller.chromeHeight() == static_cast<int>(bareBand));
    CHECK_FALSE(named(*root, QStringLiteral("bandTitle")).isVisible());
    auto const* well = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    CHECK((well == nullptr || !well->isVisible()));
    // Traffic lights only: no "+" either (the Minimal board).
    auto const* bareNewTab = root->findChild<QQuickItem*>(QStringLiteral("newTabButton"));
    CHECK((bareNewTab == nullptr || !bareNewTab->isVisible()));
    // Painted in the terminal background, opaque.
    auto const background = contour::platform::toQColor(
        setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Background)]);
    CHECK(bandBackgroundOf(*root) == background);
    CHECK(bandBackgroundOf(*root).alphaF() == 1.0);

    // A second tab: the Comfortable band returns, with the well.
    controller.setTabBarShouldShow(true);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto const* shownWell = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    REQUIRE(shownWell != nullptr);
    CHECK(shownWell->isVisible());
    auto const* newTab = root->findChild<QQuickItem*>(QStringLiteral("newTabButton"));
    REQUIRE(newTab != nullptr);
    CHECK(newTab->isVisible());
    CHECK(titleBar.height() == 38);
    CHECK(controller.chromeHeight() == 38);
    CHECK(bandBackgroundOf(*root) != background);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Minimal keeps a user's always-shown tab bar (offscreen)",
          "[contour][gui][qml][mainwindow][band][density]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Always } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine,
                               controller,
                               warnings,
                               densitySetup(ChromeStyle::MacOS, contour::config::ChromeDensity::Minimal));

    auto const* well = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
    REQUIRE(well != nullptr);
    CHECK(well->isVisible());
    CHECK(named(*root, QStringLiteral("titleBar")).height() == 38);
    CHECK(controller.chromeHeight() == 38);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Minimal leaves Linux and the Terminal row as Comfortable has them (offscreen)",
          "[contour][gui][qml][mainwindow][band][density]")
{
    // Under Always too: Minimal never overrides a user's always-shown tab bar. Fluent's joined band is
    // Pill's under Minimal: see "one tab still shows the joined bar".
    for (auto const visibility: { TabBarVisibility::Multiple, TabBarVisibility::Always })
        for (auto const row: { ChromeStyle::ContourLinux, ChromeStyle::Terminal })
            for (auto const tabCount: { 1, 3 })
            {
                INFO("row: " << contour::config::chromeStyleName(row) << ", tabs: " << tabCount
                             << ", visibility: " << static_cast<int>(visibility));
                CHECK(
                    barGeometryOf(row, contour::config::ChromeDensity::Minimal, tabCount, visibility)
                    == barGeometryOf(row, contour::config::ChromeDensity::Comfortable, tabCount, visibility));
            }
}

namespace
{
/// The item named @p name in @p root's visual tree, which reaches a Repeater's delegates, as the QObject
/// tree does not; null when there is none.
[[nodiscard]] QQuickItem const* visualChild(QQuickItem const& root, QString const& name)
{
    for (auto const* child: root.childItems())
    {
        if (child->objectName() == name)
            return child;
        if (auto const* found = visualChild(*child, name); found != nullptr)
            return found;
    }
    return nullptr;
}

/// @return Whether @p root's window draws its own close button.
[[nodiscard]] bool showsCloseButton(QObject& root)
{
    auto const* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    auto const* close = visualChild(*window->contentItem(), QStringLiteral("windowControl_close"));
    return close != nullptr && close->isVisible();
}
} // namespace

TEST_CASE("joined tabs sit on the band's floor (offscreen)", "[contour][gui][qml][mainwindow][joinedtabs]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    auto const& size = styleGroup(engine, "size");
    auto const& radii = styleGroup(engine, "radii");
    auto const& space = styleGroup(engine, "space");
    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(named(*root, QStringLiteral("tabShapeJoined")).isVisible());

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 3);
    for (auto* tab: tabs)
    {
        REQUIRE(tab != nullptr);
        CHECK(tab->height() == size.property("tab").toReal());
        CHECK(tab->mapToItem(&titleBar, QPointF(0, tab->height())).y() == titleBar.height());
    }
    CHECK(tabs[0]->mapToItem(&titleBar, QPointF(0, 0)).x() == space.property("tabStripLeading").toReal());

    // Top corners rounded, the bottom meeting the content.
    auto const& background = part(*tabs[0], QStringLiteral("tabBackground"));
    CHECK(background.property("topLeftRadius").toReal() == radii.property("tab").toReal());
    CHECK(background.property("topRightRadius").toReal() == radii.property("tab").toReal());
    CHECK(background.property("bottomLeftRadius").toReal() == 0);
    CHECK(background.property("bottomRightRadius").toReal() == 0);

    // The kit's insets: s6, the icon, s4, the title.
    auto const& icon = part(*tabs[0], QStringLiteral("tabIcon"));
    CHECK(icon.isVisible());
    CHECK(icon.x() == space.property("s6").toReal());
    CHECK(icon.width() == size.property("tabIcon").toReal());
    CHECK(part(*tabs[0], QStringLiteral("tabLabel")).x()
          == icon.x() + icon.width() + space.property("s4").toReal());

    // The close button: on the active tab, and on an inactive one only while it is hovered.
    auto const& activeClose = part(*tabs[0], QStringLiteral("tabCloseButton"));
    CHECK(activeClose.isVisible());
    CHECK(activeClose.width() == size.property("tabClose").toReal());
    CHECK(activeClose.height() == size.property("tabClose").toReal());
    auto& inactiveClose = part(*tabs[1], QStringLiteral("tabCloseButton"));
    CHECK_FALSE(inactiveClose.isVisible());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto const centre = tabs[1]->mapToScene(QPointF(tabs[1]->width() / 2, tabs[1]->height() / 2));
    QTest::mouseMove(window, centre.toPoint());
    REQUIRE(QTest::qWaitFor([&] { return inactiveClose.isVisible(); }, 1000));

    // A rule ends each inactive tab but the strip's last; the active tab has none.
    CHECK_FALSE(part(*tabs[0], QStringLiteral("tabJoinedRule")).isVisible());
    CHECK(part(*tabs[1], QStringLiteral("tabJoinedRule")).isVisible());
    CHECK_FALSE(part(*tabs[2], QStringLiteral("tabJoinedRule")).isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the active joined tab is the content's colour (offscreen)",
          "[contour][gui][qml][mainwindow][joinedtabs]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 3);
    auto const content = styleGroup(engine, "colors").property("paneGapFill").value<QColor>();
    auto const fillOf = [&](QQuickItem const& tab) {
        return part(tab, QStringLiteral("tabBackground")).property("color").value<QColor>();
    };
    CHECK(fillOf(*tabs[0]) == content);
    CHECK(fillOf(*tabs[1]).alpha() == 0);

    // The active label is resolved on the content's colour, not on the band's: a level dimmer while the
    // window is in the background.
    auto const* window = qobject_cast<QQuickWindow*>(root.get());
    auto const role = window->isActive() ? contour::config::ChromeColorRole::TabTextActive
                                         : contour::config::ChromeColorRole::TabTextDimmed;
    auto label = QColor {};
    REQUIRE(QMetaObject::invokeMethod(&chromeStyleOf(engine),
                                      "colorOn",
                                      Q_RETURN_ARG(QColor, label),
                                      Q_ARG(int, static_cast<int>(role)),
                                      Q_ARG(QColor, content)));
    // By value: QML hands the colour back in another spec.
    CHECK(part(*tabs[0], QStringLiteral("tabLabel")).property("color").value<QColor>().rgba()
          == label.rgba());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the joined strip keeps its profile dropdown (offscreen)",
          "[contour][gui][qml][mainwindow][joinedtabs]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    auto const& size = styleGroup(engine, "size");
    auto const stripButton = size.property("stripButton").toReal();
    auto const floor = named(*root, QStringLiteral("titleBar")).height()
                       - styleGroup(engine, "space").property("s1").toReal();
    auto& titleBar = named(*root, QStringLiteral("titleBar"));

    auto& menu = named(*root, QStringLiteral("newTabMenuButton"));
    CHECK(menu.isVisible());
    CHECK(menu.width() == size.property("profileButton").toReal());
    CHECK(menu.height() == stripButton);
    CHECK(menu.mapToItem(&titleBar, QPointF(0, menu.height())).y() == floor);

    auto& newTab = named(*root, QStringLiteral("newTabButton"));
    CHECK(newTab.width() == chromeStyleOf(engine).property("stripButtonWidth").toReal());
    CHECK(newTab.height() == stripButton);
    CHECK(newTab.mapToItem(&titleBar, QPointF(0, newTab.height())).y() == floor);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("one tab still shows the joined bar and its caption buttons (offscreen)",
          "[contour][gui][qml][mainwindow][joinedtabs]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple,
                                                      .affordances =
                                                          contour::test::ClientFrameAffordances::Drawn } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    CHECK(named(*root, QStringLiteral("titleBar")).isVisible());
    CHECK(tabDelegates(*root).size() == 1);
    CHECK(showsCloseButton(*root));

    // Under Minimal it is Pill's bare band: no tabs, but still the caption buttons.
    QQmlEngine minimalEngine;
    MockMainController minimal { MainWindowScene { .tabCount = 1,
                                                   .tabBarVisibility = TabBarVisibility::Multiple,
                                                   .affordances =
                                                       contour::test::ClientFrameAffordances::Drawn } };
    auto minimalRoot =
        loadMainWindow(minimalEngine,
                       minimal,
                       warnings,
                       densitySetup(ChromeStyle::Fluent, contour::config::ChromeDensity::Minimal));
    showWindow(*minimalRoot);
    auto& bare = named(*minimalRoot, QStringLiteral("titleBar"));
    CHECK(bare.isVisible());
    CHECK(bare.height() == styleGroup(minimalEngine, "size").property("bareBand").toReal());
    CHECK(showsCloseButton(*minimalRoot));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// The full-screen reveal on the macOS row under @p motion, with the band resting 24 below the window's top.
void checkRevealTop(MotionPreference motion)
{
    constexpr auto RevealTop = 24.0;
    auto band = FullScreenBand { motion };
    auto const reduced = motion == MotionPreference::Reduced;
    auto& titleBar = *band.titleBar;
    auto const& content = named(*band.root, QStringLiteral("content"));
    auto const contentY = content.y();
    auto const contentHeight = content.height();

    band.controller.setTitleBandRevealTop(RevealTop);
    QCoreApplication::processEvents();
    // Still concealed: slid wholly above the window from its new resting place, or faded out at it.
    CHECK(band.concealed());
    if (reduced)
        CHECK(titleBar.y() == RevealTop);

    QTest::mouseMove(band.window, centreOf(named(*band.root, QStringLiteral("bandRevealEdge"))));
    REQUIRE(QTest::qWaitFor(
        [&] {
            // Under Reduce Motion the band fades in place: it never leaves its resting top.
            if (reduced)
                CHECK(titleBar.y() == RevealTop);
            return band.revealed() && titleBar.y() == RevealTop && titleBar.opacity() == 1;
        },
        2000));
    CHECK(titleBar.isVisible());
    CHECK(bandBackgroundOf(*band.root).alphaF() == 1.0);
    // Over the top rows: the content keeps its place and its size.
    CHECK(content.y() == contentY);
    CHECK(content.height() == contentHeight);
    CHECK(band.controller.chromeHeight() == 0);

    QTest::mouseMove(band.window, band.awayPoint());
    REQUIRE(QTest::qWaitFor(
        [&] {
            if (reduced)
                CHECK(titleBar.y() == RevealTop);
            return band.concealed();
        },
        2000));
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}
} // namespace

TEST_CASE("the revealed band sits at the reveal top (offscreen)", "[contour][gui][qml][mainwindow][band]")
{
    SECTION("sliding")
    {
        checkRevealTop(MotionPreference::Full);
    }
    SECTION("fading, under Reduce Motion")
    {
        checkRevealTop(MotionPreference::Reduced);
    }
}

namespace
{
/// Where a pinned band rests below the menu bar, and the band height the controller measured, in these cases.
constexpr auto PinnedRevealTop = 24.0;
constexpr auto PinnedMeasured = 40.0;

/// 1c's Pinned presence (an interrupted remote connection in full screen) on the macOS row at the
/// controller's measured height, under a motion preference: loaded, taken into full screen, the pointer away
/// from the band.
struct PinnedFullScreenBand
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    QQuickItem* titleBar = nullptr;

    /// @param motion The motion preference the band moves under.
    /// @param inset  The traffic lights' inset the controller publishes.
    explicit PinnedFullScreenBand(MotionPreference motion, qreal inset = 0):
        controller { MainWindowScene { .tabCount = 1,
                                       .hostLabel = QStringLiteral("home"),
                                       .tabBarVisibility = TabBarVisibility::Multiple,
                                       .hostChipState = contour::window::HostChipState::Disconnected,
                                       .nativeControlsInset = inset } },
        root { loadMainWindow(engine, controller, warnings, macOSSetup(motion)) },
        window { qobject_cast<QQuickWindow*>(root.get()) }
    {
        REQUIRE(window != nullptr);
        controller.setTitleBandHeight(PinnedMeasured);
        controller.setTitleBandRevealTop(PinnedRevealTop);
        window->showFullScreen();
        REQUIRE(QTest::qWaitForWindowExposed(window));
        REQUIRE(QTest::qWaitFor([&] { return window->visibility() == QWindow::FullScreen; }, 2000));
        QTest::mouseMove(window, awayPoint());
        QCoreApplication::processEvents();
        titleBar = &named(*root, QStringLiteral("titleBar"));
        REQUIRE(titleBar->property("presence").toString() == QStringLiteral("pinned"));
    }

    ~PinnedFullScreenBand()
    {
        if (window != nullptr)
            window->showNormal();
    }

    PinnedFullScreenBand(PinnedFullScreenBand const&) = delete;
    PinnedFullScreenBand& operator=(PinnedFullScreenBand const&) = delete;
    PinnedFullScreenBand(PinnedFullScreenBand&&) = delete;
    PinnedFullScreenBand& operator=(PinnedFullScreenBand&&) = delete;

    /// A point on neither the band nor its reveal edge: the middle of the window's left half.
    [[nodiscard]] QPoint awayPoint() const { return { window->width() / 4, window->height() / 2 }; }

    /// Rests the pointer on the reveal edge, as on its way down from AppKit's menu bar.
    void hoverEdge() const
    {
        auto const& edge = named(*root, QStringLiteral("bandRevealEdge"));
        REQUIRE(edge.isVisible());
        QTest::mouseMove(window, centreOf(edge));
    }

    /// @return The width the band leaves clear at its leading edge for the traffic lights.
    [[nodiscard]] qreal leadingInset() const
    {
        return named(*root, QStringLiteral("leadingWindowControls")).width();
    }
};

/// 1c's Pinned presence under @p motion. AppKit's menu bar is hidden until the pointer reaches the screen
/// edge, so the pinned band rests at the top, and moves below the menu bar only while the pointer is on the
/// edge, as a native full-screen toolbar does (review I1, ruling a): sliding there and back under full
/// motion, at once under Reduce Motion.
void checkPinnedBand(MotionPreference motion)
{
    constexpr auto RevealTop = PinnedRevealTop;
    auto band = PinnedFullScreenBand { motion };
    auto* const window = band.window;
    auto& controller = band.controller;
    auto const& titleBar = *band.titleBar;
    CHECK(titleBar.isVisible());
    CHECK(titleBar.y() == 0);
    CHECK(titleBar.height() == PinnedMeasured);
    auto const& content = named(*band.root, QStringLiteral("content"));
    CHECK(content.y() == 0);
    CHECK(controller.chromeHeight() == 0);

    // Runs @p move, waits until the band rests at @p target, and says whether it slid there: whether the y
    // Behavior's animation ran, which it reports synchronously as the move starts it and a jump never does.
    // Asked of the animation rather than of y sampled mid-flight, which a stalled event-loop turn would let
    // slip past between two polls. A Behavior's animation is no QObject child of the item, so it is found by
    // its id in Main.qml's context.
    auto const* context = QQmlEngine::contextForObject(band.root.get());
    REQUIRE(context != nullptr);
    auto* const slide = context->objectForName(QStringLiteral("bandSlide"));
    REQUIRE(slide != nullptr);
    auto const moveTo = [&](auto&& move, double target) {
        auto runs = QSignalSpy { slide, SIGNAL(runningChanged(bool)) };
        move();
        REQUIRE(QTest::qWaitFor([&] { return titleBar.y() == target; }, 2000));
        return runs.count() > 0;
    };
    auto const slides = motion == MotionPreference::Full;

    // The pointer on the edge reveals AppKit's menu bar: the band makes room below it.
    CHECK(moveTo([&] { band.hoverEdge(); }, RevealTop) == slides);
    // Down onto the band keeps it there.
    QTest::mouseMove(window, centreOf(titleBar));
    QCoreApplication::processEvents();
    CHECK(titleBar.y() == RevealTop);
    CHECK(titleBar.isVisible());
    CHECK(content.y() == 0);
    CHECK(controller.chromeHeight() == 0);

    // Away again: back at the top, still pinned, and never faded.
    CHECK(moveTo([&] { QTest::mouseMove(window, band.awayPoint()); }, 0) == slides);
    CHECK(titleBar.isVisible());
    CHECK(titleBar.opacity() == 1);
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}
} // namespace

TEST_CASE("a pinned full-screen band rests at the top, and below the menu bar while the edge is hovered "
          "(offscreen)",
          "[contour][gui][qml][mainwindow][band][hosts]")
{
    SECTION("sliding")
    {
        checkPinnedBand(MotionPreference::Full);
    }
    SECTION("moving at once, under Reduce Motion")
    {
        checkPinnedBand(MotionPreference::Reduced);
    }
}

namespace
{
/// The traffic lights' inset the controller publishes in full screen too, where AppKit draws them in its
/// revealed strip (spike-decisions.md: x 12-70 in NSToolbarFullScreenWindow).
constexpr auto FullScreenInset = 84.0;
} // namespace

TEST_CASE("a revealed full-screen band leaves the traffic lights to the strip above it (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // Revealed, the band rests below AppKit's revealed strip, which holds the traffic lights (and takes the
    // clicks over itself): Tab 1 starts at the band's leading edge.
    auto band = FullScreenBand { MotionPreference::Full,
                                 MainWindowScene { .tabCount = 3, .nativeControlsInset = FullScreenInset } };
    band.revealFromEdge();
    band.waitUntilIn();
    CHECK(named(*band.root, QStringLiteral("leadingWindowControls")).width() == 0);
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a pinned full-screen band at the top leaves no room for the traffic lights (offscreen)",
          "[contour][gui][qml][mainwindow][band][hosts]")
{
    auto band = PinnedFullScreenBand { MotionPreference::Full, FullScreenInset };
    CHECK(band.titleBar->y() == 0);
    CHECK(band.leadingInset() == 0);
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a pinned full-screen band below the menu bar leaves the traffic lights to the strip (offscreen)",
          "[contour][gui][qml][mainwindow][band][hosts]")
{
    // The pointer on the edge reveals AppKit's strip with the traffic lights, and the band moves below it.
    auto band = PinnedFullScreenBand { MotionPreference::Full, FullScreenInset };
    band.hoverEdge();
    REQUIRE(QTest::qWaitFor([&] { return band.titleBar->y() == PinnedRevealTop; }, 2000));
    CHECK(band.leadingInset() == 0);
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the band paints the window's own fill (offscreen)", "[contour][gui][qml][mainwindow][band]")
{
    // The window, not the row, has the last word: a window whose material failed paints the tint where its
    // row would clear the band.
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);
    REQUIRE(bandBackgroundOf(*root).alpha() == 0);

    controller.setTitleBandFill(contour::config::BandFill::Tint);
    QCoreApplication::processEvents();
    CHECK(bandBackgroundOf(*root).alpha() == 255);

    controller.setTitleBandFill(contour::config::BandFill::Clear);
    QCoreApplication::processEvents();
    CHECK(bandBackgroundOf(*root).alpha() == 0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the ContourLinux band keeps today's colour (offscreen)", "[contour][gui][qml][mainwindow][band]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
    showWindow(*root);
    CHECK(bandBackgroundOf(*root) == QGuiApplication::palette().color(QPalette::Window));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Fluent band is cleared over Mica, and tinted without it (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    using contour::config::TransparencyPreference;
    for (auto const transparency: { TransparencyPreference::Allowed, TransparencyPreference::Reduced })
    {
        auto const reduced = transparency == TransparencyPreference::Reduced;
        INFO("reduced transparency: " << reduced);
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3 } };
        contour::test::QmlMessageCapture warnings;
        auto setup = rowSetup(ChromeStyle::Fluent);
        setup.accessibility.transparency = transparency;
        auto root = loadMainWindow(engine, controller, warnings, setup);
        showWindow(*root);
        CHECK(bandBackgroundOf(*root).alpha() == (reduced ? 255 : 0));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the Terminal row's band keeps the palette and one cell (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
    showWindow(*root);
    REQUIRE(controller.titleBandHeight() == 0);
    CHECK(bandBackgroundOf(*root) == QGuiApplication::palette().color(QPalette::Window));
    CHECK(named(*root, QStringLiteral("titleBar")).height()
          == chromeStyleOf(engine).property("cellHeight").toReal());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// One accessible node met by a walk of the tree, with the roles of the nodes above it.
struct AccessibleNode
{
    QAccessible::Role role;               ///< The node's own role.
    QString name;                         ///< The node's accessible name.
    std::vector<QAccessible::Role> above; ///< The roles from the root down to its parent.
};

/// Walks the accessible tree below @p node, depth first.
void walkAccessible(QAccessibleInterface* node,
                    std::vector<QAccessible::Role>& above,
                    std::vector<AccessibleNode>& out)
{
    if (node == nullptr || !node->isValid())
        return;
    out.push_back(
        AccessibleNode { .role = node->role(), .name = node->text(QAccessible::Name), .above = above });
    above.push_back(node->role());
    for (auto const index: std::views::iota(0, node->childCount()))
        walkAccessible(node->child(index), above, out);
    above.pop_back();
}

/// @return Every accessible node of @p window.
[[nodiscard]] std::vector<AccessibleNode> accessibleTreeOf(QQuickWindow& window)
{
    auto nodes = std::vector<AccessibleNode> {};
    auto above = std::vector<QAccessible::Role> {};
    walkAccessible(QAccessible::queryAccessibleInterface(&window), above, nodes);
    return nodes;
}
} // namespace

TEST_CASE("the assembled band has one title bar, holding the chip, the strip, its tabs and their badges "
          "(offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // §2.6: the chrome's accessible tree, walked. A second bar holding only the tabs (the Pill shape with
    // the tabs at the bottom) is no second title bar.
    using contour::config::TabBarPosition;
    for (auto const position: { TabBarPosition::Top, TabBarPosition::Bottom })
    {
        INFO("tabs at the " << (position == TabBarPosition::Top ? "top" : "bottom"));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = contour::test::DecorTabCount,
                                                          .hostLabel = QStringLiteral("home"),
                                                          .decor = TabDecor::EveryBadge,
                                                          .tabBarPosition = position } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        showWindow(*root);
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        REQUIRE(window != nullptr);

        auto const tree = accessibleTreeOf(*window);
        auto const under = [](AccessibleNode const& node, QAccessible::Role role) {
            return std::ranges::find(node.above, role) != node.above.end();
        };
        CHECK(std::ranges::count(tree, QAccessible::TitleBar, &AccessibleNode::role) == 1);
        auto const chip = std::ranges::find_if(tree, [](AccessibleNode const& node) {
            return node.name.startsWith(QStringLiteral("Attached to home"));
        });
        REQUIRE(chip != tree.end());
        CHECK(under(*chip, QAccessible::TitleBar));
        auto const strip = std::ranges::find(tree, QAccessible::PageTabList, &AccessibleNode::role);
        REQUIRE(strip != tree.end());
        CHECK(under(*strip, QAccessible::TitleBar) == (position == TabBarPosition::Top));
        auto const tabs = std::ranges::count_if(tree, [&](AccessibleNode const& node) {
            return node.role == QAccessible::PageTab && under(node, QAccessible::PageTabList);
        });
        CHECK(tabs == contour::test::DecorTabCount);
        auto const badge = std::ranges::find_if(tree, [&](AccessibleNode const& node) {
            return node.role == QAccessible::StaticText && under(node, QAccessible::PageTab)
                   && node.name
                          == QString::fromUtf8(
                              contour::window::tabBadgeLook(contour::window::TabBadgeKind::Blocked)
                                  .accessibleName);
        });
        CHECK(badge != tree.end());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a full-screen window keeps its band shown while its remote connection is interrupted (offscreen)",
          "[contour][gui][qml][mainwindow][band][hosts]")
{
    // A full-screen band reveals only on the edge; a disconnected window's frozen screen would then say
    // nothing about why. While the connection is interrupted the band stays pinned over the top rows, chip
    // and all, and once it is back the band leaves again.
    using contour::window::HostChipState;
    for (auto const row: { ChromeStyle::Fluent, ChromeStyle::MacOS })
    {
        INFO("row " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 1,
                                                          .hostLabel = QStringLiteral("home"),
                                                          .tabBarVisibility = TabBarVisibility::Multiple,
                                                          .hostChipState = HostChipState::Disconnected } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        REQUIRE(window != nullptr);
        window->showFullScreen();
        REQUIRE(QTest::qWaitForWindowExposed(window));
        REQUIRE(QTest::qWaitFor([&] { return window->visibility() == QWindow::FullScreen; }, 2000));
        QTest::mouseMove(window, QPoint { window->width() / 4, window->height() / 2 });
        QCoreApplication::processEvents();

        auto& titleBar = named(*root, QStringLiteral("titleBar"));
        auto& content = named(*root, QStringLiteral("content"));
        auto& chip = named(*root, QStringLiteral("hostChip"));
        CHECK(titleBar.property("presence").toString() == QStringLiteral("pinned"));
        CHECK(titleBar.isVisible());
        CHECK(titleBar.y() == 0);
        CHECK(titleBar.opacity() == 1);
        CHECK(chip.isVisible());
        // Over the top rows, as a revealed band is: the content keeps the whole screen.
        CHECK(content.y() == 0);
        CHECK(controller.chromeHeight() == 0);

        controller.setHostChipState(static_cast<int>(HostChipState::Connected));
        REQUIRE(QTest::qWaitFor([&] { return !titleBar.isVisible(); }, 2000));
        CHECK(content.y() == 0);
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
        window->showNormal();
    }
}

TEST_CASE("the tab bar at the bottom on macOS keeps the band at the top (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene {
        .tabCount = 3, .tabBarPosition = contour::config::TabBarPosition::Bottom } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    auto const* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);

    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    auto& tabsBar = named(*root, QStringLiteral("tabsBar"));
    auto& content = named(*root, QStringLiteral("content"));
    CHECK(titleBar.y() == 0);
    CHECK(named(*root, QStringLiteral("bandTitle")).isVisible());
    CHECK(tabsBar.isVisible());
    CHECK(tabsBar.y() + tabsBar.height() == window->height());
    CHECK(isInside(&named(*root, QStringLiteral("tabWell")), &tabsBar));
    // One strip, the tabs bar's, and one "+", the band's.
    CHECK(root->findChildren<QQuickItem*>(QStringLiteral("tabList")).size() == 1);
    CHECK(root->findChildren<QQuickItem*>(QStringLiteral("newTabButton")).size() == 1);
    CHECK(isInside(&named(*root, QStringLiteral("newTabButton")), &titleBar));
    CHECK(root->findChildren<QQuickItem*>(QStringLiteral("hostChip")).size() == 1);
    // The content runs between the two.
    CHECK(content.y() == titleBar.height());
    CHECK(content.y() + content.height() == tabsBar.y());
    CHECK(controller.chromeHeight() == static_cast<int>(titleBar.height() + tabsBar.height()));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a one-tab remote window shows its band while its connection is interrupted (offscreen)",
          "[contour][gui][qml][mainwindow][band][hosts]")
{
    // tab_bar_visibility Multiple hides a one-tab strip on the Flat rows, and the strip is the host chip's
    // home: a remote window whose connection is not up shows it anyway (R-d), chip and all. (Fluent's joined
    // band always shows: see "one tab still shows the joined bar".)
    using contour::window::HostChipState;
    for (auto const row: { ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        for (auto const state:
             { HostChipState::Connected, HostChipState::Reconnecting, HostChipState::Disconnected })
        {
            auto const& look = contour::window::hostChipLook(state);
            INFO("row " << int(row) << ", " << look.accessibleState);
            QQmlEngine engine;
            MockMainController controller { MainWindowScene { .tabCount = 1,
                                                              .hostLabel = QStringLiteral("home"),
                                                              .tabBarVisibility = TabBarVisibility::Multiple,
                                                              .hostChipState = state } };
            contour::test::QmlMessageCapture warnings;
            auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));

            auto& titleBar = named(*root, QStringLiteral("titleBar"));
            auto const interrupted = state != HostChipState::Connected;
            CHECK(titleBar.isVisible() == interrupted);
            auto& chip = named(*root, QStringLiteral("hostChip"));
            CHECK(isInside(&chip, &titleBar));
            CHECK(chip.isVisible() == interrupted);
            if (interrupted)
            {
                CHECK(titleBar.height() > 0);
                CHECK(part(chip, QStringLiteral("hostChipStatus")).property("text").toString()
                      == QString::fromUtf8(look.status));
            }
            CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
        }
    }

    SECTION("the band follows the connection live")
    {
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 1,
                                                          .hostLabel = QStringLiteral("home"),
                                                          .tabBarVisibility = TabBarVisibility::Multiple } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
        auto& titleBar = named(*root, QStringLiteral("titleBar"));
        CHECK_FALSE(titleBar.isVisible());
        controller.setHostChipState(static_cast<int>(HostChipState::Disconnected));
        QCoreApplication::processEvents();
        CHECK(titleBar.isVisible());
        controller.setHostChipState(static_cast<int>(HostChipState::Connected));
        QCoreApplication::processEvents();
        CHECK_FALSE(titleBar.isVisible());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the macOS + opens profiles on a secondary click (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // One tab (the band shows the title, so no strip exists) and three (the well beside the "+").
    for (auto const tabCount: { 1, 3 })
    {
        INFO("tabs: " << tabCount);
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = tabCount,
                                                          .tabBarVisibility = TabBarVisibility::Multiple } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        showWindow(*root);
        auto* window = qobject_cast<QQuickWindow*>(root.get());

        CHECK(root->findChild<QQuickItem*>(QStringLiteral("newTabMenuButton")) == nullptr);
        CHECK(root->findChildren<QQuickItem*>(QStringLiteral("newTabButton")).size() == 1);
        auto& plus = named(*root, QStringLiteral("newTabButton"));
        CHECK(plus.isVisible());
        CHECK(plus.width() == styleGroup(engine, "size").property("toolbarButton").toReal());
        CHECK(evaluateIn(plus, QStringLiteral("Accessible.role")).toInt() == QAccessible::Button);

        QTest::mouseClick(window, Qt::LeftButton, {}, centreOf(plus));
        CHECK(controller.calls.count(QStringLiteral("createNewTab")) == 2); // the startup tab, and this one
        CHECK(controller.newTabMenuOpenings == 0);

        QTest::mouseClick(window, Qt::RightButton, {}, centreOf(plus));
        CHECK(controller.newTabMenuOpenings == 1);
        auto* menu = root->findChild<QObject*>(QStringLiteral("newTabMenu"));
        REQUIRE(menu != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
        CHECK(menu->property("count").toInt() == static_cast<int>(contour::test::MockProfileNames.size()));

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("Fluent keeps its caret")
    {
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
        CHECK(named(*root, QStringLiteral("newTabMenuButton")).isVisible());
        CHECK(root->findChildren<QQuickItem*>(QStringLiteral("newTabButton")).size() == 1);
        CHECK(isInside(&named(*root, QStringLiteral("newTabButton")),
                       &named(*root, QStringLiteral("tabStrip"))));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the block menu opens at the anchor the controller names, not at the pointer (offscreen)",
          "[contour][gui][qml][mainwindow][blockmenu]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
    showWindow(*root);
    controller.setContextMenuModel(QVariantList { QVariantMap { { QStringLiteral("kind"), "command" },
                                                                { QStringLiteral("title"), "Copy Output" },
                                                                { QStringLiteral("enabled"), true },
                                                                { QStringLiteral("checkable"), false },
                                                                { QStringLiteral("checked"), false },
                                                                { QStringLiteral("actionId"), 1 } } });
    auto* menu = root->findChild<QObject*>(QStringLiteral("terminalContextMenu"));
    REQUIRE(menu != nullptr);
    constexpr auto Anchor = QPointF { 120, 200 };

    controller.setContextMenuAnchor(Anchor);
    controller.openContextMenu();
    REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
    CHECK(menu->property("x").toReal() == Anchor.x());
    CHECK(menu->property("y").toReal() == Anchor.y());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("an anchor under the title band opens the block menu below it (offscreen)",
          "[contour][gui][qml][mainwindow][blockmenu]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    controller.setContextMenuModel(QVariantList { QVariantMap { { QStringLiteral("kind"), "command" },
                                                                { QStringLiteral("title"), "Copy Output" },
                                                                { QStringLiteral("enabled"), true },
                                                                { QStringLiteral("checkable"), false },
                                                                { QStringLiteral("checked"), false },
                                                                { QStringLiteral("actionId"), 1 } } });
    auto& band = named(*root, QStringLiteral("titleBar"));
    REQUIRE(band.isVisible());
    auto* menu = root->findChild<QObject*>(QStringLiteral("terminalContextMenu"));
    REQUIRE(menu != nullptr);
    auto const bandBottom = band.y() + band.height();

    // A block at the top of the pane: its anchor is inside the band, whose centred title the menu would
    // cover.
    controller.setContextMenuAnchor(QPointF { 120, bandBottom / 2 });
    controller.openContextMenu();
    REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
    CHECK(menu->property("x").toReal() == 120);
    CHECK(menu->property("y").toReal() == bandBottom);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row snaps the native inset to cells and brackets its chips (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    constexpr auto Inset = 70.0;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene {
        .tabCount = 2, .hostLabel = QStringLiteral("build-01"), .nativeControlsInset = Inset } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
    showWindow(*root);

    qreal snapped = 0;
    REQUIRE(QMetaObject::invokeMethod(
        &chromeStyleOf(engine), "snapToCells", Q_RETURN_ARG(qreal, snapped), Q_ARG(qreal, Inset)));
    CHECK(snapped >= Inset);
    auto const cell = chromeStyleOf(engine).property("cellWidth").toReal();
    CHECK(std::fmod(snapped, cell) == Catch::Approx(0.0).margin(1e-6));
    CHECK(named(*root, QStringLiteral("leadingWindowControls")).width() == snapped);

    auto& chip = named(*root, QStringLiteral("hostChip"));
    auto const& open = part(chip, QStringLiteral("hostChipOpen"));
    auto const& label = part(chip, QStringLiteral("hostChipLabel"));
    auto const& close = part(chip, QStringLiteral("hostChipClose"));
    CHECK(open.property("text").toString() == QStringLiteral("["));
    CHECK(close.property("text").toString() == QStringLiteral("]"));
    CHECK(part(chip, QStringLiteral("hostChipDotGlyph")).isVisible());
    CHECK_FALSE(part(chip, QStringLiteral("hostChipDot")).isVisible());
    CHECK(open.x() + open.width() <= label.x());
    CHECK(label.x() + label.width() <= close.x());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the band grows to the title bar in whole cells (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // D3 = GrowBandToCells (spike-decisions.md): AppKit re-places the traffic lights on every expose, resize
    // and full-screen change, so the Terminal row's band is not snapped to them; the controller grows it to
    // whole cell rows around them (resolveTitleBandGeometry), and the band takes that height as measured.
    constexpr auto Inset = 70.0;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 2, .nativeControlsInset = Inset } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
    showWindow(*root);

    auto const cellHeight = chromeStyleOf(engine).property("cellHeight").toReal();
    controller.setTitleBandHeight(2 * cellHeight);
    QCoreApplication::processEvents();

    auto const& titleBar = named(*root, QStringLiteral("titleBar"));
    CHECK(titleBar.height() == 2 * cellHeight);
    CHECK(controller.chromeHeight() == static_cast<int>(2 * cellHeight)); // a cell is whole pixels
    // The leading slot still clears the lights in whole cells.
    qreal snapped = 0;
    REQUIRE(QMetaObject::invokeMethod(
        &chromeStyleOf(engine), "snapToCells", Q_RETURN_ARG(qreal, snapped), Q_ARG(qreal, Inset)));
    CHECK(named(*root, QStringLiteral("leadingWindowControls")).width() == snapped);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row keeps the active tab and the + in view when its tabs overflow (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // A tab is a whole number of cells, a cell rarely a whole number of pixels, so where the last tab ends
    // has a different fraction for every tab count -- and Flickable settles a fractional scroll onto a whole
    // pixel, either way. Over a run of counts some land on each side, whatever the platform's font.
    for (auto const tabCount: std::views::iota(12, 24))
    {
        INFO("tabs: " << tabCount);
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = tabCount, .activeTab = tabCount - 1 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
        showWindow(*root);

        auto const& list = named(*root, QStringLiteral("tabList"));
        auto const tabs = tabDelegates(*root);
        REQUIRE(tabs.size() == static_cast<std::size_t>(tabCount));
        auto const* active = tabs.back();
        REQUIRE(active != nullptr);
        auto const contentX = list.property("contentX").toReal();
        CHECK(contentX > 0);
        CHECK(active->x() >= contentX);
        CHECK(active->x() + active->width() <= contentX + list.width());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    constexpr auto TabCount = 20;
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = TabCount, .activeTab = TabCount - 1 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
    showWindow(*root);

    auto& titleBar = named(*root, QStringLiteral("titleBar"));
    auto const& list = named(*root, QStringLiteral("tabList"));
    REQUIRE(tabDelegates(*root).size() == static_cast<std::size_t>(TabCount));

    // Back to the first tab: the strip scrolls it into view again.
    controller.setActiveTab(0);
    auto const firstInView = [&] {
        auto const first = tabDelegates(*root).front();
        auto const x = list.property("contentX").toReal();
        return first != nullptr && first->x() >= x && first->x() + first->width() <= x + list.width();
    };
    REQUIRE(QTest::qWaitFor(firstInView, 2000));

    auto& plus = named(*root, QStringLiteral("newTabButton"));
    CHECK(plus.isVisible());
    CHECK(plus.mapToItem(&titleBar, QPointF(plus.width(), 0)).x() <= titleBar.width());
    auto const& drag = named(*root, QStringLiteral("dragRegion"));
    CHECK(drag.width() >= styleGroup(engine, "space").property("dragReserve").toReal());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a pointer that reaches the sliding band keeps it revealed (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const motion: { MotionPreference::Full, MotionPreference::Reduced })
    {
        INFO("reduced motion: " << (motion == MotionPreference::Reduced));
        auto band = FullScreenBand { motion };
        band.revealFromEdge();
        // Down to where the band will rest, while it is still sliding in from above (or fading in, in place).
        if (motion == MotionPreference::Reduced)
        {
            REQUIRE(band.titleBar->opacity() < 1);
            CHECK(band.titleBar->y() == 0);
        }
        else
            REQUIRE(band.titleBar->y() < 0);
        QTest::mouseMove(
            band.window,
            QPoint(band.window->width() / 2, static_cast<int>(band.titleBar->implicitHeight() / 2)));
        band.linger();
        CHECK(band.revealed());
        CHECK(band.titleBar->y() == 0);
        CHECK(band.titleBar->opacity() == 1);

        // Away: it goes, after its short grace.
        QTest::mouseMove(band.window, band.awayPoint());
        REQUIRE(QTest::qWaitFor([&] { return !band.revealed(); }, 2000));
        REQUIRE(QTest::qWaitFor([&] { return band.concealed(); }, 2000));
        CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a full-screen band stays while one of its controls holds keyboard focus (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    // A keyboard user on the "+" (or the host chip) must not see the band hide under them once the pointer
    // has gone: the focus holds it, as an open popup does, and it goes when the focus leaves.
    auto band = FullScreenBand {};
    band.revealFromEdge();
    band.waitUntilIn();
    auto& plus = named(*band.root, QStringLiteral("newTabButton"));
    plus.forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(plus.hasActiveFocus());
    CHECK(band.titleBar->property("holdsFocus").toBool());
    QTest::mouseMove(band.window, band.awayPoint());
    band.linger();
    CHECK(band.revealed());

    named(*band.root, QStringLiteral("content")).forceActiveFocus(Qt::TabFocusReason);
    CHECK_FALSE(band.titleBar->property("holdsFocus").toBool());
    REQUIRE(QTest::qWaitFor([&] { return !band.revealed(); }, 2000));
    CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a full-screen band stays while its menu or a rename is open (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    SECTION("the + menu")
    {
        auto band = FullScreenBand {};
        band.revealFromEdge();
        band.waitUntilIn();
        auto& plus = named(*band.root, QStringLiteral("newTabButton"));
        QTest::mouseClick(band.window, Qt::RightButton, {}, centreOf(plus));
        auto* menu = band.root->findChild<QObject*>(QStringLiteral("newTabMenu"));
        REQUIRE(menu != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
        QTest::mouseMove(band.window, band.awayPoint());
        band.linger();
        CHECK(band.revealed());

        // Once the menu has gone, so does the band.
        REQUIRE(QMetaObject::invokeMethod(menu, "dismiss"));
        REQUIRE(QTest::qWaitFor([&] { return !band.revealed(); }, 2000));
        CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("an inline rename")
    {
        auto band = FullScreenBand {};
        band.revealFromEdge();
        band.waitUntilIn();
        band.controller.beginActiveTabTitleEdit();
        // A delegate has no QObject parent: the field is found in the visual tree.
        REQUIRE(QTest::qWaitFor(
            [&] { return isInVisualTree(*band.titleBar, QStringLiteral("tabRenameField")); }, 2000));
        QTest::mouseMove(band.window, band.awayPoint());
        band.linger();
        CHECK(band.revealed());
        CHECK(band.warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a concealed full-screen band is not on screen, nor in the accessibility tree (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const motion: { MotionPreference::Full, MotionPreference::Reduced })
    {
        INFO("reduced motion: " << (motion == MotionPreference::Reduced));
        auto band = FullScreenBand { motion };
        CHECK_FALSE(band.revealed());
        CHECK_FALSE(band.titleBar->isVisible());
        band.revealFromEdge();
        CHECK(band.titleBar->isVisible());

        // Still on screen while it leaves -- sliding, or under Reduce Motion fading -- and gone once it has.
        QTest::mouseMove(band.window, band.awayPoint());
        REQUIRE(QTest::qWaitFor([&] { return !band.revealed(); }, 2000));
        CHECK(band.titleBar->isVisible());
        REQUIRE(QTest::qWaitFor([&] { return band.concealed(); }, 2000));
        CHECK_FALSE(band.titleBar->isVisible());
    }
}

TEST_CASE("the macOS + opens profiles on a long press, and says so (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto& plus = named(*root, QStringLiteral("newTabButton"));
    CHECK_FALSE(evaluateIn(plus, QStringLiteral("Accessible.description")).toString().isEmpty());

    QTest::mousePress(window, Qt::LeftButton, {}, centreOf(plus));
    QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() * 2);
    QTest::mouseRelease(window, Qt::LeftButton, {}, centreOf(plus));
    CHECK(controller.newTabMenuOpenings == 1);
    CHECK(controller.calls.count(QStringLiteral("createNewTab")) == 1); // only the startup tab
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Main.qml fills the title bar under the expanded client area", "[display][cocoa][mainwindow][band]")
{
    // F14 on the real root: under Qt::ExpandedClientAreaHint an ApplicationWindow pads its content by the
    // safe area, which Main.qml zeroes, so the content and the band start at the window's top. Needs the
    // cocoa platform plugin, which alone reports a title bar as the safe area.
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")
        || !contour::test::displayTestsEnabled(core::LiveEnvironment {}))
        SKIP("Needs CONTOUR_TEST_DISPLAY=1 and the cocoa platform plugin");
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    SKIP(
        "Qt::ExpandedClientAreaHint, Qt::NoTitleBarBackgroundHint and QWindow::safeAreaMargins are Qt 6.9's");
#else
    QQmlEngine engine;
    MockMainController controller {};
    controller.setNeedsExpandedClientArea(true);
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    CHECK(window->flags().testFlag(Qt::ExpandedClientAreaHint));
    CHECK_FALSE(window->flags().testFlag(Qt::NoTitleBarBackgroundHint));
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window));

    CHECK(window->safeAreaMargins().top() > 0);
    auto* const content = window->property("contentItem").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    CHECK(content->mapToScene({ 0, 0 }).y() == 0);
    CHECK(named(*root, QStringLiteral("titleBar")).mapToScene({ 0, 0 }).y() == 0);

    // The client backdrop clears the title bar's background through the same `flags`.
    controller.setClearsTitleBarBackground(true);
    QCoreApplication::processEvents();
    CHECK(window->flags().testFlag(Qt::NoTitleBarBackgroundHint));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
#endif
}

// {{{ The command palette over a dialog: the keyboard goes back to what the palette opened over.
namespace
{

/// A palette model over the action catalogue, for Main.qml's CommandPalette, and one terminal pane for
/// restoreTerminalFocus() to give the keyboard to.
struct MainWindowPalette
{
    MainWindowPalette()
    {
        pane.setSession(&session);
        pane.setActive(true);
        model.refresh({});
    }

    /// The scene Main.qml is loaded with: the pane and the palette.
    [[nodiscard]] contour::test::MainWindowScene scene()
    {
        return { .rootPane = &pane, .commandPalette = &model };
    }

    contour::test::MockSession session;
    contour::test::MockPaneProxy pane { /*leaf=*/true };
    contour::command::CommandHistory history { 5 };
    contour::command::ActionCommandSource actions {};
    contour::window::CommandPaletteModel model { history,
                                                 std::vector<contour::command::CommandSource const*> {
                                                     &actions },
                                                 contour::command::ShortcutStyle::Text };
};

/// Whether @p item is @p ancestor or lies beneath it in the visual tree.
[[nodiscard]] bool isWithin(QQuickItem const* item, QQuickItem const* ancestor)
{
    return item != nullptr && ancestor != nullptr && (item == ancestor || ancestor->isAncestorOf(item));
}

/// Shows and activates Main.qml's window, so it holds an active focus item.
[[nodiscard]] QQuickWindow& activeMainWindow(QObject& root)
{
    showWindow(root);
    auto* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    window->requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(window));
    return *window;
}

} // namespace

TEST_CASE("Closing the palette over a dialog gives the dialog its focus back (offscreen)",
          "[contour][gui][qml][mainwindow][palette]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    MockMainController controller { palette.scene() };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);
    auto& window = activeMainWindow(*root);

    auto* dialog = root->findChild<QObject*>(QStringLiteral("aboutDialog"));
    auto* commandPalette = root->findChild<QObject*>(QStringLiteral("commandPalette"));
    REQUIRE(dialog != nullptr);
    REQUIRE(commandPalette != nullptr);
    controller.showAbout();
    waitForPopupOpened(*dialog);
    auto* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
    REQUIRE(accept != nullptr);
    accept->forceActiveFocus();
    QCoreApplication::processEvents();
    REQUIRE(window.activeFocusItem() == accept);

    controller.openCommandPalette();
    waitForPopupOpened(*commandPalette);
    REQUIRE(window.activeFocusItem() != accept);

    SECTION("the button that had it")
    {
        QTest::keyClick(&window, Qt::Key_Escape);
        waitForPopupClosed(*commandPalette);

        CHECK(dialog->property("opened").toBool());
        CHECK(window.activeFocusItem() == accept);
    }

    SECTION("the dialog, where that button has gone while the palette was up")
    {
        accept->setVisible(false);
        QTest::keyClick(&window, Qt::Key_Escape);
        waitForPopupClosed(*commandPalette);

        CHECK(dialog->property("opened").toBool());
        auto* content = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content != nullptr);
        CHECK(isWithin(window.activeFocusItem(), content->parentItem()));
    }
    CHECK(controller.ranCommands.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A picked command that opens a dialog keeps that dialog focused (offscreen)",
          "[contour][gui][qml][mainwindow][palette]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    MockMainController controller { palette.scene() };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);
    auto& window = activeMainWindow(*root);

    auto* dialog = root->findChild<QObject*>(QStringLiteral("aboutDialog"));
    auto* commandPalette = root->findChild<QObject*>(QStringLiteral("commandPalette"));
    REQUIRE(dialog != nullptr);
    REQUIRE(commandPalette != nullptr);

    controller.openCommandPalette();
    waitForPopupOpened(*commandPalette);
    auto* filter = commandPalette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    filter->setProperty("text", QStringLiteral("ShowAbout"));
    QCoreApplication::processEvents();
    QTest::keyClick(&window, Qt::Key_Return);
    waitForPopupClosed(*commandPalette);
    waitForPopupOpened(*dialog);

    CHECK(controller.ranCommands == QStringList { QStringLiteral("ShowAbout") });
    // The palette's own hand-back ran before the dispatch, so the dialog's focus is the last word.
    auto* frame = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(frame != nullptr);
    CHECK(isWithin(window.activeFocusItem(), frame->parentItem()));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Without a dialog, closing the palette hands the keyboard back to the terminal (offscreen)",
          "[contour][gui][qml][mainwindow][palette]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    MockMainController controller { palette.scene() };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);
    auto& window = activeMainWindow(*root);

    auto* commandPalette = root->findChild<QObject*>(QStringLiteral("commandPalette"));
    REQUIRE(commandPalette != nullptr);
    controller.openCommandPalette();
    waitForPopupOpened(*commandPalette);
    QTest::keyClick(&window, Qt::Key_Escape);
    waitForPopupClosed(*commandPalette);

    // restoreTerminalFocus() ran: the pane tree has the keyboard, nothing in the overlay keeps it.
    auto* focus = window.activeFocusItem();
    auto* overlayLayer = commandPalette->property("parent").value<QQuickItem*>();
    auto const items = root->findChildren<QQuickItem*>();
    auto const tree = std::ranges::find_if(items, [](QQuickItem const* item) {
        return QByteArrayView(item->metaObject()->className()).startsWith("PaneNode");
    });
    auto* paneTree = tree != items.end() ? *tree : nullptr;
    REQUIRE(overlayLayer != nullptr);
    REQUIRE(paneTree != nullptr);
    CHECK_FALSE(isWithin(focus, overlayLayer));
    CHECK(isWithin(focus, paneTree));
    CHECK(controller.ranCommands.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
// }}}

// {{{ Notices
namespace
{

/// Where @p item is laid out, in window coordinates: its own transform -- a toast's slide as it shows,
/// which no animation advances offscreen -- left out.
[[nodiscard]] QRectF sceneRectOf(QQuickItem const& item)
{
    auto const laidOut = QRectF { item.position(), QSizeF { item.width(), item.height() } };
    return item.parentItem() != nullptr ? item.parentItem()->mapRectToScene(laidOut) : laidOut;
}

/// What a copy tells its window.
[[nodiscard]] contour::window::Notice copiedNotice()
{
    return contour::window::Notice { .kind = contour::window::NoticeKind::Copied,
                                     .text = contour::window::copiedNoticeText(3) };
}

} // namespace

TEST_CASE("Main.qml notices: the toast sits in a band that holds it, else in the content (offscreen)",
          "[contour][gui][qml][mainwindow][notices]")
{
    SECTION("the macOS row: in the title band")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        showWindow(*root);
        REQUIRE(root->property("band").toMap().value("band").toString() == "shown");

        controller.notices().post(copiedNotice());
        QCoreApplication::processEvents();
        auto* const toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(toast != nullptr);
        REQUIRE(titleBar != nullptr);
        CHECK(toast->property("shown").toBool());
        CHECK(toast->property("text").toString() == "Copied 3 lines");
        auto const* toastText = toast->findChild<QQuickItem*>(QStringLiteral("toastText"));
        REQUIRE(toastText != nullptr);
        CHECK(toastText->property("textFormat").toInt() == Qt::PlainText);
        CHECK(sceneRectOf(*titleBar).contains(sceneRectOf(*toast).center()));
        auto const* style = engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
        REQUIRE(style != nullptr);
        CHECK(toast->height()
              == Catch::Approx(style->property("size").value<QObject*>()->property("toast").toDouble()));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("the ContourLinux row with the strip hidden: no band, so in the content")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
        showWindow(*root);
        controller.setTabBarShouldShow(false);
        QCoreApplication::processEvents();
        REQUIRE(root->property("band").toMap().value("band").toString() == "hidden");

        controller.notices().post(copiedNotice());
        QCoreApplication::processEvents();
        auto* const toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
        REQUIRE(toast != nullptr);
        REQUIRE(content != nullptr);
        CHECK(sceneRectOf(*content).contains(sceneRectOf(*toast)));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("the Fluent row with the strip hidden: the joined band stays, so in the band")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
        showWindow(*root);
        controller.setTabBarShouldShow(false);
        QCoreApplication::processEvents();
        REQUIRE(root->property("band").toMap().value("band").toString() == "shown");

        controller.notices().post(copiedNotice());
        QCoreApplication::processEvents();
        auto const* toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(toast != nullptr);
        REQUIRE(titleBar != nullptr);
        CHECK(toast->property("shown").toBool());
        CHECK(sceneRectOf(*titleBar).contains(sceneRectOf(*toast).center()));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("the Terminal row: the one-row toast sits in the one-row bar")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal));
        showWindow(*root);
        REQUIRE(root->property("band").toMap().value("band").toString() == "shown");
        controller.notices().post(copiedNotice());
        QCoreApplication::processEvents();
        auto const* toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(toast != nullptr);
        REQUIRE(titleBar != nullptr);
        CHECK(toast->property("shown").toBool());
        CHECK(toast->property("host").toString() == "titleBand");
        CHECK(sceneRectOf(*titleBar).contains(sceneRectOf(*toast).center()));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("without a band, a cursor in the top rows sends it to the bottom")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.cursorRow = 0;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::ContourLinux));
        showWindow(*root);
        controller.setTabBarShouldShow(false);
        QCoreApplication::processEvents();
        REQUIRE(root->property("band").toMap().value("band").toString() == "hidden");

        controller.notices().post(copiedNotice());
        QCoreApplication::processEvents();
        auto const* toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
        REQUIRE(toast != nullptr);
        REQUIRE(content != nullptr);
        CHECK(toast->property("host").toString() == "contentBottom");
        CHECK(sceneRectOf(*toast).center().y() > sceneRectOf(*content).center().y());
        CHECK(sceneRectOf(*content).contains(sceneRectOf(*toast)));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("it is no alert of its own: the notices say it, once")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);
        auto* const toast = root->findChild<QQuickItem*>(QStringLiteral("noticeToast"));
        REQUIRE(toast != nullptr);
        auto const children = toast->findChildren<QObject*>(Qt::FindDirectChildrenOnly);
        auto const attached = std::ranges::find_if(
            children, [](QObject const* child) { return child->inherits("QQuickAccessibleAttached"); });
        REQUIRE(attached != children.end());
        CHECK((*attached)->property("role").toInt() == QAccessible::StaticText);
    }
}

TEST_CASE("Main.qml notices: a banner pushes the content down, and shows its text as text (offscreen)",
          "[contour][gui][qml][mainwindow][notices]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);
    auto* const banner = root->findChild<QQuickItem*>(QStringLiteral("noticeBanner"));
    auto const* content = root->findChild<QQuickItem*>(QStringLiteral("content"));
    REQUIRE(banner != nullptr);
    REQUIRE(content != nullptr);
    CHECK_FALSE(banner->isVisible());
    auto const contentTop = content->y();

    auto const markup = QStringLiteral("<b>bold</b> <img src=x>");
    controller.notices().post(contour::window::Notice {
        .kind = contour::window::NoticeKind::ConfigReloadFailed, .text = markup.toStdString() });
    QCoreApplication::processEvents();
    REQUIRE(banner->isVisible());
    CHECK(banner->height() > 0);
    CHECK(content->y() == Catch::Approx(contentTop + banner->height()));

    auto const* text = banner->findChild<QQuickItem*>(QStringLiteral("bannerText"));
    REQUIRE(text != nullptr);
    CHECK(text->property("text").toString() == markup);
    CHECK(text->property("textFormat").toInt() == Qt::PlainText);

    // Dismissed, it gives the room back.
    REQUIRE(QMetaObject::invokeMethod(banner, "dismissed"));
    QCoreApplication::processEvents();
    CHECK(controller.notices().bannerText().isEmpty());
    CHECK(content->y() == Catch::Approx(contentTop));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
// }}}

// {{{ Strong and medium type
TEST_CASE("Main.qml: the host chip's name is bold, and the band's title medium (offscreen)",
          "[contour][gui][qml][mainwindow][type]")
{
    SECTION("the host chip")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.attachedHost = QStringLiteral("home");
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);
        auto* chip = root->findChild<QQuickItem*>(QStringLiteral("hostChip"));
        REQUIRE(chip != nullptr);
        auto const* label = chip->findChild<QObject*>(QStringLiteral("hostChipLabel"));
        REQUIRE(label != nullptr);
        CHECK(label->property("font").value<QFont>().weight() == QFont::Bold);
    }

    SECTION("the band's title")
    {
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 1,
                                                          .tabBarVisibility = TabBarVisibility::Multiple } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        auto& title = named(*root, QStringLiteral("bandTitle"));
        CHECK(title.property("font").value<QFont>().weight() == QFont::Medium);
    }
}
// }}}

// {{{ Secure input: the lock on a tab, or in the band

namespace
{
/// The name @p item announces, from its `Accessible` attached object; empty without one.
[[nodiscard]] QString announcedName(QQuickItem const& item)
{
    for (auto* child: item.findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child->property("name").toString();
    return {};
}
} // namespace

TEST_CASE("a tab whose pane waits for a password carries the lock, and only that tab (offscreen)",
          "[contour][gui][qml][mainwindow][secureinput]")
{
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::Terminal })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        // The active tab and the last one are locked: with the second active, only the second.
        MockMainController controller { MainWindowScene {
            .tabCount = 2, .activeTab = 1, .secure = contour::test::snapshot::SecureDecor::Locked } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto const tabs = tabDelegates(*root);
        REQUIRE(tabs.size() == 2);
        CHECK_FALSE(part(*tabs[0], QStringLiteral("tabLock")).isVisible());
        auto const& lock = part(*tabs[1], QStringLiteral("tabLock"));
        CHECK(lock.isVisible());
        // Before the title, which keeps its room after it; named for assistive technology.
        CHECK(part(*tabs[1], QStringLiteral("tabLabel")).x() >= lock.x() + lock.width());
        CHECK(announcedName(lock) == QStringLiteral("Input hidden"));
        // A tab well is shown, so the band shows no chip.
        CHECK_FALSE(root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("one tab of one pane at a password prompt shows the lock as a band chip (offscreen)",
          "[contour][gui][qml][mainwindow][secureinput]")
{
    // The Pill rows only: they keep the band for its traffic lights when the strip hides. The Flat rows hide
    // the band with the strip (TitleBandRules); the next case gives them their lock.
    for (auto const row: { ChromeStyle::MacOS })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        // tab_bar: multiple hides the strip for one tab, and one pane has no header: the band is left.
        MockMainController controller { MainWindowScene {
            .tabCount = 1,
            .tabBarVisibility = TabBarVisibility::Multiple,
            .secure = contour::test::snapshot::SecureDecor::Locked } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto* chip = root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"));
        REQUIRE(chip != nullptr);
        CHECK(chip->isVisible());
        CHECK(chip->height() == styleGroup(engine, "size").property("toolbarChip").toReal());
        CHECK(announcedName(*chip) == QStringLiteral("Input hidden"));
        // No strip is loaded for one tab under `multiple`: no tab carries a lock.
        CHECK(std::ranges::none_of(root->findChildren<QQuickItem*>(QStringLiteral("tabLock")),
                                   [](QQuickItem const* lock) { return lock->isVisible(); }));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE(
    "one tab of one pane at a password prompt on the joined Fluent row shows the lock on its tab (offscreen)",
    "[contour][gui][qml][mainwindow][secureinput]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple,
                                                      .secure =
                                                          contour::test::snapshot::SecureDecor::Locked } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
    showWindow(*root);

    // The band always holds the tabs on a joined row, one tab being still a tab.
    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 1);
    CHECK(part(*tabs[0], QStringLiteral("tabLock")).isVisible());
    CHECK_FALSE(root->property("secureLockInPane").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE(
    "one tab of one pane at a password prompt, with no band either, shows the lock in the pane (offscreen)",
    "[contour][gui][qml][mainwindow][secureinput][securelock]")
{
    // ContourLinux and Terminal hide the band with the strip: no header, well or band holds the lock. (Fluent
    // is not here: its joined tabs are the title bar, so its band holds the tab, and the tab the lock.)
    for (auto const row: { ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene {
            .tabCount = 1,
            .tabBarVisibility = TabBarVisibility::Multiple,
            .secure = contour::test::snapshot::SecureDecor::Locked } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        CHECK(root->property("secureLockInPane").toBool());
        CHECK_FALSE(root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the in-pane lock is the last resort: a band, a tab well or a header holds it first (offscreen)",
          "[contour][gui][qml][mainwindow][secureinput][securelock]")
{
    SECTION("the Pill row keeps its band chip")
    {
        QQmlEngine engine;
        MockMainController controller { MainWindowScene {
            .tabCount = 1,
            .tabBarVisibility = TabBarVisibility::Multiple,
            .secure = contour::test::snapshot::SecureDecor::Locked } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
        showWindow(*root);
        CHECK_FALSE(root->property("secureLockInPane").toBool());
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible());
    }

    SECTION("a shown tab well holds it on every row")
    {
        for (auto const row:
             { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
        {
            INFO("row: " << contour::config::chromeStyleName(row));
            QQmlEngine engine;
            MockMainController controller { MainWindowScene {
                .tabCount = 2, .activeTab = 1, .secure = contour::test::snapshot::SecureDecor::Locked } };
            contour::test::QmlMessageCapture warnings;
            auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
            showWindow(*root);
            CHECK_FALSE(root->property("secureLockInPane").toBool());
        }
    }
}

TEST_CASE("tabs in a full-screen band that is not on screen do not hold the lock (offscreen)",
          "[contour][gui][qml][mainwindow][secureinput][securelock]")
{
    // Review I1: macOS full screen, two tabs, Pill row. The tabs sit in the band's centre, and the band only
    // appears while the pointer is on the edge: with it away, nothing on screen holds the lock.
    auto scene = MainWindowScene { .tabCount = 2,
                                   .activeTab = 1,
                                   .secure = contour::test::snapshot::SecureDecor::Locked };
    auto band = FullScreenBand { MotionPreference::Reduced, scene };
    CHECK_FALSE(band.root->property("tabsOnScreen").toBool());
    CHECK_FALSE(band.root->property("bandOnScreen").toBool());
    CHECK(band.root->property("secureLockInPane").toBool());
    CHECK_FALSE(band.root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible());

    // The same window, not in full screen: the band rests on screen with the tabs in it.
    band.window->showNormal();
    REQUIRE(QTest::qWaitFor([&] { return band.window->visibility() != QWindow::FullScreen; }, 2000));
    CHECK(band.root->property("tabsOnScreen").toBool());
    CHECK(band.root->property("bandOnScreen").toBool());
    CHECK_FALSE(band.root->property("secureLockInPane").toBool());
}

TEST_CASE("icon-only tabs cannot hold the lock, so it falls to the next host: the band's chip, else the pane "
          "(offscreen)",
          "[contour][gui][qml][mainwindow][secureinput][securelock]")
{
    // A narrow well with many tabs shows icons alone, and an icon-only tab has no room for the lock.
    constexpr auto TabCount = 20;
    for (auto const row: { ChromeStyle::MacOS, ChromeStyle::Fluent })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene {
            .tabCount = TabCount,
            .activeTab = TabCount - 1,
            .secure = contour::test::snapshot::SecureDecor::Locked } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root, 900);
        if (!tabDelegates(*root).back()->property("iconOnly").toBool())
        {
            // A strip with no icon-only form at this width: its tabs hold the lock, so neither the chip
            // nor the pane has to.
            CHECK(root->property("tabsOnScreen").toBool());
            CHECK_FALSE(root->property("secureLockInPane").toBool());
            continue;
        }

        CHECK_FALSE(root->property("tabsOnScreen").toBool());
        // The Pill row's band is on screen and holds the lock as its chip; with no band, the pane does.
        CHECK(root->property("secureLockInPane").toBool() == !root->property("bandOnScreen").toBool());
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible()
              == root->property("bandOnScreen").toBool());
    }
}

TEST_CASE("no pane waiting for a password shows no lock anywhere (offscreen)",
          "[contour][gui][qml][mainwindow][secureinput]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 1,
                                                      .tabBarVisibility = TabBarVisibility::Multiple } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    CHECK_FALSE(root->findChild<QQuickItem*>(QStringLiteral("secureInputChip"))->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
// }}}

// {{{ Keyboard focus in the chrome
namespace
{

/// Asks the window to take keyboard focus, which an offscreen window needs before an item can hold it.
/// @return The window.
QQuickWindow& activated(QObject& root)
{
    auto* window = qobject_cast<QQuickWindow*>(&root);
    REQUIRE(window != nullptr);
    window->requestActivate();
    REQUIRE(QTest::qWaitFor([window] { return window->isActive(); }, 1000));
    return *window;
}

/// The tab ring items of the window's tabs that are drawn, by tab index.
[[nodiscard]] std::vector<int> ringedTabs(QObject& root)
{
    auto rung = std::vector<int> {};
    auto const tabs = tabDelegates(root);
    for (auto const index: std::views::iota(std::size_t { 0 }, tabs.size()))
        if (part(*tabs[index], QStringLiteral("focusRing")).isVisible())
            rung.push_back(static_cast<int>(index));
    return rung;
}

} // namespace

TEST_CASE("keyboard focus draws the ring on the focused tab only (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    struct Row
    {
        ChromeStyle style;
        int outset;
    };
    // The outset per row: 3 on the Mac, 2 on the other native rows, and cells cannot draw one.
    for (auto const& [style, outset]: std::array { Row { ChromeStyle::MacOS, 3 },
                                                   Row { ChromeStyle::Fluent, 2 },
                                                   Row { ChromeStyle::ContourLinux, 2 },
                                                   Row { ChromeStyle::Terminal, 0 } })
    {
        INFO("row: " << contour::config::chromeStyleName(style));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3 } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(style));
        showWindow(*root);
        activated(*root);

        // Nothing is ringed until the chrome has keyboard focus.
        CHECK(ringedTabs(*root).empty());

        controller.focusTabWell(1);
        auto const tabs = tabDelegates(*root);
        REQUIRE(QTest::qWaitFor([&] { return tabs[1]->hasActiveFocus(); }, 1000));

        CHECK(ringedTabs(*root) == std::vector<int> { 1 });
        auto& ring = part(*tabs[1], QStringLiteral("focusRing"));
        auto const& size = styleGroup(engine, "size");
        auto const& colors = styleGroup(engine, "colors");
        CHECK(size.property("focusRing").toReal() == outset);
        CHECK(ring.property("border").value<QObject*>()->property("width").toReal() == outset);
        CHECK(ring.property("color").value<QColor>() == colors.property("focusFill").value<QColor>());
        // Assistive technology hears the focused tab, and only that one.
        auto const* focused = QAccessible::queryAccessibleInterface(tabs[1]);
        REQUIRE(focused != nullptr);
        CHECK(focused->state().focused);
        auto const* unfocused = QAccessible::queryAccessibleInterface(tabs[0]);
        REQUIRE(unfocused != nullptr);
        CHECK_FALSE(unfocused->state().focused);
        CHECK(tabs[1]->property("keyboardFocused").toBool());
        CHECK_FALSE(tabs[0]->property("keyboardFocused").toBool());
        CHECK_FALSE(tabs[2]->property("keyboardFocused").toBool());

        // Leaving the chrome puts the ring away.
        controller.leaveChrome();
        CHECK(ringedTabs(*root).empty());
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("a pointer press shows no ring (offscreen)", "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    auto& window = activated(*root);
    auto const tabs = tabDelegates(*root);
    auto const leaveCalls = [&] {
        return controller.calls.count(QStringLiteral("leaveChrome"));
    };

    // With no chrome focus a click has nothing to end: the controller is not asked.
    QTest::mouseClick(&window, Qt::LeftButton, {}, tabs[2]->mapToScene(QPointF(4, 4)).toPoint());
    CHECK(leaveCalls() == 0);
    CHECK(ringedTabs(*root).empty());

    // With it, the click ends it, and no tab is left ringed.
    controller.focusTabWell(1);
    REQUIRE(QTest::qWaitFor([&] { return tabs[1]->hasActiveFocus(); }, 1000));
    REQUIRE(ringedTabs(*root) == std::vector<int> { 1 });
    QTest::mouseClick(&window, Qt::LeftButton, {}, tabs[2]->mapToScene(QPointF(4, 4)).toPoint());
    CHECK(leaveCalls() == 1);
    CHECK(ringedTabs(*root).empty());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("keys in the tab well reach the controller (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    auto& window = activated(*root);
    controller.focusTabWell(1);
    auto const tabs = tabDelegates(*root);
    REQUIRE(QTest::qWaitFor([&] { return tabs[1]->hasActiveFocus(); }, 1000));

    // A key the controller handles is accepted, so it goes no further.
    auto handled = QKeyEvent { QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier };
    QCoreApplication::sendEvent(&window, &handled);
    REQUIRE(controller.chromeKeys.size() == 1);
    CHECK(controller.chromeKeys.front() == std::pair { static_cast<int>(Qt::Key_Right), 0 });
    CHECK(handled.isAccepted());

    // One it does not handle is not accepted: it goes on, up through the title bar, which offers it again.
    controller.chromeKeysHandled = false;
    auto unhandled = QKeyEvent { QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier };
    QCoreApplication::sendEvent(&window, &unhandled);
    REQUIRE(controller.chromeKeys.size() > 1);
    CHECK(std::ranges::all_of(std::views::drop(controller.chromeKeys, 1),
                              [](auto const& key) { return key.first == Qt::Key_Home; }));
    CHECK_FALSE(unhandled.isAccepted());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("closing a busy tab asks first, and Cancel keeps it (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3 } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    activated(*root);
    auto* dialog = root->findChild<QObject*>(QStringLiteral("closeTabConfirmation"));
    REQUIRE(dialog != nullptr);
    CHECK_FALSE(dialog->property("opened").toBool());

    SECTION("a named program")
    {
        emit controller.closeTabConfirmationRequested(41, QStringLiteral("build"), QStringLiteral("make"));
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
        CHECK(dialog->property("heading").toString() == QStringLiteral("Close “build”?"));
        CHECK(dialog->property("message").toString()
              == QStringLiteral("“make” is still running in this tab."));
        CHECK(dialog->property("acceptText").toString() == QStringLiteral("Close"));
        CHECK(dialog->property("rejectText").toString() == QStringLiteral("Cancel"));
        // Cancel is what Enter takes, and Close is the destructive one.
        CHECK(dialog->property("defaultButton").toInt() == 1);
        // Both buttons are named, for a screen reader.
        for (auto const& [part, label]: std::array { std::pair { "kitDialogAccept", "Close" },
                                                     std::pair { "kitDialogReject", "Cancel" } })
        {
            auto* button = dialog->findChild<QQuickItem*>(QString::fromLatin1(part));
            REQUIRE(button != nullptr);
            auto const* accessible = QAccessible::queryAccessibleInterface(button);
            REQUIRE(accessible != nullptr);
            CHECK(accessible->text(QAccessible::Name) == QString::fromLatin1(label));
        }
        CHECK(dialog->property("acceptRole").toInt()
              == chromeStyleOf(engine).property("statusDanger").toInt());

        REQUIRE(QMetaObject::invokeMethod(dialog, "reject"));
        REQUIRE(QTest::qWaitFor([&] { return !dialog->property("opened").toBool(); }, 2000));
        CHECK(controller.calls.filter(QStringLiteral("confirmCloseTab")).isEmpty());

        emit controller.closeTabConfirmationRequested(41, QStringLiteral("build"), QStringLiteral("make"));
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
        REQUIRE(QMetaObject::invokeMethod(dialog, "accept"));
        CHECK(controller.calls.filter(QStringLiteral("confirmCloseTab"))
              == QStringList { "confirmCloseTab:41" });
    }

    SECTION("the tab goes while the question is open")
    {
        emit controller.closeTabConfirmationRequested(41, QStringLiteral("build"), QStringLiteral("make"));
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));

        // Another tab going leaves the question be; the tab's own going withdraws it, and Close is not
        // offered.
        emit controller.closeTabConfirmationWithdrawn(40);
        QCoreApplication::processEvents();
        CHECK(dialog->property("opened").toBool());
        emit controller.closeTabConfirmationWithdrawn(41);
        REQUIRE(QTest::qWaitFor([&] { return !dialog->property("opened").toBool(); }, 2000));
        CHECK(controller.calls.filter(QStringLiteral("confirmCloseTab")).isEmpty());
    }

    SECTION("a program that cannot be known")
    {
        emit controller.closeTabConfirmationRequested(42, QStringLiteral("shell"), QString {});
        REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
        CHECK(dialog->property("message").toString()
              == QStringLiteral("A program may still be running in this tab."));
        REQUIRE(QMetaObject::invokeMethod(dialog, "accept"));
        CHECK(controller.calls.filter(QStringLiteral("confirmCloseTab"))
              == QStringList { "confirmCloseTab:42" });
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Cancel on the close question gives the keyboard back to the tab asked about (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    auto scene = palette.scene();
    scene.tabCount = 3;
    MockMainController controller { scene };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    static_cast<void>(activeMainWindow(*root));
    auto const tabs = tabDelegates(*root);
    auto* dialog = root->findChild<QObject*>(QStringLiteral("closeTabConfirmation"));
    REQUIRE(dialog != nullptr);

    controller.focusTabWell(1);
    REQUIRE(QTest::qWaitFor([&] { return tabs[1]->hasActiveFocus(); }, 1000));
    emit controller.closeTabConfirmationRequested(41, QStringLiteral("build"), QStringLiteral("make"));
    REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));

    // Cancel. The chrome still holds focus on that tab, so that is where the keys must go.
    REQUIRE(QMetaObject::invokeMethod(dialog, "reject"));
    REQUIRE(QTest::qWaitFor([&] { return !dialog->property("opened").toBool(); }, 2000));
    CHECK(controller.calls.filter(QStringLiteral("confirmCloseTab")).isEmpty());
    CHECK(controller.focusedTabIndex() == 1);
    CHECK(tabs[1]->hasActiveFocus());
    CHECK(ringedTabs(*root) == std::vector<int> { 1 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the window reports what its chrome shows, and the toolbar's items wear the ring (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MockMainController controller { MainWindowScene { .tabCount = 3,
                                                      .hostLabel = QStringLiteral("home"),
                                                      .drift =
                                                          contour::test::snapshot::SnapshotDrift::Shown } };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    showWindow(*root);
    activated(*root);

    // The tab well, and three toolbar items in visual order: the host chip, the drift chip, the new-tab
    // button.
    CHECK(controller.reportedTabsShown);
    CHECK(controller.reportedToolbarItems == 3);

    auto const ringOf = [&root](char const* itemName) -> QQuickItem& {
        auto& item = named(*root, QString::fromLatin1(itemName));
        return part(item, QStringLiteral("focusRing"));
    };
    auto const items = std::array { "hostChip", "daemonDriftBadge", "newTabButton" };
    for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
    {
        INFO("toolbar item: " << items[index]);
        controller.focusToolbarItem(static_cast<int>(index));
        auto& item = named(*root, QString::fromLatin1(items[index]));
        REQUIRE(QTest::qWaitFor([&] { return item.hasActiveFocus(); }, 1000));
        CHECK(ringOf(items[index]).isVisible());
        for (auto const other: std::views::iota(std::size_t { 0 }, items.size()))
            if (other != index)
                CHECK_FALSE(ringOf(items[other]).isVisible());
    }

    // Hiding the tab strip is reported: the controller then takes focus out of a well that is gone.
    controller.setTabBarShouldShow(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(controller.reportedTabsShown);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{

/// The pane tree Main.qml's terminal content is, which a key typed into the terminal lands in.
[[nodiscard]] QQuickItem* paneTreeOf(QObject& root)
{
    auto const items = root.findChildren<QQuickItem*>();
    auto const tree = std::ranges::find_if(items, [](QQuickItem const* item) {
        return QByteArrayView(item->metaObject()->className()).startsWith("PaneNode");
    });
    return tree != items.end() ? *tree : nullptr;
}

} // namespace

TEST_CASE("moving a tab with the keyboard keeps the chrome focus, and Escape gives it back (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    auto scene = palette.scene();
    scene.tabCount = 3;
    MockMainController controller { scene };
    controller.mirrorsChromeKeys = true;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    auto& window = activeMainWindow(*root);
    auto const tabs = tabDelegates(*root);
    auto* paneTree = paneTreeOf(*root);
    REQUIRE(paneTree != nullptr);

    // Ctrl+F6 puts the focus on the active tab; Alt+Right moves that tab one place and the model announces
    // it.
    controller.focusTabWell(0);
    REQUIRE(QTest::qWaitFor([&] { return tabs[0]->hasActiveFocus(); }, 1000));
    QTest::keyClick(&window, Qt::Key_Right, Qt::AltModifier);
    QCoreApplication::processEvents();

    // The moved tab still holds the chrome focus, and the ring: the announcement was not the user leaving.
    CHECK(controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Tabs));
    CHECK(controller.focusedTabIndex() == 1);
    CHECK(controller.calls.filter(QStringLiteral("leaveChrome")).isEmpty());
    REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[1]->hasActiveFocus(); }, 1000));
    CHECK(ringedTabs(*root) == std::vector<int> { 1 });

    // So it can move again, and Escape ends the chrome focus.
    QTest::keyClick(&window, Qt::Key_Right, Qt::AltModifier);
    QCoreApplication::processEvents();
    CHECK(controller.focusedTabIndex() == 2);
    QTest::keyClick(&window, Qt::Key_Escape);
    QCoreApplication::processEvents();
    CHECK(controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Terminal));
    CHECK(ringedTabs(*root).empty());
    CHECK(isWithin(window.activeFocusItem(), paneTree));

    // A key typed now is the terminal's: the chrome is not offered it.
    auto const offered = controller.chromeKeys.size();
    QTest::keyClick(&window, Qt::Key_A);
    CHECK(controller.chromeKeys.size() == offered);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a tab's ring reaches 3:1 on what it lies over (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus][contrast]")
{
    struct Fill
    {
        char const* name;
        TabDecor decor; ///< Plain: row 0 has no colour; EveryBadge: row 0 wears one.
        int activeTab;  ///< 0 draws row 0 active (OS accent or the active coloured fill), 1 at rest.
    };
    for (auto const row: { ChromeStyle::Fluent, ChromeStyle::ContourLinux })
        for (auto const appearance: contour::test::snapshot::SnapshotAppearances)
            for (auto const& fill: { Fill { "active, accent", TabDecor::Plain, 0 },
                                     Fill { "active, coloured", TabDecor::EveryBadge, 0 },
                                     Fill { "at rest, coloured", TabDecor::EveryBadge, 1 } })
            {
                INFO("row: " << contour::config::chromeStyleName(row)
                             << " appearance: " << contour::test::snapshot::appearanceName(appearance)
                             << " fill: " << fill.name);
                auto const scoped = contour::test::ScopedChromeAppearance { appearance };
                QQmlEngine engine;
                MockMainController controller { MainWindowScene {
                    .tabCount = 2, .decor = fill.decor, .activeTab = fill.activeTab } };
                contour::test::QmlMessageCapture warnings;
                auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
                showWindow(*root);
                activated(*root);

                controller.focusTabWell(0);
                auto const tabs = tabDelegates(*root);
                REQUIRE(QTest::qWaitFor([&] { return tabs[0]->hasActiveFocus(); }, 1000));

                // A Flat or Joined tab's ring is drawn inside it, over the fill the tab paints on the window;
                // a pill's lies outside the pill, over the window itself.
                auto const window =
                    evaluateIn(*tabs[0], QStringLiteral("systemPalette.window")).value<QColor>();
                auto const ringsOutside =
                    contour::config::ChromeTokenRows.at(static_cast<size_t>(row)).layout.tabShape
                    == contour::config::TabShape::Pill;
                auto const painted = ringsOutside ? window
                                                  : over(part(*tabs[0], QStringLiteral("tabBackground"))
                                                             .property("color")
                                                             .value<QColor>(),
                                                         window);
                auto& ring = part(*tabs[0], QStringLiteral("focusRing"));
                auto const ringColor =
                    ring.property("border").value<QObject*>()->property("color").value<QColor>();
                CHECK(contrastOf(ringColor, painted) >= contour::config::MinimumMarkContrast);
                CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
            }
}

TEST_CASE("closing another tab does not take the focus out of the chrome (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    auto scene = palette.scene();
    scene.tabCount = 3;
    MockMainController controller { scene };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    [[maybe_unused]] auto& window = activeMainWindow(*root);
    auto const tabs = tabDelegates(*root);

    controller.focusTabWell(0);
    REQUIRE(QTest::qWaitFor([&] { return tabs[0]->hasActiveFocus(); }, 1000));

    // A tab closes in the background (its shell exited): the model announces the active row.
    controller.closeTabAtIndex(2);
    QCoreApplication::processEvents();

    CHECK(controller.calls.filter(QStringLiteral("leaveChrome")).isEmpty());
    CHECK(controller.focusedTabIndex() == 0);
    CHECK(tabs[0]->hasActiveFocus());
    CHECK(ringedTabs(*root) == std::vector<int> { 0 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the keyboard stays on its tab when rows move around it (offscreen)",
          "[contour][gui][qml][mainwindow][chromefocus]")
{
    QQmlEngine engine;
    MainWindowPalette palette;
    auto scene = palette.scene();
    scene.tabCount = 3;
    MockMainController controller { scene };
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::MacOS));
    [[maybe_unused]] auto& window = activeMainWindow(*root);

    controller.focusTabWell(2);
    auto const focusedId = controller.focusedTabId();
    REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[2]->hasActiveFocus(); }, 1000));

    SECTION("a tab is inserted before it")
    {
        controller.insertTab(0, MockMainController::TabActivation::Keep);
        QCoreApplication::processEvents();

        // The focus names the same tab, now on row 3, and the item holding the keyboard is that tab's.
        CHECK(controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Tabs));
        CHECK(controller.focusedTabId() == focusedId);
        CHECK(controller.focusedTabIndex() == 3);
        REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[3]->hasActiveFocus(); }, 1000));
        CHECK(ringedTabs(*root) == std::vector<int> { 3 });
    }

    SECTION("a tab moves in from another window and is activated")
    {
        // The model activates a tab it takes in, which moves the keyboard to it.
        auto const incoming = controller.insertTab(0, MockMainController::TabActivation::Activate);
        QCoreApplication::processEvents();

        CHECK(controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Tabs));
        CHECK(controller.focusedTabId() == incoming);
        CHECK(controller.focusedTabIndex() == 0);
        REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[0]->hasActiveFocus(); }, 1000));
        CHECK(ringedTabs(*root) == std::vector<int> { 0 });
    }

    SECTION("a tab is activated from outside the tab well")
    {
        controller.setActiveTab(1);
        QCoreApplication::processEvents();

        CHECK(controller.focusedTabIndex() == 1);
        REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[1]->hasActiveFocus(); }, 1000));
        CHECK(ringedTabs(*root) == std::vector<int> { 1 });
    }

    SECTION("a tab before it closes")
    {
        controller.closeTabAtIndex(0);
        QCoreApplication::processEvents();

        CHECK(controller.chromeFocusArea() == static_cast<int>(contour::window::ChromeArea::Tabs));
        CHECK(controller.focusedTabId() == focusedId);
        CHECK(controller.focusedTabIndex() == 1);
        REQUIRE(QTest::qWaitFor([&] { return tabDelegates(*root)[1]->hasActiveFocus(); }, 1000));
        CHECK(ringedTabs(*root) == std::vector<int> { 1 });
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
// }}}

TEST_CASE("Main.qml takes every pane or tab dropped on it, and nothing else (offscreen)",
          "[contour][gui][qml][mainwindow][drop]")
{
    // A drop no pane, tab or gap took is still taken by the window, as a no-op: the drag then ends
    // MoveAction, so IgnoreAction means only a drop outside every window, or a cancel.
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);
    auto* const window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    auto const* const catchAll = root->findChild<QQuickItem*>(QStringLiteral("windowDropCatchAll"));
    REQUIRE(catchAll != nullptr);
    // Below everything else the window shows: the first of its content's children.
    REQUIRE(catchAll->parentItem() != nullptr);
    CHECK(catchAll->parentItem()->childItems().front() == catchAll);
    // The mock's window is never sized from cell metrics, nor shown; give it a size to drop into.
    window->resize(800, 600);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window));
    REQUIRE(catchAll->width() > 0);

    auto const at = QPoint { window->width() / 2, window->height() / 2 };
    auto const dropOf = [&](QString const& format) {
        auto mime = QMimeData {};
        mime.setData(format, QByteArrayLiteral("{}"));
        auto enter = QDragEnterEvent { at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier };
        QCoreApplication::sendEvent(window, &enter);
        auto move = QDragMoveEvent { at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier };
        QCoreApplication::sendEvent(window, &move);
        auto drop = QDropEvent { QPointF { at }, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier };
        QCoreApplication::sendEvent(window, &drop);
        return drop.isAccepted() && drop.dropAction() == Qt::MoveAction;
    };
    CHECK(dropOf(QString::fromLatin1(contour::window::NodeDragMime)));
    // The strip's old payload is gone: nothing writes it any more, so nothing takes it.
    CHECK_FALSE(dropOf(QStringLiteral("application/x-contour-tab")));
    CHECK_FALSE(dropOf(QStringLiteral("application/x-something-else")));
}
