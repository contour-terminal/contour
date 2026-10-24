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

#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

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

TEST_CASE("Main.qml remote chrome: a window attached to a host names it (offscreen)",
          "[contour][gui][qml][mainwindow][window][hosts]")
{
    SECTION("a remote window")
    {
        QQmlEngine engine;
        MockMainController controller;
        controller.attachedHost = QStringLiteral("home");
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        // No session has a title yet, so the title is the host alone.
        CHECK(root->property("title").toString() == QStringLiteral("home"));
        auto const* badge = root->findChild<QQuickItem*>(QStringLiteral("hostBadge"));
        REQUIRE(badge != nullptr);
        CHECK(badge->isVisible());
        auto const* text = root->findChild<QObject*>(QStringLiteral("hostBadgeText"));
        REQUIRE(text != nullptr);
        CHECK(text->property("text").toString() == QStringLiteral("home"));
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }

    SECTION("a local window")
    {
        QQmlEngine engine;
        MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto root = loadMainWindow(engine, controller, warnings);

        CHECK(root->property("title").toString().isEmpty());
        auto const* badge = root->findChild<QQuickItem*>(QStringLiteral("hostBadge"));
        REQUIRE(badge != nullptr);
        CHECK_FALSE(badge->isVisible());
        CHECK(badge->width() == 0);
    }
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
