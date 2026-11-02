// SPDX-License-Identifier: Apache-2.0
//
// The kit's tooltip: its shortcut slot, plain multi-line text, warm and cold delay, placement, and the
// controls that carry one (the title band's new-tab button, the find bar's buttons).

#include <contour/command/Shortcut.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>

#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>

#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

/// A window as a ThemedWindow is to its tooltips (a `tooltipClock` and the controller as `win`), with a
/// control-sized item and one KitToolTip beneath it, and a second control declaring a KitToolTipHost.
struct ToolTipHost
{
    QmlHost window {};
    QQuickItem* control = nullptr;
    QObject* toolTip = nullptr;
    QObject* tooltipClock = nullptr;
    QQuickItem* hoverControl = nullptr; ///< A control declaring a KitToolTipHost.

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
    auto const source =
        QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Window\n"
            "import Contour.Ui\n"
            "Window {\n"
            "  id: host\n"
            "  width: 400; height: 200; visible: true\n"
            "  property double fakeNow: 1000\n"
            "  property var tooltipClock: KitToolTipClock { now: function() { return host.fakeNow } }\n"
            "  property var win: null\n"
            "  Item {\n"
            "    objectName: \"control\"\n"
            "    x: %1; y: %2; width: 40; height: 20\n"
            "    KitToolTip {\n"
            "      objectName: \"tip\"\n"
            "      text: \"New tab\"\n"
            "    }\n"
            "  }\n"
            "  Item {\n"
            "    objectName: \"hoverControl\"\n"
            "    x: 250; y: 50; width: 40; height: 20\n"
            "    KitToolTipHost {\n"
            "      text: \"Hover me\"\n"
            "      shortcutCommand: \"CreateNewTab\"\n"
            "    }\n"
            "  }\n"
            "}\n")
            .arg(controlX)
            .arg(controlY);
    auto result =
        ToolTipHost { .window = buildQmlHost(engine, source, QStringLiteral("ToolTipTestHost.qml")) };
    result.control = result.window->findChild<QQuickItem*>(QStringLiteral("control"));
    result.toolTip = result.window->findChild<QObject*>(QStringLiteral("tip"));
    result.tooltipClock = result.window->property("tooltipClock").value<QObject*>();
    result.hoverControl = result.window->findChild<QQuickItem*>(QStringLiteral("hoverControl"));
    REQUIRE(result.control != nullptr);
    REQUIRE(result.toolTip != nullptr);
    REQUIRE(result.tooltipClock != nullptr);
    REQUIRE(result.hoverControl != nullptr);
    return result;
}

/// @return What @p clock, a KitToolTipClock, answers to `isWarm()`.
[[nodiscard]] bool clockIsWarm(QObject& clock)
{
    return QQmlExpression(qmlContext(&clock), &clock, QStringLiteral("isWarm()")).evaluate().toBool();
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
    CHECK(colorOf(label) == chromeColor(engine, "tooltipText").name(QColor::HexArgb));
    CHECK(colorOf(secondary) == chromeColor(engine, "tooltipTextSecondary").name(QColor::HexArgb));
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
              == chromeColor(engine, "tooltipFill").name(QColor::HexArgb));
        CHECK(host.toolTip->property("leftPadding").toReal() == chromeToken(engine, "size", "tooltipPadX"));
        CHECK(host.toolTip->property("topPadding").toReal() == chromeToken(engine, "size", "tooltipPadTop"));
        CHECK(host.toolTip->property("bottomPadding").toReal()
              == chromeToken(engine, "size", "tooltipPadBottom"));
        // A hairline only where the row has an edge colour.
        auto const hasEdge = chromeColor(engine, "tooltipEdge").alpha() > 0;
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
    CHECK(host.tooltipClock->property("warmMs").toInt() == 1500);

    auto const isWarm = [&] {
        return clockIsWarm(*host.tooltipClock);
    };
    CHECK_FALSE(isWarm());
    REQUIRE(QMetaObject::invokeMethod(host.tooltipClock, "touch"));
    CHECK(isWarm());
    // Time is the test's: just inside the span it is warm, at its end cold.
    host.window->setProperty("fakeNow", 1000.0 + 1499.0);
    CHECK(isWarm());
    host.window->setProperty("fakeNow", 1000.0 + 1500.0);
    CHECK_FALSE(isWarm());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: the first tooltip waits the token's delay, a warm one shows at once (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    // The window's clock starts cold.
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto const delay = static_cast<int>(chromeToken(engine, "motion", "tooltipDelay"));
    REQUIRE(delay == 600);

    host.toolTip->setProperty("wanted", true);
    CHECK(host.toolTip->property("delay").toInt() == delay);
    CHECK_FALSE(host.toolTip->property("visible").toBool());

    host.toolTip->setProperty("wanted", false);
    CHECK_FALSE(host.toolTip->property("visible").toBool());

    // A tooltip that showed has warmed the window's clock: the next one has no delay.
    REQUIRE(QMetaObject::invokeMethod(host.tooltipClock, "touch"));
    host.toolTip->setProperty("wanted", true);
    CHECK(host.toolTip->property("delay").toInt() == 0);
    CHECK(QTest::qWaitFor([&] { return host.toolTip->property("visible").toBool(); }, 2000));

    host.toolTip->setProperty("wanted", false);
    CHECK(QTest::qWaitFor([&] { return !host.toolTip->property("visible").toBool(); }, 2000));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// Shows @p host's tooltip at once (its clock warm) and waits until it is open.
void showToolTip(ToolTipHost& host)
{
    REQUIRE(QMetaObject::invokeMethod(host.tooltipClock, "touch"));
    host.toolTip->setProperty("wanted", true);
    REQUIRE(QTest::qWaitFor([&] { return host.toolTip->property("opened").toBool(); }, 3000));
}
} // namespace

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
        // The box is its content and its padding, which a template popup does not size itself by.
        CHECK(tip->property("width").toReal()
              == partOf(tip, "kitToolTipContent")->implicitWidth()
                     + (2 * chromeToken(engine, "size", "tooltipPadX")));
        CHECK(tip->property("height").toReal() > 0);
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
        // The window keeps the box, and its shadow, off its edge: the popup's margins say how far.
        CHECK(tip->property("margins").toReal() > 0);
        CHECK(right <= host.quick().width() - tip->property("margins").toReal());
        CHECK(host.control->x() + tip->property("x").toReal() >= tip->property("margins").toReal());
    }

    SECTION("flips above only where there is no room below")
    {
        host.control->setY(host.quick().height() - 20);
        showToolTip(host);
        CHECK(tip->property("y").toReal() + tip->property("height").toReal() <= 0.0);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// Moves the pointer to the middle of @p item, as the user's does.
void hoverOver(QQuickWindow& window, QQuickItem const& item)
{
    QTest::mouseMove(&window, item.mapToScene(QPointF { item.width() / 2, item.height() / 2 }).toPoint());
}

/// @return The tooltip @p control's KitToolTipHost has now, or null.
[[nodiscard]] QObject* toolTipOf(QQuickItem const& control)
{
    auto const* host = control.findChild<QQuickItem*>(QStringLiteral("kitToolTipHost"));
    REQUIRE(host != nullptr);
    return host->property("toolTip").value<QObject*>();
}
} // namespace

TEST_CASE("KitToolTip: its shortcut is asked of the window's controller when about to show (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const engine = std::make_unique<QQmlEngine>();
    auto const controller = std::make_unique<MockTabController>();
    auto host = openToolTipHost(*engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* const tip = host.toolTip;
    tip->setProperty("shortcutCommand", QStringLiteral("CreateNewTab"));

    // No controller on the window: nothing to ask, no slot.
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK_FALSE(tip->property("shortcutShown").toBool());

    host.window->setProperty("win", QVariant::fromValue(static_cast<QObject*>(controller.get())));
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shownShortcut").toString().isEmpty());
    CHECK_FALSE(tip->property("shortcutShown").toBool());

    controller->boundChords.insert(QStringLiteral("CreateNewTab"), QStringLiteral("Ctrl+Shift+T"));
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shownShortcut").toString() == QStringLiteral("Ctrl+Shift+T"));
    CHECK(tip->property("shortcutShown").toBool());

    // A shortcut named as it is wins over the looked-up one, and the binding behind it survives a lookup.
    tip->setProperty("shortcut", QStringLiteral("⌘T"));
    REQUIRE(QMetaObject::invokeMethod(tip, "refreshShortcut"));
    CHECK(tip->property("shownShortcut").toString() == QStringLiteral("⌘T"));
    tip->setProperty("shortcut", QString {});
    CHECK(tip->property("shownShortcut").toString() == QStringLiteral("Ctrl+Shift+T"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a control's tooltip is built while the pointer rests on it, and gone after (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    REQUIRE(QTest::qWaitForWindowExposed(&host.quick()));
    // The pointer starts off the control; a warm clock leaves out the delay.
    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    REQUIRE(QMetaObject::invokeMethod(host.tooltipClock, "touch"));

    CHECK(toolTipOf(*host.hoverControl) == nullptr);

    hoverOver(host.quick(), *host.hoverControl);
    REQUIRE(QTest::qWaitFor(
        [&] {
            auto const* tip = toolTipOf(*host.hoverControl);
            return tip != nullptr && tip->property("opened").toBool();
        },
        3000));
    auto* const tip = toolTipOf(*host.hoverControl);
    CHECK(partOf(tip, "kitToolTipLabel")->property("text").toString() == QStringLiteral("Hover me"));

    // A button pressed on the control sends it away; the exit fades out, then it is not built any more.
    QTest::mousePress(&host.quick(), Qt::LeftButton, Qt::NoModifier, QPoint { 270, 60 });
    CHECK(QTest::qWaitFor([&] { return toolTipOf(*host.hoverControl) == nullptr; }, 3000));
    QTest::mouseRelease(&host.quick(), Qt::LeftButton, Qt::NoModifier, QPoint { 270, 60 });

    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the title band's new-tab button carries the bound chord, or none (offscreen)",
          "[contour][gui][qml][kit][tooltip][titlebar]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const engine = std::make_unique<QQmlEngine>();
    auto const controller = std::make_unique<MockTabController>(1);
    auto const host =
        makeTitleBarHostWith(*engine, *controller, rowSetup(contour::config::ChromeStyle::MacOS));
    auto& window = *qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto* const newTab = findItemNamed(*window.contentItem(), QStringLiteral("newTabButton"));
    REQUIRE(newTab != nullptr);
    auto const* const toolTipHost = newTab->findChild<QQuickItem*>(QStringLiteral("kitToolTipHost"));
    REQUIRE(toolTipHost != nullptr);

    // The label is the button's accessible name; the chord is looked up by command.
    CHECK(toolTipHost->property("text").toString() == QStringLiteral("New tab"));
    CHECK(toolTipHost->property("shortcutCommand").toString() == QStringLiteral("CreateNewTab"));

    auto const showsChord = [&](QString const& expected) {
        QTest::mouseMove(&window, QPoint { 300, 300 });
        hoverOver(window, *newTab);
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto const* tip = toolTipOf(*newTab);
                return tip != nullptr && tip->property("opened").toBool();
            },
            5000));
        auto* const tip = toolTipOf(*newTab);
        CHECK(tip->property("shownShortcut").toString() == expected);
        CHECK(tip->property("shortcutShown").toBool() == !expected.isEmpty());
        QTest::mouseMove(&window, QPoint { 300, 300 });
        REQUIRE(QTest::qWaitFor([&] { return toolTipOf(*newTab) == nullptr; }, 5000));
    };
    showsChord({});
    controller->boundChords.insert(QStringLiteral("CreateNewTab"), QStringLiteral("⌘T"));
    showsChord(QStringLiteral("⌘T"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a ToolbarButton's tooltip text is its accessible name unless told otherwise (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ToolbarButton.qml")));
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> button(component.createWithInitialProperties(
        QVariantMap { { "glyph", QStringLiteral("+") }, { "accessibleName", QStringLiteral("New tab") } }));
    REQUIRE(button != nullptr);
    auto const* const host = button->findChild<QObject*>(QStringLiteral("kitToolTipHost"));
    REQUIRE(host != nullptr);
    CHECK(host->property("text").toString() == QStringLiteral("New tab"));

    button->setProperty("toolTipText", QStringLiteral("Open a tab"));
    CHECK(host->property("text").toString() == QStringLiteral("Open a tab"));

    // Nothing to say, no tooltip: the host never wants one.
    button->setProperty("toolTipText", QString {});
    CHECK_FALSE(host->property("wanted").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("no chrome QML shows a tooltip the attached way (offscreen)", "[contour][gui][qml][kit][tooltip]")
{
    // An attached ToolTip is the style's, and on macOS and Windows that is Fusion's or Fluent's, not the
    // kit's. Main.qml alone touches ToolTip.toolTip, to make a stray one plain text.
    auto const attached =
        QRegularExpression(QStringLiteral(R"(\bToolTip\.(visible|text|delay|timeout|toolTip)\b)"));
    auto files = QDirIterator(QStringLiteral(":/qt/qml/Contour/Ui"), { QStringLiteral("*.qml") });
    auto scanned = 0;
    while (files.hasNext())
    {
        auto const path = files.next();
        auto file = QFile(path);
        REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        ++scanned;
        for (auto const& line: QString::fromUtf8(file.readAll()).split(u'\n'))
        {
            if (line.trimmed().startsWith(u"//") || path.endsWith(QStringLiteral("/Main.qml")))
                continue;
            INFO(path.toStdString() << ": " << line.toStdString());
            CHECK_FALSE(attached.match(line).hasMatch());
        }
    }
    CHECK(scanned > 0);
}

TEST_CASE("KitToolTip: a tooltip open a long while leaves the next one warm (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    REQUIRE(QTest::qWaitForWindowExposed(&host.quick()));
    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    showToolTip(host);

    // Open for far longer than the warm span: it was last noted when it opened.
    host.window->setProperty("fakeNow", 1000.0 + (10 * 1500.0));
    host.toolTip->setProperty("wanted", false);
    // The hide has begun, not ended, and the clock already says so.
    CHECK(clockIsWarm(*host.tooltipClock));
    REQUIRE(QTest::qWaitFor([&] { return !host.toolTip->property("visible").toBool(); }, 3000));

    // The neighbour shows with no delay.
    hoverOver(host.quick(), *host.hoverControl);
    REQUIRE(QTest::qWaitFor([&] { return toolTipOf(*host.hoverControl) != nullptr; }, 3000));
    CHECK(toolTipOf(*host.hoverControl)->property("delay").toInt() == 0);
    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTipHost: a press keeps the tooltip away until the pointer leaves (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    REQUIRE(QTest::qWaitForWindowExposed(&host.quick()));
    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    REQUIRE(QMetaObject::invokeMethod(host.tooltipClock, "touch")); // warm: a tooltip would come back at once
    auto const centre = host.hoverControl->mapToScene(QPointF { 20, 10 }).toPoint();

    hoverOver(host.quick(), *host.hoverControl);
    REQUIRE(QTest::qWaitFor([&] { return toolTipOf(*host.hoverControl) != nullptr; }, 3000));
    QTest::mousePress(&host.quick(), Qt::LeftButton, Qt::NoModifier, centre);
    REQUIRE(QTest::qWaitFor([&] { return toolTipOf(*host.hoverControl) == nullptr; }, 3000));
    QTest::mouseRelease(&host.quick(), Qt::LeftButton, Qt::NoModifier, centre);

    // Released over the control, it stays away; a moment is long enough to see a tooltip that was coming.
    QTest::qWait(400);
    CHECK(toolTipOf(*host.hoverControl) == nullptr);

    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    QTest::qWait(100);
    hoverOver(host.quick(), *host.hoverControl);
    CHECK(QTest::qWaitFor([&] { return toolTipOf(*host.hoverControl) != nullptr; }, 3000));
    QTest::mouseMove(&host.quick(), QPoint { 5, 5 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: it is placed once and cannot outgrow its token's lines (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    auto* const tip = host.toolTip;
    auto lines = QStringList {};
    for (auto const index: std::views::iota(0, 40))
        lines << QStringLiteral("line %1 of a very long message").arg(index);
    tip->setProperty("text", lines.join(u'\n'));
    showToolTip(host);

    // Placed as it began to show and not again, so it cannot move during the fade-in.
    CHECK(tip->property("placements").toInt() == 1);

    auto const maxLines = static_cast<int>(chromeToken(engine, "size", "tooltipMaxLines"));
    REQUIRE(maxLines > 0);
    auto const* label = partOf(tip, "kitToolTipLabel");
    auto const* secondary = partOf(tip, "kitToolTipSecondary");
    REQUIRE(label != nullptr);
    REQUIRE(secondary != nullptr);
    CHECK(label->property("lineCount").toInt() + secondary->property("lineCount").toInt() <= maxLines);
    CHECK(tip->property("height").toReal() <= host.quick().height());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitToolTip: CRLF text splits into the same lines as LF (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto host = openToolTipHost(engine, rowSetup(contour::config::ChromeStyle::MacOS));
    host.toolTip->setProperty("text", QStringLiteral("Build finished\r\n~/repo\r\n/tmp"));
    CHECK(partOf(host.toolTip, "kitToolTipLabel")->property("text").toString()
          == QStringLiteral("Build finished"));
    CHECK(partOf(host.toolTip, "kitToolTipSecondary")->property("text").toString()
          == QStringLiteral("~/repo\n/tmp"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the styles' stray tooltips are placed as KitToolTip is (offscreen)",
          "[contour][gui][qml][kit][tooltip]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const engine = std::make_unique<QQmlEngine>();
    for (auto const* style: { "ContourLinux", "ContourTui" })
    {
        CAPTURE(style);
        auto host = openToolTipHost(*engine, rowSetup(contour::config::ChromeStyle::MacOS));
        // Near the bottom: there is no room below, so it goes above, as KitToolTip does.
        host.control->setY(host.quick().height() - 24);
        QQmlComponent component(
            engine.get(), QUrl(QStringLiteral("qrc:/qt/qml/%1/ToolTip.qml").arg(QLatin1StringView(style))));
        INFO(component.errorString().toStdString());
        REQUIRE(component.isReady());
        std::unique_ptr<QObject> stray(component.create());
        REQUIRE(stray != nullptr);
        stray->setProperty("parent", QVariant::fromValue(static_cast<QObject*>(host.control)));
        stray->setProperty("text", QStringLiteral("stray"));
        CHECK(stray->property("margins").toReal() > 0);
        REQUIRE(QMetaObject::invokeMethod(stray.get(), "open"));
        REQUIRE(QTest::qWaitFor([&] { return stray->property("opened").toBool(); }, 3000));
        CHECK(stray->property("y").toReal() + stray->property("height").toReal() <= 0.0);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the find bar's shortcut table names exactly the keys SearchBar.qml handles",
          "[contour][gui][qml][search]")
{
    using contour::command::FindBarShortcuts;
    auto const read = [](QString const& path) {
        auto file = QFile(path);
        REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
        return QString::fromUtf8(file.readAll());
    };
    auto const searchBar = read(QStringLiteral(":/qt/qml/Contour/Ui/SearchBar.qml"));

    // Every name a tooltip asks for is a row; every row is asked for.
    auto asked = QStringList {};
    auto files = QDirIterator(QStringLiteral(":/qt/qml/Contour/Ui"), { QStringLiteral("*.qml") });
    auto const call = QRegularExpression(QStringLiteral(R"re(findBarShortcutText\("(\w+)"\))re"));
    while (files.hasNext())
    {
        auto matches = call.globalMatch(read(files.next()));
        while (matches.hasNext())
            asked << matches.next().captured(1);
    }
    for (auto const& name: asked)
    {
        INFO("asked for: " << name.toStdString());
        CHECK(std::ranges::any_of(FindBarShortcuts,
                                  [&](auto const& row) { return QString::fromLatin1(row.name) == name; }));
    }
    for (auto const& row: FindBarShortcuts)
    {
        INFO("row: " << row.name);
        CHECK(asked.contains(QString::fromLatin1(row.name)));
        // The key the row names, and the handler SearchBar.qml answers it with.
        if (std::get<vtbackend::Key>(row.input) == vtbackend::Key::Enter)
        {
            CHECK(searchBar.contains(QStringLiteral("Keys.onReturnPressed")));
            CHECK(searchBar.contains(QStringLiteral("Keys.onEnterPressed")));
            // Shift turns the step round: the row with Shift is Previous, the one without is Next.
            CHECK(row.modifiers.test(vtbackend::Modifier::Shift)
                  == (QString::fromLatin1(row.name) == QStringLiteral("FindPrevious")));
        }
        else
        {
            CHECK(std::get<vtbackend::Key>(row.input) == vtbackend::Key::Escape);
            CHECK(searchBar.contains(QStringLiteral("Keys.onEscapePressed")));
        }
    }
    // Shift on Enter reads as "previous" in the handler itself.
    CHECK(searchBar.contains(QStringLiteral("event.modifiers & Qt.ShiftModifier")));
    CHECK(FindBarShortcuts.size() == 3);
}
