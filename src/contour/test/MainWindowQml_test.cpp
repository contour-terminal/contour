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

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtGui/QWindow>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vtworkspace/PaneLayout.hpp>

using contour::test::loadMainWindow;
using contour::test::MockMainController;

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
    //    effective height (34 logical px, its implicitHeight).
    CHECK(controller.chromeHeight() == 34);
}

TEST_CASE("Main.qml tab strip: visibility gate collapses the chrome when hidden (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // When win.tabBarShouldShow is false (tab_bar_visibility: Never, or Multiple with a single tab),
    // the TitleBar collapses: its effectiveHeight -> 0 -> the declared chromeHeight -> 0, and the strip
    // item is not visible. When true, the strip occupies its 34px implicitHeight (as the startup test
    // already pins). Driving the mock property proves the real Main.qml binding reacts.
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

        CHECK(controller.chromeHeight() == 34);
        auto* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        CHECK(titleBar->isVisible());
    }
}

TEST_CASE("Main.qml tab strip: position flips the title-bar / content stacking (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // The strip is pinned to the top OR bottom edge via conditional anchors; the content area fills the
    // opposite side. chromeHeight is position-invariant (34px either way). We read the laid-out y/height
    // off the live items to prove the stacking flipped.
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
        CHECK(controller.chromeHeight() == 34);
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
        CHECK(controller.chromeHeight() == 34);
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
QString const HostingNotice =
    QStringLiteral("Contour could not reach its daemon (refused). This window runs its "
                   "shells itself; they end when it closes.");
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

    controller.setHostingNotice(HostingNotice);
    QCoreApplication::processEvents();
    CHECK(bar->isVisible());
    CHECK(bar->height() > 0);
    auto const* text = bar->findChild<QObject*>(QStringLiteral("bannerText"));
    REQUIRE(text != nullptr);
    CHECK(text->property("text").toString() == HostingNotice);
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

    controller.setHostingNotice(HostingNotice);
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

    controller.setHostingNotice(HostingNotice);
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

    controller.setHostingNotice(HostingNotice);
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
        // The row's host decides its window controls: traffic lights on the leading edge on macOS,
        // buttons on the trailing edge everywhere else.
        auto const* controls = engine.rootContext()->contextProperty("windowControls").value<QObject*>();
        REQUIRE(controls != nullptr);
        auto const isMac = host == contour::config::HostPlatform::MacOS;
        CHECK(controls->property("presentation").toString() == (isMac ? "trafficLight" : "button"));
        CHECK(controls->property("side").toString() == (isMac ? "leading" : "trailing"));
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

TEST_CASE("the Fluent and ContourLinux rows keep today's geometry (offscreen)",
          "[contour][gui][qml][mainwindow][tabwell]")
{
    for (auto const row: { ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        QQmlEngine engine;
        MockMainController controller { MainWindowScene { .tabCount = 3, .decor = TabDecor::EveryBadge } };
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(row));
        showWindow(*root);

        auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        CHECK(titleBar->height() == 34); // today's chromeHeightUnits

        auto const tabs = tabDelegates(*root);
        REQUIRE(tabs.size() == 3);
        auto& first = *tabs[0];              // coloured, active, no badge
        CHECK(first.implicitHeight() == 32); // today's tabHeightUnits
        CHECK(first.height() == titleBar->height());
        auto const labelWidth = part(first, QStringLiteral("tabLabel")).implicitWidth();
        // Today's sum, snapped up to the whole pixel so the label never loses a fraction of one.
        CHECK(first.implicitWidth() == std::ceil(std::clamp(labelWidth + 56, 120.0, 240.0)));

        auto const* well = root->findChild<QQuickItem*>(QStringLiteral("tabWell"));
        CHECK((well == nullptr || !well->isVisible()));
        auto const* newTab = root->findChild<QQuickItem*>(QStringLiteral("newTabButton"));
        REQUIRE(newTab != nullptr);
        CHECK(newTab->width() == 32);
        auto const* newTabMenu = root->findChild<QQuickItem*>(QStringLiteral("newTabMenuButton"));
        REQUIRE(newTabMenu != nullptr);
        CHECK(newTabMenu->isVisible());
        CHECK(part(*tabs[1], QStringLiteral("tabCloseButton")).isVisible());

        // A coloured tab is filled WT-style, as it always was.
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        auto const expected = controller.tabBackgroundColor(
            contour::platform::toQColor(contour::test::DecorTabColor),
            QGuiApplication::palette().color(QPalette::Active, QPalette::Window),
            true,
            false,
            window->isActive());
        CHECK(part(first, QStringLiteral("tabBackground")).property("color").value<QColor>() == expected);
        CHECK(part(first, QStringLiteral("tabColorCell")).property("text").toString().isEmpty());

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
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

    auto* strip = root->findChild<QQuickItem*>(QStringLiteral("tabStrip"));
    auto* list = root->findChild<QQuickItem*>(QStringLiteral("tabList"));
    REQUIRE(strip != nullptr);
    REQUIRE(list != nullptr);
    auto* area = list->parentItem(); // the DropArea's coordinates
    REQUIRE(area != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return list->property("contentX").toReal() > 0; }, 1000)); // scrolled

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
        // A quarter into the tab is before its midpoint: the slot before it.
        auto const pointer = tab->mapToItem(area, QPointF(tab->width() / 4, tab->height() / 2));
        auto slot = QVariant {};
        REQUIRE(QMetaObject::invokeMethod(
            strip, "dropIndexFor", Q_RETURN_ARG(QVariant, slot), Q_ARG(QVariant, pointer.x())));
        CHECK(slot.toInt() == index);
        // The caret for that slot stands at the tab's left edge, inside the visible well.
        auto caret = QVariant {};
        REQUIRE(QMetaObject::invokeMethod(
            strip, "caretX", Q_RETURN_ARG(QVariant, caret), Q_ARG(QVariant, index)));
        CHECK(caret.toReal() == Catch::Approx(tab->mapToItem(area, QPointF(0, 0)).x()));
        CHECK(caret.toReal() >= list->x());
        CHECK(caret.toReal() <= list->x() + list->width());
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
    MockMainController controller { MainWindowScene { .tabCount = 6, .decor = TabDecor::EveryBadge } };
    contour::test::QmlMessageCapture warnings;
    auto const scheme = bundledScheme("contour-light");
    auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Terminal, scheme));
    showWindow(*root);

    auto const* titleBar = root->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(titleBar != nullptr);
    CHECK(titleBar->height() == chromeStyleOf(engine).property("cellHeight").toReal());

    auto const tabs = tabDelegates(*root);
    REQUIRE(tabs.size() == 6);
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

    auto const glyphs = std::array { QStringLiteral("•"),
                                     QStringLiteral("◑"),
                                     QStringLiteral("!"),
                                     QStringLiteral("✓"),
                                     QStringLiteral("✕") };
    static_assert(glyphs.size() == contour::test::DecorBadges.size());
    auto const well = colors.property("tabWellFill").value<QColor>();
    auto const bar = scheme.bySlot[static_cast<size_t>(SchemeSlot::Bar)];
    auto const floor =
        std::min(contour::config::MinimumTextContrast,
                 vtbackend::contrastRatio(scheme.bySlot[static_cast<size_t>(SchemeSlot::BarText)], bar));
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

TEST_CASE("with one tab under Multiple, Fluent and Linux show no bar, as today (offscreen)",
          "[contour][gui][qml][mainwindow][band]")
{
    for (auto const row: { ChromeStyle::Fluent, ChromeStyle::ContourLinux })
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
        MockMainController controller { MainWindowScene { .tabCount = 6,
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
        CHECK(tabs == 6);
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
    // home: a remote window whose connection is not up shows it anyway (R-d), chip and all.
    using contour::window::HostChipState;
    for (auto const row: { ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
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
        auto root = loadMainWindow(engine, controller, warnings, rowSetup(ChromeStyle::Fluent));
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
