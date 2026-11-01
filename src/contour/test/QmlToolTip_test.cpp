// SPDX-License-Identifier: Apache-2.0
//
// The kit's tooltip: its shortcut slot, plain multi-line text, warm and cold delay, placement, and the
// controls that carry one (the title band's new-tab button, the find bar's buttons).

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

/// A window with a control-sized item at @p controlY and one KitToolTip beneath it, as a control hosts it.
struct ToolTipHost
{
    QmlHost window;
    QQuickItem* control = nullptr;
    QObject* toolTip = nullptr;
    QObject* clock = nullptr;

    [[nodiscard]] QQuickWindow& quick() const { return *qobject_cast<QQuickWindow*>(window.get()); }
};

/// Builds the host in the row of @p setup. The window is 400 x 200, the control 40 x 20 at (@p controlX,
/// @p controlY), and the tooltip a child of it, wanting nothing until a test says so.
[[nodiscard]] ToolTipHost openToolTipHost(QQmlEngine& engine,
                                          contour::test::ChromeSetup const& setup,
                                          int controlX = 100,
                                          int controlY = 50)
{
    contour::test::installChromeStyle(engine, setup);
    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: 400; height: 200; visible: true\n"
                                       "  property alias clock: clockObject\n"
                                       "  KitToolTipClock { id: clockObject }\n"
                                       "  Item {\n"
                                       "    objectName: \"control\"\n"
                                       "    x: %1; y: %2; width: 40; height: 20\n"
                                       "    KitToolTip {\n"
                                       "      objectName: \"tip\"\n"
                                       "      text: \"New tab\"\n"
                                       "    }\n"
                                       "  }\n"
                                       "}\n")
                            .arg(controlX)
                            .arg(controlY);
    auto result =
        ToolTipHost { .window = buildQmlHost(engine, source, QStringLiteral("ToolTipTestHost.qml")) };
    result.control = result.window->findChild<QQuickItem*>(QStringLiteral("control"));
    result.toolTip = result.window->findChild<QObject*>(QStringLiteral("tip"));
    result.clock = result.window->property("clock").value<QObject*>();
    REQUIRE(result.control != nullptr);
    REQUIRE(result.toolTip != nullptr);
    REQUIRE(result.clock != nullptr);
    return result;
}

[[nodiscard]] contour::test::ChromeSetup rowSetup(contour::config::ChromeStyle row)
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = row;
    return setup;
}

/// @return The chrome colour role @p name of @p engine's chromeStyle.
[[nodiscard]] QColor chromeColorOf(QQmlEngine& engine, char const* name)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    return style->property("colors").value<QObject*>()->property(name).value<QColor>();
}

/// @return The item named @p name inside @p tip's content.
[[nodiscard]] QQuickItem* partOf(QObject* tip, char const* name)
{
    return tip->findChild<QQuickItem*>(QString::fromLatin1(name));
}

} // namespace

TEST_CASE("KitToolTip: the shortcut slot exists only when there is a shortcut (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* label = partOf(host.toolTip, "kitToolTipLabel");
    auto* shortcut = partOf(host.toolTip, "kitToolTipShortcut");
    auto* content = partOf(host.toolTip, "kitToolTipContent");
    REQUIRE(label != nullptr);
    REQUIRE(shortcut != nullptr);
    REQUIRE(content != nullptr);

    CHECK_FALSE(host.toolTip->property("shortcutShown").toBool());
    auto const bare = content->implicitWidth();
    CHECK(bare == std::ceil(label->implicitWidth()));

    host.toolTip->setProperty("shortcut", QStringLiteral("⌘T"));
    CHECK(host.toolTip->property("shortcutShown").toBool());
    CHECK(shortcut->property("text").toString() == QStringLiteral("⌘T"));
    auto const gap = chromeToken(engine, "size", "tooltipGap");
    CHECK(content->implicitWidth() == bare + gap + std::ceil(shortcut->implicitWidth()));

    host.toolTip->setProperty("shortcut", QString {});
    CHECK_FALSE(host.toolTip->property("shortcutShown").toBool());
    CHECK(content->implicitWidth() == bare);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: the text is plain, its first line primary and the rest secondary (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* label = partOf(host.toolTip, "kitToolTipLabel");
    auto* secondary = partOf(host.toolTip, "kitToolTipSecondary");
    auto* shortcut = partOf(host.toolTip, "kitToolTipShortcut");
    REQUIRE(label != nullptr);
    REQUIRE(secondary != nullptr);
    REQUIRE(shortcut != nullptr);

    host.toolTip->setProperty("text",
                              QStringLiteral("<b>Blocked</b>\n<img src=\"file:///etc/passwd\">\n/tmp"));
    CHECK(label->property("text").toString() == QStringLiteral("<b>Blocked</b>"));
    CHECK(secondary->property("text").toString() == QStringLiteral("<img src=\"file:///etc/passwd\">\n/tmp"));
    // 0 = Text.PlainText: markup is shown as the characters it is.
    CHECK(label->property("textFormat").toInt() == 0);
    CHECK(secondary->property("textFormat").toInt() == 0);
    CHECK(shortcut->property("textFormat").toInt() == 0);

    CHECK(host.toolTip->property("secondaryShown").toBool());
    auto const colorOf = [](QQuickItem* item) {
        return item->property("color").value<QColor>().name(QColor::HexArgb);
    };
    CHECK(colorOf(label) == chromeColorOf(engine, "tooltipText").name(QColor::HexArgb));
    CHECK(colorOf(secondary) == chromeColorOf(engine, "tooltipTextSecondary").name(QColor::HexArgb));
    CHECK(colorOf(label) != colorOf(secondary));

    host.toolTip->setProperty("text", QStringLiteral("one line"));
    CHECK_FALSE(host.toolTip->property("secondaryShown").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: every row paints its box from the tooltip tokens (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    using contour::config::ChromeStyle;
    contour::test::QmlMessageCapture const warnings;
    for (auto const row:
         { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal })
    {
        CAPTURE(static_cast<int>(row));
        QQmlEngine engine;
        auto host = openToolTipHost(engine, rowSetup(row));
        auto* background = host.toolTip->property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        CHECK(background->property("radius").toReal() == chromeToken(engine, "radii", "tooltip"));
        CHECK(background->property("color").value<QColor>().name(QColor::HexArgb)
              == chromeColorOf(engine, "tooltipFill").name(QColor::HexArgb));
        CHECK(host.toolTip->property("leftPadding").toReal() == chromeToken(engine, "size", "tooltipPadX"));
        CHECK(host.toolTip->property("topPadding").toReal() == chromeToken(engine, "size", "tooltipPadTop"));
        CHECK(host.toolTip->property("bottomPadding").toReal()
              == chromeToken(engine, "size", "tooltipPadBottom"));
        // A hairline only where the row has an edge colour.
        auto const hasEdge = chromeColorOf(engine, "tooltipEdge").alpha() > 0;
        CHECK((background->property("border").value<QObject*>()->property("width").toReal() > 0) == hasEdge);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTipClock: warm for the token's span after a tooltip (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    CHECK(host.clock->property("warmMs").toInt() == 1500);

    auto const isWarm = [&] {
        QVariant answer;
        REQUIRE(QMetaObject::invokeMethod(host.clock, "isWarm", Q_RETURN_ARG(QVariant, answer)));
        return answer.toBool();
    };
    CHECK_FALSE(isWarm());
    REQUIRE(QMetaObject::invokeMethod(host.clock, "touch"));
    CHECK(isWarm());
    host.clock->setProperty("lastActive", static_cast<double>(QDateTime::currentMSecsSinceEpoch() - 5000));
    CHECK_FALSE(isWarm());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: the first tooltip waits the token's delay, a warm one shows at once (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    // No ThemedWindow here, so the tooltip keeps a clock of its own: cold on its own account.
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto const delay = static_cast<int>(chromeToken(engine, "motion", "tooltipDelay"));
    REQUIRE(delay == 600);

    host.toolTip->setProperty("wanted", true);
    CHECK(host.toolTip->property("delay").toInt() == delay);
    CHECK_FALSE(host.toolTip->property("visible").toBool());

    host.toolTip->setProperty("wanted", false);
    CHECK_FALSE(host.toolTip->property("visible").toBool());

    // The tooltip that showed has warmed its own clock: the next one has no delay.
    auto* own = host.toolTip->property("_clock").value<QObject*>();
    REQUIRE(own != nullptr);
    REQUIRE(QMetaObject::invokeMethod(own, "touch"));
    host.toolTip->setProperty("wanted", true);
    CHECK(host.toolTip->property("delay").toInt() == 0);
    CHECK(QTest::qWaitFor([&] { return host.toolTip->property("visible").toBool(); }, 2000));

    host.toolTip->setProperty("wanted", false);
    CHECK(QTest::qWaitFor([&] { return !host.toolTip->property("visible").toBool(); }, 2000));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

/// Shows @p host's tooltip at once (its clock warm) and waits until it is open.
void showToolTip(ToolTipHost& host)
{
    auto* const own = host.toolTip->property("_clock").value<QObject*>();
    REQUIRE(own != nullptr);
    REQUIRE(QMetaObject::invokeMethod(own, "touch"));
    host.toolTip->setProperty("wanted", true);
    REQUIRE(QTest::qWaitFor([&] { return host.toolTip->property("opened").toBool(); }, 3000));
}

TEST_CASE("KitToolTip: opens below its control at the pointer, never covering it (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* const tip = host.toolTip;

    SECTION("below, the pointer inside its leading edge")
    {
        tip->setProperty("pointerX", 10.0);
        showToolTip(host);
        CHECK(tip->property("y").toReal() >= host.control->height());
        CHECK(tip->property("x").toReal() == 10.0 - tip->property("pointerInset").toReal());
    }

    SECTION("clamped inside the window's right edge")
    {
        host.control->setX(380);
        tip->setProperty("shortcut", QStringLiteral("Ctrl+Shift+T"));
        tip->setProperty("pointerX", 30.0);
        showToolTip(host);
        auto const right = host.control->x() + tip->property("x").toReal() + tip->property("width").toReal();
        CHECK(right <= host.quick().width() - tip->property("windowMargin").toReal());
        CHECK(host.control->x() + tip->property("x").toReal() >= tip->property("windowMargin").toReal());
    }

    SECTION("flips above only where there is no room below")
    {
        host.control->setY(host.quick().height() - 20);
        showToolTip(host);
        CHECK(tip->property("y").toReal() + tip->property("height").toReal() <= 0.0);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: its shortcut is asked of the controller when about to show (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* const tip = host.toolTip;
    tip->setProperty("shortcutCommand", QStringLiteral("CreateNewTab"));
    tip->setProperty("shortcutSource", QVariant::fromValue(static_cast<QObject*>(&controller)));

    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shortcut").toString().isEmpty());
    CHECK_FALSE(tip->property("shortcutShown").toBool());

    controller.boundChords.insert(QStringLiteral("CreateNewTab"), QStringLiteral("Ctrl+Shift+T"));
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shortcut").toString() == QStringLiteral("Ctrl+Shift+T"));
    CHECK(tip->property("shortcutShown").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the title band's new-tab button carries the bound chord, or none (offscreen)",
          "[contour][gui][qml][kit][tooltip][titlebar]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller { 1 };
    auto const host = makeTitleBarHostWith(engine, controller, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* const newTab = findItemNamed(*qobject_cast<QQuickWindow*>(host.get())->contentItem(),
                                       QStringLiteral("newTabButton"));
    REQUIRE(newTab != nullptr);
    auto* const tip = newTab->findChild<QObject*>(QStringLiteral("toolbarButtonToolTip"));
    REQUIRE(tip != nullptr);

    CHECK(tip->property("text").toString() == QStringLiteral("New tab"));
    CHECK(tip->property("shortcutCommand").toString() == QStringLiteral("CreateNewTab"));

    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shortcut").toString().isEmpty());
    CHECK_FALSE(tip->property("shortcutShown").toBool());

    controller.boundChords.insert(QStringLiteral("CreateNewTab"), QStringLiteral("⌘T"));
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(partOf(tip, "kitToolTipShortcut")->property("text").toString() == QStringLiteral("⌘T"));
    CHECK(tip->property("shortcutShown").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a ToolbarButton without tooltip text wants no tooltip (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ToolbarButton.qml")));
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> button(
        component.createWithInitialProperties(QVariantMap { { "glyph", QStringLiteral("+") } }));
    REQUIRE(button != nullptr);
    auto* const tip = button->findChild<QObject*>(QStringLiteral("toolbarButtonToolTip"));
    REQUIRE(tip != nullptr);
    CHECK_FALSE(tip->property("wanted").toBool());

    button->setProperty("toolTipText", QStringLiteral("New tab"));
    button->setProperty("toolTipShortcut", QStringLiteral("⌘T"));
    CHECK(tip->property("text").toString() == QStringLiteral("New tab"));
    CHECK(tip->property("shortcut").toString() == QStringLiteral("⌘T"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
