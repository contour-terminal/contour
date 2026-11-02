// SPDX-License-Identifier: Apache-2.0
//
// QML component tests, continued: the title bar and its window controls, the find bar, an ended pane and the
// drift badge.

#include <contour/command/Shortcut.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlLook.hpp>
#include <contour/platform/CaptionHitTest.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/SearchStatus.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <core/Utils.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDirIterator>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeData>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtQml/QJSValue>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

/// How many window controls @p slot holds, by their `windowControl_` object names.
[[nodiscard]] int windowControlCount(QQuickItem const& slot)
{
    auto count = 0;
    for (auto const* action: { "close", "minimize", "maximize" })
        if (findItemNamed(slot, QStringLiteral("windowControl_") + QLatin1StringView(action)) != nullptr)
            ++count;
    return count;
}

/// A title bar host on @p row with the Linux window-control style placed by @p layout.
[[nodiscard]] QmlHost makeLinuxControlsHost(QQmlEngine& engine,
                                            MockTabController& controller,
                                            contour::config::ChromeStyle row,
                                            contour::config::WindowButtonLayout layout)
{
    auto setup = contour::test::defaultChromeSetup(contour::config::UiStyle::Native,
                                                   contour::config::WindowControlStyle::Linux);
    setup.row = row;
    setup.buttonLayout = std::move(layout);
    return makeTitleBarHostWith(engine, controller, setup);
}

/// The chrome setup of a Windows host's title bar on @p row: the Windows controls, and no icon font
/// installed, so the glyphs do not depend on the machine's fonts.
[[nodiscard]] contour::test::ChromeSetup windowsChromeOn(contour::config::ChromeStyle row)
{
    return contour::test::ChromeSetup { .row = row,
                                        .font = QGuiApplication::font(),
                                        .windowControls = contour::config::WindowControlStyle::Windows };
}

struct SearchBarHost
{
    std::unique_ptr<QObject> window;
    QObject* bar = nullptr;

    /// How often the bar handed the keyboard back to the "terminal" it floats over.
    [[nodiscard]] int focusRestores() const { return window->property("focusRestores").toInt(); }
};

/// Builds the bar's offscreen host and opens it.
///
/// @param engine    The QML engine to build in.
/// @param session   The session the bar is bound to at first.
/// @param alternate The session `useAlternate` on the host rebinds it to, standing in for the tab
///                  switch SessionChrome performs under a bar that stays open. Defaults to @p session,
///                  so a host that never flips the flag sees exactly one binding.
[[nodiscard]] SearchBarHost openSearchBar(
    QQmlEngine& engine,
    MockSession& session,
    MockSession* alternate = nullptr,
    contour::test::ChromeSetup const& chrome = contour::test::ChromeSetup {
        .row = contour::config::ChromeStyle::MacOS,
        .font = QGuiApplication::font(),
        .windowControls = contour::config::WindowControlStyle::Windows })
{
    contour::test::installChromeStyle(engine, chrome);
    engine.rootContext()->setContextProperty("mockSession", &session);
    engine.rootContext()->setContextProperty("mockSessionAlt", alternate != nullptr ? alternate : &session);

    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "import QtQuick.Window\n"
            "import Contour.Ui\n"
            "Window {\n"
            "  id: host\n"
            "  width: 800; height: 400; visible: true\n"
            "  property int focusRestores: 0\n"
            "  property bool useAlternate: false\n"
            "  property alias searchBarItem: barItem\n"
            // Stands in for the ContourTerminal the bar docks against: the bar reads its
            // size and its rows' band to place itself, and calls forceActiveFocus() on it when
            // it closes. Rows are 20 pixels, from the top.
            "  Item {\n"
            "    id: fakeDisplay\n"
            "    anchors.fill: parent\n"
            "    function forceActiveFocus() { host.focusRestores++; }\n"
            "    function rowBand(first, last) { return { top: first * 20, bottom: (last + 1) * 20 }; }\n"
            "  }\n"
            "  SearchBar {\n"
            "    id: barItem\n"
            "    session: host.useAlternate ? mockSessionAlt : mockSession\n"
            "    displayItem: fakeDisplay\n"
            "  }\n"
            "}\n"),
        QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SearchBarTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    auto host = SearchBarHost {};
    {
        INFO("SearchBarTestHost errors: " << component.errorString().toStdString());
        if (!component.isReady())
            return host;
    }

    host.window.reset(component.create());
    if (host.window == nullptr)
        return host;

    host.bar = host.window->property("searchBarItem").value<QObject*>();
    if (host.bar == nullptr)
        return host;

    // A Popup builds its contentItem lazily; opening it materializes the field and the buttons.
    QMetaObject::invokeMethod(host.bar, "open");
    QCoreApplication::processEvents();
    return host;
}

/// The item of @p bar named @p name, found in its visual tree: a Repeater's delegates are not QObject
/// children of the popup, so findChild() does not reach them.
[[nodiscard]] QQuickItem* searchBarPart(QObject const& bar, QString const& name)
{
    auto const* content = bar.property("contentItem").value<QQuickItem*>();
    if (content == nullptr)
        return nullptr;
    auto const found = contour::test::itemsNamed(*content, name);
    return found.empty() ? nullptr : found.front();
}

/// The room between the count and the first toggle of @p bar, in the bar's row.
[[nodiscard]] qreal countToToggleGap(QObject const& bar)
{
    auto const* count = searchBarPart(bar, QStringLiteral("searchBarCount"));
    auto const* toggle =
        searchBarPart(bar, QString::fromUtf8(contour::session::SearchToggleRows.front().objectName));
    REQUIRE(count != nullptr);
    REQUIRE(toggle != nullptr);
    return toggle->x() - (count->x() + count->width());
}

/// The toggle buttons of @p bar, in the session table's order.
[[nodiscard]] std::vector<QQuickItem*> searchToggleButtons(QObject const& bar)
{
    auto buttons = std::vector<QQuickItem*> {};
    for (auto const& row: contour::session::SearchToggleRows)
        buttons.push_back(searchBarPart(bar, QString::fromUtf8(row.objectName)));
    return buttons;
}

/// A drift badge shown in a window, with the model it reads.
struct DriftBadgeFixture
{
    QQmlEngine engine;
    contour::window::DaemonDriftModel model;
    QQuickWindow window;
    std::unique_ptr<QObject> object;
    QQuickItem* badge = nullptr;

    DriftBadgeFixture()
    {
        contour::test::installChromeStyle(engine);
        engine.rootContext()->setContextProperty("daemonDrift", &model);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/DaemonDriftBadge.qml")));
        REQUIRE(component.isReady());
        object.reset(component.create());
        badge = qobject_cast<QQuickItem*>(object.get());
        REQUIRE(badge != nullptr);
        window.resize(400, 300);
        badge->setParentItem(window.contentItem());
        badge->setPosition({ 10, 10 });
    }

    /// Clicks the middle of the badge.
    void click()
    {
        window.show();
        auto const centre = badge->mapToScene(QPointF { badge->width() / 2, badge->height() / 2 });
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    }

    [[nodiscard]] QObject* dialog() const
    {
        return badge->findChild<QObject*>(QStringLiteral("daemonDriftConfirm"));
    }
};

} // namespace

TEST_CASE("The window controls follow the desktop's layout on either side (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    using contour::config::ChromeStyle;
    using contour::config::WindowButtonLayout;
    using enum contour::config::WindowControlKind;
    contour::test::QmlMessageCapture const warnings;

    SECTION("a split layout fills both slots")
    {
        QQmlEngine engine;
        MockTabController controller;
        auto const host = makeLinuxControlsHost(
            engine,
            controller,
            ChromeStyle::ContourLinux,
            WindowButtonLayout { .leading = { Close }, .trailing = { Minimize, Maximize } });
        auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        auto* leading = bar->findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
        auto* trailing = bar->findChild<QQuickItem*>(QStringLiteral("trailingWindowControls"));
        REQUIRE(leading != nullptr);
        REQUIRE(trailing != nullptr);
        CHECK(leading->property("active").toBool());
        CHECK(trailing->property("active").toBool());
        CHECK(windowControlCount(*leading) == 1);
        CHECK(windowControlCount(*trailing) == 2);
        CHECK(findItemNamed(*leading, QStringLiteral("windowControl_close")) != nullptr);

        // Under the ContourLinux row a control is the round button: a windowButton square, fully rounded.
        auto const* round = findItemNamed(*bar, QStringLiteral("windowControlRound_minimize"));
        REQUIRE(round != nullptr);
        CHECK(round->width() == chromeToken(engine, "size", "windowButton"));
        CHECK(round->height() == chromeToken(engine, "size", "windowButton"));
        CHECK(round->property("radius").toReal() == chromeToken(engine, "radii", "windowButton"));
    }

    SECTION("a desktop that wants no buttons gets no slot")
    {
        // Review Focus 5: neither Loader is active, and neither reserves width.
        QQmlEngine engine;
        MockTabController controller;
        auto const host =
            makeLinuxControlsHost(engine, controller, ChromeStyle::ContourLinux, WindowButtonLayout {});
        auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        auto* leading = bar->findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
        auto* trailing = bar->findChild<QQuickItem*>(QStringLiteral("trailingWindowControls"));
        REQUIRE(leading != nullptr);
        REQUIRE(trailing != nullptr);
        CHECK_FALSE(leading->property("active").toBool());
        CHECK_FALSE(trailing->property("active").toBool());
        CHECK(leading->width() == 0.0);
        CHECK(trailing->width() == 0.0);
    }

    SECTION("the Terminal row draws the same layout as dot cells")
    {
        QQmlEngine engine;
        MockTabController controller;
        auto const host = makeLinuxControlsHost(
            engine,
            controller,
            ChromeStyle::Terminal,
            WindowButtonLayout { .leading = { Close }, .trailing = { Minimize, Maximize } });
        auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        for (auto const* action: { "close", "minimize", "maximize" })
            CHECK(findItemNamed(*bar, QStringLiteral("windowControlCell_") + QLatin1StringView(action))
                  != nullptr);
        CHECK(findItemNamed(*bar, QStringLiteral("windowControlRound_close")) == nullptr);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Traffic lights are drawn as cells in the terminal chrome and as shapes in the native one "
          "(offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // The mechanism that lets the macOS controls exist in a cell-quantized chrome at all: the
    // cell-counting style states a trafficLightGlyph and the QML draws a character, the pixel-counting
    // one leaves it empty and the QML draws a circle. Which delegate a Loader picked is not directly
    // observable, so this asserts the shape each one builds: a circle is a Rectangle whose radius is
    // half its width, and the cell delegate has no Rectangle at all.
    //
    // Structure rather than rendered text, deliberately: both delegates DO have a Text, and which
    // string it holds depends on whether the group is hovered -- which offscreen, with no pointer,
    // is not a state a test can rely on either way.
    contour::test::QmlMessageCapture const warnings;

    auto drawsACircle = [](contour::config::UiStyle uiStyle) {
        QQmlEngine engine;
        MockTabController controller;
        auto const host =
            makeTitleBarHost(engine, controller, uiStyle, contour::config::WindowControlStyle::MacOS);
        auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(bar != nullptr);
        auto const* control = findItemNamed(*bar, QStringLiteral("windowControl_close"));
        REQUIRE(control != nullptr);

        // A circle is a Rectangle whose radius is half its width.
        return findDescendant(*control,
                              [](QQuickItem const& item) {
                                  auto const radius = item.property("radius");
                                  return radius.isValid() && item.width() > 0.0
                                         && radius.toReal() == Catch::Approx(item.width() / 2.0);
                              })
               != nullptr;
    };

    // Native counts in pixels, so a dot is the circle it is.
    CHECK(drawsACircle(contour::config::UiStyle::Native));
    // Terminal counts in cells, so it is a character instead -- and a circle here would be the one
    // part of the bar that did not line up with the terminal below it.
    CHECK(!drawsACircle(contour::config::UiStyle::Terminal));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the native hover state lights the maximise button (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host =
        makeTitleBarHostWith(engine, controller, windowsChromeOn(contour::config::ChromeStyle::Fluent));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    activate(*window);
    auto const* fill = findItemNamed(*window->contentItem(), QStringLiteral("windowControlFill_maximize"));
    auto const* glyph = findItemNamed(*window->contentItem(), QStringLiteral("windowControlGlyph_maximize"));
    REQUIRE(fill != nullptr);
    REQUIRE(glyph != nullptr);
    auto const fillColor = [&] {
        return fill->property("color").value<QColor>();
    };
    auto const glyphColor = [&] {
        return glyph->property("color").value<QColor>();
    };

    CHECK(fillColor().alpha() == 0);

    controller.setNativeMaximizeVisual(contour::platform::CaptionButtonVisual::Hover);
    // The Windows style washes with the kit's fill.
    CHECK(fillColor() == chromeColor(engine, "captionWash"));
    CHECK(glyphColor() != chromeColor(engine, "captionGlyphPressed"));

    controller.setNativeMaximizeVisual(contour::platform::CaptionButtonVisual::Pressed);
    CHECK(fillColor() == chromeColor(engine, "captionWash"));
    REQUIRE(window->isActive());
    CHECK(glyphColor().rgba() == chromeColor(engine, "captionGlyphPressed").rgba());

    controller.setNativeMaximizeVisual(contour::platform::CaptionButtonVisual::Rest);
    CHECK(fillColor().alpha() == 0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a contrast theme drops the red close hover (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    for (auto const contrast:
         { contour::platform::ContrastTheme::None, contour::platform::ContrastTheme::Active })
    {
        INFO("contrast theme: " << static_cast<int>(contrast));
        contour::test::QmlMessageCapture const warnings;
        QQmlEngine engine;
        MockTabController controller;
        auto setup = windowsChromeOn(contour::config::ChromeStyle::Fluent);
        setup.appearance.contrastTheme = contrast;
        auto const host = makeTitleBarHostWith(engine, controller, setup);
        auto* window = qobject_cast<QQuickWindow*>(host.get());
        REQUIRE(window != nullptr);
        activate(*window);
        auto const* slot = findItemNamed(*window->contentItem(), QStringLiteral("windowControl_close"));
        auto const* fill = findItemNamed(*window->contentItem(), QStringLiteral("windowControlFill_close"));
        REQUIRE(slot != nullptr);
        REQUIRE(fill != nullptr);

        // The offscreen platform enables no hover effects of its own, so the button is asked to take
        // hover as a desktop style would have it do. The fill sits in the button's background item.
        auto* button = fill->parentItem() != nullptr ? fill->parentItem()->parentItem() : nullptr;
        REQUIRE(button != nullptr);
        button->setProperty("hoverEnabled", true);
        // From off the button, so each pass sees the pointer enter.
        QTest::mouseMove(window, QPoint(0, window->height() - 1));
        QTest::mouseMove(window, sceneRectOf(*slot).center().toPoint());
        REQUIRE(QTest::qWaitFor([&] { return button->property("hovered").toBool(); }, 2000));
        // Windows' red without a contrast theme; under one, the wash every other caption button takes.
        auto const expected = contrast == contour::platform::ContrastTheme::Active
                                  ? chromeColor(engine, "captionWash")
                                  : QColor(QStringLiteral("#c42b1c"));
        CHECK(QTest::qWaitFor([&] { return fill->property("color").value<QColor>() == expected; }, 2000));

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the caption buttons report their rectangles (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host =
        makeTitleBarHostWith(engine, controller, windowsChromeOn(contour::config::ChromeStyle::Fluent));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    auto const& provider = providerOf(engine);
    auto const maximize = static_cast<int>(contour::config::WindowControlKind::Maximize);
    auto const* slot = findItemNamed(*window->contentItem(), QStringLiteral("windowControl_maximize"));
    REQUIRE(slot != nullptr);

    REQUIRE(QTest::qWaitFor([&] { return controller.captionRects.contains(maximize); }, 2000));
    auto const reported = controller.captionRects.value(maximize);
    CHECK(reported == sceneRectOf(*slot));
    CHECK(reported.width() == Catch::Approx(provider.windowControlWidth()).margin(1.0));
    CHECK(reported.height() == Catch::Approx(provider.chromeHeight()).margin(1.0));
    // Every control reports, not only the maximise one.
    CHECK(controller.captionRects.size() == static_cast<qsizetype>(contour::config::WindowControlCount));

    // The trailing controls follow the window's edge once the bar is laid out again.
    window->setWidth(window->width() + 100);
    CHECK(QTest::qWaitFor([&] { return controller.captionRects.value(maximize).x() == reported.x() + 100; },
                          2000));

    // Handed to the native frame (show_title_bar), the controls go, and each says so.
    auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);
    bar->setProperty("useCustomWindowControls", false);
    CHECK(QTest::qWaitFor([&] { return controller.captionRects.isEmpty(); }, 2000));
    for (auto const kind: std::views::iota(0, static_cast<int>(contour::config::WindowControlCount)))
        CHECK(std::ranges::find(controller.clearedCaptionKinds, kind)
              != controller.clearedCaptionKinds.end());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the caption buttons keep their rectangles across a change of side (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // A side change swaps the leading and trailing Loaders, and the two do not go in a fixed order: the
    // new controls may report before the old ones are destroyed. What the old ones clear on their way out
    // must not take the new ones' rectangles with it, or Snap Layouts stay off until the next move.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host =
        makeTitleBarHostWith(engine, controller, windowsChromeOn(contour::config::ChromeStyle::Fluent));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    auto* provider = engine.rootContext()
                         ->contextProperty(QStringLiteral("windowControls"))
                         .value<contour::window::WindowControlStyleProvider*>();
    REQUIRE(provider != nullptr);
    auto const maximize = static_cast<int>(contour::config::WindowControlKind::Maximize);
    REQUIRE(QTest::qWaitFor([&] { return controller.captionRects.contains(maximize); }, 2000));

    // Both ways round, so whichever order the Loaders go in, one of the swaps puts the new controls first.
    for (auto const style:
         { contour::config::WindowControlStyle::MacOS, contour::config::WindowControlStyle::Windows })
    {
        INFO("style " << static_cast<int>(style));
        auto const before = controller.captionRects.value(maximize);
        provider->update(style, contour::config::windowButtonPlacement(style, std::nullopt));
        REQUIRE(QTest::qWaitFor(
            [&] {
                auto const* slot =
                    findItemNamed(*window->contentItem(), QStringLiteral("windowControl_maximize"));
                return slot != nullptr && sceneRectOf(*slot) != before;
            },
            2000));
        QCoreApplication::processEvents();
        auto const* slot = findItemNamed(*window->contentItem(), QStringLiteral("windowControl_maximize"));
        REQUIRE(slot != nullptr);
        CHECK(QTest::qWaitFor([&] { return controller.captionRects.value(maximize) == sceneRectOf(*slot); },
                              2000));
        CHECK(controller.captionRects.size() == static_cast<qsizetype>(contour::config::WindowControlCount));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("every window-control style draws real buttons on every row (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // The Settings dropdown offers every style on every row: each combination must draw buttons that can be
    // seen and clicked, and hand the Win32 caption a non-empty rectangle for each.
    using contour::config::WindowControlStyle;
    using enum contour::config::WindowControlKind;
    contour::test::QmlMessageCapture const warnings;
    for (auto const rowIndex: std::views::iota(std::size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        auto const row = static_cast<contour::config::ChromeStyle>(rowIndex);
        for (auto const& entry: contour::config::configEnumValues<WindowControlStyle>())
        {
            if (entry.value == WindowControlStyle::Auto)
                continue;
            CAPTURE(rowIndex, std::string(entry.token));
            QQmlEngine engine;
            MockTabController controller;
            auto setup = windowsChromeOn(row);
            setup.windowControls = entry.value;
            setup.buttonLayout =
                contour::config::WindowButtonLayout { .leading = {},
                                                      .trailing = { Minimize, Maximize, Close } };
            auto const host = makeTitleBarHostWith(engine, controller, setup);
            auto* window = qobject_cast<QQuickWindow*>(host.get());
            REQUIRE(window != nullptr);
            REQUIRE(QTest::qWaitForWindowExposed(window));

            REQUIRE(QTest::qWaitFor(
                [&] {
                    return controller.captionRects.size()
                           == static_cast<qsizetype>(contour::config::WindowControlCount);
                },
                2000));
            for (auto const& rect: std::as_const(controller.captionRects))
            {
                CHECK(rect.width() > 0.0);
                CHECK(rect.height() > 0.0);
            }

            // The round button's circle and its icon, wherever the style draws round buttons.
            auto const* round =
                findItemNamed(*window->contentItem(), QStringLiteral("windowControlRound_close"));
            auto const drawsRound =
                contour::config::presentationFor(contour::config::windowControlTokens(entry.value), row)
                == contour::config::WindowControlPresentation::Round;
            CHECK((round != nullptr) == drawsRound);
            if (round != nullptr)
            {
                CHECK(round->width() > 0.0);
                CHECK(round->property("radius").toReal() > 0.0);
                auto const* icon =
                    findItemNamed(*window->contentItem(), QStringLiteral("windowControlIcon_close"));
                REQUIRE(icon != nullptr);
                CHECK(icon->width() > 0.0);
                CHECK(icon->height() > 0.0);
            }
        }
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("an inactive window dims the caption glyphs (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host =
        makeTitleBarHostWith(engine, controller, windowsChromeOn(contour::config::ChromeStyle::Fluent));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    activate(*window);
    auto const* glyph = findItemNamed(*window->contentItem(), QStringLiteral("windowControlGlyph_close"));
    REQUIRE(glyph != nullptr);
    auto const glyphColor = [&] {
        return glyph->property("color").value<QColor>();
    };

    CHECK(glyphColor() == QGuiApplication::palette().color(QPalette::Active, QPalette::WindowText));

    // Another window takes the focus.
    auto other = QQuickWindow {};
    other.show();
    activate(other);
    REQUIRE(QTest::qWaitFor([&] { return !window->isActive(); }, 2000));
    // By value: the binding hands back the colour in another spec than the role's.
    CHECK(glyphColor().rgba() == chromeColor(engine, "captionGlyphInactive").rgba());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row draws a Windows host's controls as scheme-inked cells (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host =
        makeTitleBarHostWith(engine, controller, windowsChromeOn(contour::config::ChromeStyle::Terminal));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    activate(*window);
    auto const& provider = providerOf(engine);

    struct Cell
    {
        char const* action;
        contour::config::StatusColorRole role;
    };
    auto lefts = std::vector<qreal> {};
    // Trailing, in Windows' order.
    for (auto const& cell: { Cell { .action = "minimize", .role = contour::config::StatusColorRole::Warning },
                             Cell { .action = "maximize", .role = contour::config::StatusColorRole::Success },
                             Cell { .action = "close", .role = contour::config::StatusColorRole::Danger } })
    {
        INFO("control: " << cell.action);
        auto const* text = findItemNamed(
            *window->contentItem(), QStringLiteral("windowControlCell_") + QLatin1StringView(cell.action));
        REQUIRE(text != nullptr);
        CHECK(text->property("color").value<QColor>() == provider.statusInk(static_cast<int>(cell.role)));
        lefts.push_back(sceneRectOf(*text).left());
    }
    CHECK(std::ranges::is_sorted(lefts));

    // The maximise cell is where the caption adapter looks for it too.
    CHECK(QTest::qWaitFor(
        [&] {
            return controller.captionRects.contains(
                static_cast<int>(contour::config::WindowControlKind::Maximize));
        },
        2000));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar drives the session's search and restores focus (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;

    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);

    auto* field = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarField"));
    REQUIRE(field != nullptr);
    auto* count = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCount"));
    REQUIRE(count != nullptr);
    auto* previous = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarPrevious"));
    REQUIRE(previous != nullptr);
    auto* next = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarNext"));
    REQUIRE(next != nullptr);

    SECTION("typing runs the search and shows its count")
    {
        field->setProperty("text", QStringLiteral("needle"));
        QCoreApplication::processEvents();

        CHECK(session.searchPattern() == QStringLiteral("needle"));
        CHECK(count->property("text").toString() == QStringLiteral("1 of 3"));
    }

    SECTION("stepping is inert until there is somewhere to step")
    {
        // Nothing typed yet: the buttons are visible but dead, rather than absent -- a bar whose
        // controls come and go as you type is the thing this deliberately does not do.
        CHECK_FALSE(previous->property("enabled").toBool());
        CHECK_FALSE(next->property("enabled").toBool());

        field->setProperty("text", QStringLiteral("needle"));
        QCoreApplication::processEvents();
        CHECK(previous->property("enabled").toBool());
        CHECK(next->property("enabled").toBool());
    }

    SECTION("a fruitless search says so")
    {
        field->setProperty("text", QStringLiteral("zzz"));
        QCoreApplication::processEvents();

        CHECK(count->property("text").toString() == QStringLiteral("No results"));
        CHECK_FALSE(previous->property("enabled").toBool());
    }

    SECTION("the case toggle cycles all three policies, and shows which one is active")
    {
        auto* toggle = searchBarPart(*host.bar, QStringLiteral("searchBarCaseToggle"));
        REQUIRE(toggle != nullptr);

        // Asserted through what the BUTTON shows, not through enumerator values: an earlier version
        // compared raw integers here and happily pinned an inverted mapping in place.
        CHECK(toggle->property("text").toString() == QStringLiteral("Aa"));
        CHECK_FALSE(toggle->property("checked").toBool()); // smart is not "pinned"

        auto const press = [&] {
            QMetaObject::invokeMethod(toggle, "clicked");
            QCoreApplication::processEvents();
        };
        press(); // Smart -> Sensitive: still the capitalised glyph, now lit.
        CHECK(toggle->property("text").toString() == QStringLiteral("Aa"));
        CHECK(toggle->property("checked").toBool());
        press(); // Sensitive -> Insensitive: the lowercase glyph is the one that means "ignoring case".
        CHECK(toggle->property("text").toString() == QStringLiteral("aa"));
        CHECK(toggle->property("checked").toBool());
        press(); // Insensitive -> Smart closes the cycle.
        CHECK(toggle->property("text").toString() == QStringLiteral("Aa"));
        CHECK_FALSE(toggle->property("checked").toBool());
    }

    SECTION("the bar tells the session when it opens and closes")
    {
        // The session cannot infer this, and tallying while nothing displays the answer is a walk of
        // the whole scrollback for nobody.
        CHECK(session.openNotifications == 1); // openSearchBar() opened it
        CHECK(session.closeNotifications == 0);

        QMetaObject::invokeMethod(host.bar, "close");
        QCoreApplication::processEvents();
        CHECK(session.closeNotifications == 1);
    }

    SECTION("closing hands the keyboard back to the terminal")
    {
        auto const before = host.focusRestores();
        QMetaObject::invokeMethod(host.bar, "close");
        QCoreApplication::processEvents();

        // Whatever closed the bar, the terminal gets the keyboard back, or the user is left typing
        // into nothing.
        CHECK(host.focusRestores() == before + 1);
    }

    SECTION("re-opening onto an existing term selects it, so typing replaces rather than appends")
    {
        field->setProperty("text", QStringLiteral("needle"));
        QCoreApplication::processEvents();

        QMetaObject::invokeMethod(host.bar, "close");
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(host.bar, "open");
        QCoreApplication::processEvents();

        CHECK(field->property("text").toString() == QStringLiteral("needle"));
        CHECK(field->property("selectedText").toString() == QStringLiteral("needle"));
    }

    SECTION("re-seeding the field does not bounce the term back into the session")
    {
        // The field pushes what it holds into the session, which is right for a keystroke and wrong
        // for a value the bar just read OUT of that same session: the push costs a walk of the whole
        // scrollback and re-runs the search from the vi cursor, moving the very position the seed was
        // there to preserve.
        field->setProperty("text", QStringLiteral("needle"));
        QCoreApplication::processEvents();

        QMetaObject::invokeMethod(host.bar, "close");
        QCoreApplication::processEvents();

        // The session's term has to MOVE, or the re-seed writes the value the field already holds and
        // no push could happen with or without the guard -- a test that passes either way.
        session.setSearchPattern(QStringLiteral("other"));
        auto const pushes = session.setPatternCalls;

        QMetaObject::invokeMethod(host.bar, "open");
        QCoreApplication::processEvents();

        CHECK(field->property("text").toString() == QStringLiteral("other"));
        CHECK(session.setPatternCalls == pushes);
    }

    SECTION("Escape leaves the bar even when a button, not the field, holds the focus")
    {
        // The field handles Escape itself, so that path is direct. This is the other one: tab onto a
        // button and the key would go unhandled, leaving the application's only modeless popup with no
        // way out but the mouse.
        auto* window = qobject_cast<QQuickWindow*>(host.window.get());
        REQUIRE(window != nullptr);
        auto* toggle = searchBarPart(*host.bar, QStringLiteral("searchBarCaseToggle"));
        REQUIRE(toggle != nullptr);

        QMetaObject::invokeMethod(toggle, "forceActiveFocus");
        QCoreApplication::processEvents();
        REQUIRE(toggle->hasActiveFocus());

        QTest::keyClick(window, Qt::Key_Escape);
        QCoreApplication::processEvents();

        CHECK_FALSE(host.bar->property("opened").toBool());
    }

    SECTION("activating the case toggle for real keeps its lit state in step with the policy")
    {
        // The section above cycles through the bar's own function, which a CHECKABLE button would
        // survive: such a button writes `checked` itself on activation, and that write is corrected
        // only when the binding's value actually changes -- so Sensitive -> Insensitive, where pinned
        // stays true both sides, left the button unlit while the policy was pinned. Only a real
        // activation reaches that write, and Space on a focused button takes the same press/release
        // path a click does.
        auto* window = qobject_cast<QQuickWindow*>(host.window.get());
        REQUIRE(window != nullptr);
        auto* toggle = searchBarPart(*host.bar, QStringLiteral("searchBarCaseToggle"));
        REQUIRE(toggle != nullptr);

        QMetaObject::invokeMethod(toggle, "forceActiveFocus");
        QCoreApplication::processEvents();
        REQUIRE(toggle->hasActiveFocus());

        QTest::keyClick(window, Qt::Key_Space); // Smart -> Sensitive
        QCoreApplication::processEvents();
        REQUIRE(session.searchOptions.caseSensitivity == vtbackend::SearchCaseSensitivity::Sensitive);
        CHECK(toggle->property("checked").toBool());

        QTest::keyClick(window, Qt::Key_Space); // Sensitive -> Insensitive: still pinned, still lit
        QCoreApplication::processEvents();
        REQUIRE(session.searchOptions.caseSensitivity == vtbackend::SearchCaseSensitivity::Insensitive);
        CHECK(toggle->property("checked").toBool());
        CHECK(toggle->property("text").toString() == QStringLiteral("aa"));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar hands its open state over when the pane is rebound (offscreen)",
          "[contour][gui][qml][search]")
{
    // SessionChrome re-targets on every tab switch and pane hand-off while the popup stays open, so
    // "which session was told the bar is open" is emphatically not "which session is bound now". Told-
    // ness has to be HANDED OVER: the session left behind would otherwise keep re-arming a walk of its
    // whole scrollback -- and, since screenUpdated() reads the same flag, keep its viewport pinned off
    // the bottom -- while the session arriving is never told at all and never shows a count.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession first;
    MockSession second;
    second.setSearchPattern(QStringLiteral("beta")); // the incoming pane is mid-search

    auto const host = openSearchBar(engine, first, &second);
    REQUIRE(host.bar != nullptr);
    auto* field = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarField"));
    REQUIRE(field != nullptr);

    REQUIRE(first.openNotifications == 1);
    REQUIRE(first.closeNotifications == 0);
    REQUIRE(second.openNotifications == 0);
    auto const secondPushes = second.setPatternCalls;

    // The tab switch, with the bar still standing open.
    host.window->setProperty("useAlternate", true);
    QCoreApplication::processEvents();

    CHECK(first.closeNotifications == 1);
    CHECK(second.openNotifications == 1);
    CHECK(second.closeNotifications == 0);

    // The term on screen belongs to the scrollback being searched, and re-seeding it must not bounce
    // back into the session it was just read from.
    CHECK(field->property("text").toString() == QStringLiteral("beta"));
    CHECK(second.setPatternCalls == secondPushes);

    // And closing now closes the session that actually holds the bar, not the one it started on.
    QMetaObject::invokeMethod(host.bar, "close");
    QCoreApplication::processEvents();
    CHECK(second.closeNotifications == 1);
    CHECK(first.closeNotifications == 1);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar sits at its pane's top right with the design's extents (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);

    CHECK(host.bar->property("x").toReal() == 412.0);
    CHECK(host.bar->property("y").toReal() == 8.0);
    CHECK(host.bar->property("width").toReal() == 380.0);
    CHECK(host.bar->property("height").toReal() == 32.0);
    auto* background = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarBackground"));
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == 8.0);
    // Lifted by e2: its soft shadow, in place of the style's popup shadow.
    auto const* elevation = providerOf(engine).property("elevation").value<QObject*>();
    REQUIRE(elevation != nullptr);
    CHECK(background->property("elevation").value<QObject*>() == elevation->property("e2").value<QObject*>());
    auto* soft = background->findChild<QQuickItem*>(QStringLiteral("elevationSoftShadow"));
    REQUIRE(soft != nullptr);
    CHECK(soft->isVisible());
    for (auto* toggle: searchToggleButtons(*host.bar))
    {
        REQUIRE(toggle != nullptr);
        CHECK(toggle->width() == 24.0);
        CHECK(toggle->height() == 22.0);
    }
    auto* count = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCount"));
    REQUIRE(count != nullptr);
    CHECK(count->property("font").value<QFont>().featureValue(QFont::Tag("tnum")) == 1);
    // The count and the first toggle are parted by the bar's spacing alone, s2.
    auto const* space = providerOf(engine).property("space").value<QObject*>();
    REQUIRE(space != nullptr);
    CHECK(countToToggleGap(*host.bar) == Catch::Approx(space->property("s2").toReal()));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar moves to the bottom when the current match is under it (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);

    // Row 0 is 0-20, under the bar's 8-40: the bar goes to the bottom right.
    session.setSearchFocusedRows({ { QStringLiteral("first"), 0 }, { QStringLiteral("last"), 0 } });
    QCoreApplication::processEvents();
    CHECK(host.bar->property("y").toReal() == 360.0);

    // Row 10 is 200-220, clear of it: back at the top.
    session.setSearchFocusedRows({ { QStringLiteral("first"), 10 }, { QStringLiteral("last"), 10 } });
    QCoreApplication::processEvents();
    CHECK(host.bar->property("y").toReal() == 8.0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar follows the current match when a zoom moves the rows (offscreen)",
          "[contour][gui][qml][search]")
{
    // Over the C++ stub, not a QML fake: a fake's rowBand() is a QML function whose reads the binding would
    // track by itself, so only a Q_INVOKABLE shows whether the bar re-reads the band on a zoom.
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(
        engine,
        contour::test::ChromeSetup { .row = contour::config::ChromeStyle::MacOS,
                                     .font = QGuiApplication::font(),
                                     .windowControls = contour::config::WindowControlStyle::Windows });
    MockSession session;
    engine.rootContext()->setContextProperty("mockSession", &session);
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Window\n"
                          "import Contour.Ui\n"
                          "import Contour.Terminal\n"
                          "Window {\n"
                          "  width: 800; height: 400; visible: true\n"
                          "  property alias display: displayItem\n"
                          "  property alias bar: barItem\n"
                          "  ContourTerminal { id: displayItem; anchors.fill: parent }\n"
                          "  SearchBar { id: barItem; session: mockSession; displayItem: displayItem }\n"
                          "}\n"),
        QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SearchBarZoomHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO("errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto const window = std::unique_ptr<QObject>(component.create());
    REQUIRE(window != nullptr);
    auto* display = qobject_cast<StubContourTerminal*>(window->property("display").value<QObject*>());
    auto* bar = window->property("bar").value<QObject*>();
    REQUIRE(display != nullptr);
    REQUIRE(bar != nullptr);
    QMetaObject::invokeMethod(bar, "open");
    QCoreApplication::processEvents();

    // Row 1 is 20-40 at 20-pixel rows, under the bar's 8-40: the bar goes to the bottom right.
    session.setSearchFocusedRows({ { QStringLiteral("first"), 1 }, { QStringLiteral("last"), 1 } });
    QCoreApplication::processEvents();
    CHECK(bar->property("y").toReal() == 360.0);

    // Zoomed to 50-pixel rows, row 1 is 50-100, clear of it: back at the top, with no step taken.
    display->setRowHeight(50.0);
    QCoreApplication::processEvents();
    CHECK(bar->property("y").toReal() == 8.0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar's toggles render the session's table and light the active one (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    session.searchOptions.syntax = vtbackend::SearchSyntax::Regex;
    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);

    auto const buttons = searchToggleButtons(*host.bar);
    REQUIRE(buttons.size() == contour::session::SearchToggleRows.size());
    auto const texts = std::array { QStringLiteral("Aa"), QStringLiteral(".*"), QStringLiteral("ab") };
    REQUIRE(texts.size() == buttons.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, buttons.size()))
    {
        REQUIRE(buttons[index] != nullptr);
        CHECK(buttons[index]->property("text").toString() == texts[index]);
    }

    auto* regex = buttons[static_cast<std::size_t>(contour::session::SearchToggle::Regex)];
    CHECK(regex->property("checked").toBool());
    auto* background = regex->findChild<QQuickItem*>(QStringLiteral("searchBarToggleBackground"));
    REQUIRE(background != nullptr);
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    CHECK(background->property("color").value<QColor>()
          == colors->property("findToggleOnFill").value<QColor>());

    // The lit toggle is ringed in the accent's mark ink, re-read when the palette changes (B-I4); an unlit
    // one is not ringed.
    auto const& provider = providerOf(engine);
    auto const ringOn = [&provider](QColor const& window) {
        return provider.statusMarkOn(static_cast<int>(contour::config::StatusColorRole::Running), window);
    };
    // A window in the accent itself puts the ring onto the window's ink, so the two rings differ.
    auto const light = QColor(0xF6, 0xF6, 0xF6);
    auto const accentWindow = ringOn(light);
    REQUIRE(ringOn(accentWindow) != ringOn(light));
    auto const saved = QGuiApplication::palette();
    for (auto const& window: { light, accentWindow })
    {
        INFO("window: " << window.name().toStdString());
        auto palette = saved;
        palette.setColor(QPalette::Window, window);
        QGuiApplication::setPalette(palette);
        QCoreApplication::processEvents();
        CHECK(background->property("border").value<QObject*>()->property("width").toReal() > 0.0);
        CHECK(background->property("border").value<QObject*>()->property("color").value<QColor>()
              == ringOn(window));
    }
    QGuiApplication::setPalette(saved);
    QCoreApplication::processEvents();
    auto* wholeWord = buttons[static_cast<std::size_t>(contour::session::SearchToggle::WholeWord)];
    auto* unlit = wholeWord->findChild<QQuickItem*>(QStringLiteral("searchBarToggleBackground"));
    REQUIRE(unlit != nullptr);
    CHECK_FALSE(wholeWord->property("checked").toBool());
    CHECK(unlit->property("border").value<QObject*>()->property("width").toReal() == 0.0);

    QMetaObject::invokeMethod(buttons[static_cast<std::size_t>(contour::session::SearchToggle::WholeWord)],
                              "clicked");
    QCoreApplication::processEvents();
    CHECK(session.toggledOptions == std::vector<int> { 2 });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar tints the field and says so on a miss and on an invalid pattern (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);
    auto* field = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarField"));
    auto* count = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCount"));
    REQUIRE(field != nullptr);
    REQUIRE(count != nullptr);
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const error = style->property("colors").value<QObject*>()->property("findErrorText").value<QColor>();

    for (auto const& summary: { QStringLiteral("No results"), QStringLiteral("Invalid pattern") })
    {
        INFO(summary.toStdString());
        session.setSearchSummary(
            summary, MockSession::SearchMatchPresence::None, MockSession::SearchNavigation::None);
        QCoreApplication::processEvents();
        CHECK(count->property("text").toString() == summary);
        CHECK(count->property("color").value<QColor>() == error);
        auto const* palette = field->property("palette").value<QObject*>();
        REQUIRE(palette != nullptr);
        CHECK(palette->property("highlight").value<QColor>() == error);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The find bar draws in cells on the Terminal row (offscreen)", "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    session.searchOptions.syntax = vtbackend::SearchSyntax::Regex;
    auto const chrome =
        contour::test::ChromeSetup { .row = contour::config::ChromeStyle::Terminal,
                                     .font = QGuiApplication::font(),
                                     .windowControls = contour::config::WindowControlStyle::Windows };
    auto const host = openSearchBar(engine, session, nullptr, chrome);
    REQUIRE(host.bar != nullptr);
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const cellHeight = style->property("cellHeight").toReal();
    auto const cellWidth = style->property("widthQuantum").toReal();

    CHECK(host.bar->property("height").toReal() == Catch::Approx(cellHeight));
    CHECK(host.bar->property("width").toReal() == Catch::Approx(48 * cellWidth));
    auto* background = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarBackground"));
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == 0.0);
    // Lifted by e2, which is a hard offset copy here, a cell's worth down and across.
    auto const* e2 = style->property("elevation").value<QObject*>()->property("e2").value<QObject*>();
    REQUIRE(e2 != nullptr);
    CHECK(background->property("elevation").value<QObject*>() == e2);
    auto* hard = background->findChild<QQuickItem*>(QStringLiteral("elevationHardShadow"));
    REQUIRE(hard != nullptr);
    CHECK(hard->isVisible());
    CHECK(e2->property("offsetX").toReal() == Catch::Approx(cellWidth));
    CHECK(e2->property("offsetY").toReal() == Catch::Approx(cellHeight));
    CHECK(hard->x() == e2->property("offsetX").toReal());
    CHECK(hard->y() == e2->property("offsetY").toReal());

    auto const buttons = searchToggleButtons(*host.bar);
    auto const texts = std::array { QStringLiteral("[Aa]"), QStringLiteral("[.*]"), QStringLiteral("[ab]") };
    REQUIRE(texts.size() == buttons.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, buttons.size()))
    {
        REQUIRE(buttons[index] != nullptr);
        CHECK(buttons[index]->property("text").toString() == texts[index]);
    }
    // "2 of 5" does not run into "[Aa]": one cell between them, to the pixel the layout snaps to.
    CHECK(countToToggleGap(*host.bar) == Catch::Approx(cellWidth).margin(1.0));
    // A lit toggle is its cells in inverse video, with no ring.
    auto* regex = buttons[static_cast<std::size_t>(contour::session::SearchToggle::Regex)];
    REQUIRE(regex->property("checked").toBool());
    auto* regexBackground = regex->findChild<QQuickItem*>(QStringLiteral("searchBarToggleBackground"));
    REQUIRE(regexBackground != nullptr);
    CHECK(regexBackground->property("border").value<QObject*>()->property("width").toReal() == 0.0);
    for (auto const& [name, glyph]: std::array { std::pair { "searchBarPrevious", QStringLiteral("↑") },
                                                 std::pair { "searchBarNext", QStringLiteral("↓") },
                                                 std::pair { "searchBarClose", QStringLiteral("×") } })
    {
        auto* button = host.bar->findChild<QQuickItem*>(QString::fromUtf8(name));
        REQUIRE(button != nullptr);
        CHECK(button->property("text").toString() == glyph);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the drift badge shows the model's text and confirms before restarting", "[contour][qml][drift]")
{
    auto fixture = DriftBadgeFixture {};
    CHECK_FALSE(fixture.badge->isVisible());

    fixture.model.show(
        vthost::client::assessDrift("0.8.2", "0.8.1", vthost::client::LayoutAfterRestart::Lost));
    CHECK(fixture.badge->isVisible());
    auto* label = fixture.badge->findChild<QObject*>(QStringLiteral("daemonDriftLabel"));
    REQUIRE(label != nullptr);
    CHECK(label->property("text").toString() == fixture.model.text());

    auto* dialog = fixture.dialog();
    REQUIRE(dialog != nullptr);
    CHECK_FALSE(dialog->property("visible").toBool());

    // A click opens the confirmation; nothing restarts until it is confirmed.
    auto spy = QSignalSpy { &fixture.model, &contour::window::DaemonDriftModel::restartRequested };
    fixture.click();
    CHECK(dialog->property("visible").toBool());
    CHECK(dialog->property("heading").toString() == QStringLiteral("Restart daemon on the new version"));
    CHECK(dialog->property("message").toString() == fixture.model.confirmation());
    CHECK(spy.count() == 0);

    // A KitDialog: its accept button is what confirms.
    CHECK(QString::fromLatin1(dialog->metaObject()->className()).startsWith(QStringLiteral("KitDialog")));
    REQUIRE(QMetaObject::invokeMethod(dialog, "accept"));
    CHECK(spy.count() == 1);
}

TEST_CASE("the drift badge ignores clicks while a restart runs", "[contour][qml][drift]")
{
    auto fixture = DriftBadgeFixture {};
    fixture.model.show(
        vthost::client::assessDrift("0.8.2", "0.8.1", vthost::client::LayoutAfterRestart::Lost));
    fixture.model.requestRestart();
    REQUIRE(fixture.model.isRestarting());

    fixture.click();
    CHECK_FALSE(fixture.dialog()->property("visible").toBool());
    auto* label = fixture.badge->findChild<QObject*>(QStringLiteral("daemonDriftLabel"));
    REQUIRE(label != nullptr);
    CHECK(label->property("text").toString() == QStringLiteral("Restarting daemon…"));
}

TEST_CASE("Banner: shows its text, and its buttons emit (offscreen)", "[contour][gui][qml][kit][banner]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto banner = createBanner(engine,
                               QVariantMap { { "text", QStringLiteral("Contour has new defaults") },
                                             { "actionText", QStringLiteral("Use New") },
                                             { "dismissText", QStringLiteral("Keep Mine") } });
    auto* item = qobject_cast<QQuickItem*>(banner.get());
    REQUIRE(item != nullptr);
    CHECK(item->isVisible());
    CHECK(item->height() > 0);

    auto* text = item->findChild<QObject*>(QStringLiteral("bannerText"));
    auto* action = item->findChild<QQuickItem*>(QStringLiteral("bannerAction"));
    auto* dismiss = item->findChild<QQuickItem*>(QStringLiteral("bannerDismiss"));
    REQUIRE(text != nullptr);
    REQUIRE(action != nullptr);
    REQUIRE(dismiss != nullptr);
    CHECK(text->property("text").toString() == QStringLiteral("Contour has new defaults"));
    CHECK(action->property("text").toString() == QStringLiteral("Use New"));
    CHECK(dismiss->property("text").toString() == QStringLiteral("Keep Mine"));

    QSignalSpy const actionSpy(banner.get(), SIGNAL(actionTriggered()));
    QSignalSpy const dismissedSpy(banner.get(), SIGNAL(dismissed()));
    REQUIRE(QMetaObject::invokeMethod(action, "clicked"));
    REQUIRE(QMetaObject::invokeMethod(dismiss, "clicked"));
    CHECK(actionSpy.count() == 1);
    CHECK(dismissedSpy.count() == 1);

    SECTION("no action text hides the action button")
    {
        item->setProperty("actionText", QString());
        CHECK_FALSE(action->isVisible());
        CHECK(dismiss->isVisible());
    }
    SECTION("no text hides the whole banner")
    {
        item->setProperty("text", QString());
        CHECK_FALSE(item->isVisible());
        CHECK(item->height() == 0);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Banner: the Terminal row is the content's height in the scheme's palette[0] (offscreen)",
          "[contour][gui][qml][kit][banner]")
{
    using contour::config::ChromeStyle;
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;

    auto setup = contour::test::defaultChromeSetup();
    setup.row = GENERATE(ChromeStyle::Terminal, ChromeStyle::MacOS);
    setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Bar)] =
        vtbackend::RGBColor { 0x2A2D35u };
    contour::test::installChromeStyle(engine, setup);

    auto banner =
        createBanner(engine, QVariantMap { { "text", QStringLiteral("Contour has new defaults") } });
    auto* item = qobject_cast<QQuickItem*>(banner.get());
    REQUIRE(item != nullptr);
    auto* content = item->property("contentItem").value<QQuickItem*>();
    auto* background = item->property("background").value<QQuickItem*>();
    REQUIRE(content != nullptr);
    REQUIRE(background != nullptr);

    auto const inset = chromeToken(engine, "space", "s3");
    CHECK(item->property("topPadding").toReal() == inset);
    CHECK(item->property("bottomPadding").toReal() == inset);
    CHECK(item->height() == item->implicitHeight());
    CHECK(item->height() == content->implicitHeight() + (2 * inset));

    if (setup.row == ChromeStyle::Terminal)
    {
        CHECK(inset == 0);
        CHECK(item->height() == content->implicitHeight());
        CHECK(background->property("color").value<QColor>() == QColor(0x2A, 0x2D, 0x35));
    }
    else
    {
        CHECK(inset > 0);
        CHECK(item->height() > content->implicitHeight());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Banner: the Terminal row's text and button labels read on its fill (offscreen)",
          "[contour][gui][qml][kit][banner]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark", "contour");
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, terminalRowSetup(schemeName));

    auto banner = createBanner(engine,
                               QVariantMap { { "text", QStringLiteral("Contour has new defaults") },
                                             { "actionText", QStringLiteral("Use New") } });
    auto* item = qobject_cast<QQuickItem*>(banner.get());
    REQUIRE(item != nullptr);
    auto* background = item->property("background").value<QQuickItem*>();
    auto* text = item->findChild<QObject*>(QStringLiteral("bannerText"));
    REQUIRE(background != nullptr);
    REQUIRE(text != nullptr);

    auto const fill = fillOf(background);
    CHECK(contrastOn(text, fill) >= 4.5);
    for (auto const* name: { "bannerAction", "bannerDismiss" })
    {
        INFO("button: " << name);
        auto* button = item->findChild<QQuickItem*>(QString::fromLatin1(name));
        REQUIRE(button != nullptr);
        auto* label = button->property("contentItem").value<QQuickItem*>();
        REQUIRE(label != nullptr);
        CHECK(contrastOn(label, fill) >= 4.5);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: a pill of the toast tokens (offscreen)", "[contour][gui][qml][kit][toast]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto toast = createToast(engine, QVariantMap { { "text", QStringLiteral("Copied 3 lines") } });
    auto* item = qobject_cast<QQuickItem*>(toast.get());
    REQUIRE(item != nullptr);
    auto* background = item->property("background").value<QQuickItem*>();
    auto* text = item->findChild<QObject*>(QStringLiteral("toastText"));
    REQUIRE(background != nullptr);
    REQUIRE(text != nullptr);

    CHECK(item->height() == chromeToken(engine, "size", "toast"));
    CHECK(background->property("radius").toReal() == chromeToken(engine, "radii", "toast"));
    CHECK(text->property("text").toString() == QStringLiteral("Copied 3 lines"));
    CHECK(item->implicitWidth()
          == item->property("implicitContentWidth").toReal() + item->property("leftPadding").toReal()
                 + item->property("rightPadding").toReal());
    CHECK(item->property("opacity").toReal() == 0.0);

    item->setProperty("shown", true);
    CHECK(QTest::qWaitFor([&] { return item->property("opacity").toReal() == 1.0; }, 2000));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: a host sets its extent, radius and detail (offscreen)", "[contour][gui][qml][kit][toast]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    // Shown, as the detail case below is: a toast that is not shown is invisible whole (Step 3c), which would
    // leave its detail hidden whatever the detail said.
    auto plain =
        createToast(engine, QVariantMap { { "text", QStringLiteral("Copied 3 lines") }, { "shown", true } });
    auto* plainItem = qobject_cast<QQuickItem*>(plain.get());
    REQUIRE(plainItem != nullptr);
    REQUIRE(QTest::qWaitFor([plainItem] { return plainItem->isVisible(); }));
    auto* plainDetail = plain->findChild<QQuickItem*>(QStringLiteral("toastDetail"));
    REQUIRE(plainDetail != nullptr);
    CHECK_FALSE(plainDetail->isVisible());

    auto toast = createToast(engine,
                             QVariantMap { { "text", QStringLiteral("<b>x</b>") },
                                           { "detail", QStringLiteral("<i>open</i>") },
                                           { "extent", 22.0 },
                                           { "cornerRadius", 11.0 },
                                           { "shown", true } });
    auto* item = qobject_cast<QQuickItem*>(toast.get());
    REQUIRE(item != nullptr);
    // A toast is visible only while it shows (or fades): Step 3c's hover fix.
    REQUIRE(QTest::qWaitFor([item] { return item->isVisible(); }));
    auto* background = item->property("background").value<QQuickItem*>();
    auto* text = item->findChild<QQuickItem*>(QStringLiteral("toastText"));
    auto* detail = item->findChild<QQuickItem*>(QStringLiteral("toastDetail"));
    REQUIRE(background != nullptr);
    REQUIRE(text != nullptr);
    REQUIRE(detail != nullptr);
    CHECK(item->height() == 22.0);
    CHECK(background->property("radius").toReal() == 11.0);
    CHECK(detail->isVisible());
    CHECK(detail->property("text").toString() == QStringLiteral("<i>open</i>"));
    // What an application chose is shown as written, never as markup.
    for (auto const* label: { text, detail })
        CHECK(label->property("textFormat").toInt() == Qt::PlainText);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: the Terminal row is a one-row inverse strip (offscreen)", "[contour][gui][qml][kit][toast]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    // A scheme whose two ends read on each other: its toast is their inverse (chromeSchemeColorsOf()).
    auto palette = vtbackend::ColorPalette {};
    palette.defaultForeground = vtbackend::RGBColor { 0xD9DCE3u };
    palette.defaultBackground = vtbackend::RGBColor { 0x1C1E24u };
    setup.scheme = contour::config::chromeSchemeColorsOf(palette);
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto toast = createToast(engine, QVariantMap { { "text", QStringLiteral("Copied 3 lines") } });
    auto* item = qobject_cast<QQuickItem*>(toast.get());
    REQUIRE(item != nullptr);
    auto* background = item->property("background").value<QQuickItem*>();
    auto* text = item->findChild<QObject*>(QStringLiteral("toastText"));
    REQUIRE(background != nullptr);
    REQUIRE(text != nullptr);

    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(item->height() == style->property("cellHeight").toReal());
    CHECK(background->property("radius").toReal() == 0.0);
    CHECK(background->property("color").value<QColor>() == QColor(0xD9, 0xDC, 0xE3));
    CHECK(text->property("color").value<QColor>() == QColor(0x1C, 0x1E, 0x24));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: the Terminal row's text reads on its fill (offscreen)", "[contour][gui][qml][kit][toast]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark", "contour");
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, terminalRowSetup(schemeName));

    auto toast = createToast(engine, QVariantMap { { "text", QStringLiteral("Copied 3 lines") } });
    auto const* item = qobject_cast<QQuickItem*>(toast.get());
    REQUIRE(item != nullptr);
    auto const* background = item->property("background").value<QQuickItem*>();
    auto const* text = item->findChild<QObject*>(QStringLiteral("toastText"));
    REQUIRE(background != nullptr);
    REQUIRE(text != nullptr);
    CHECK(contrastOn(text, fillOf(background)) >= 4.5);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: Reduce Motion fades without travel (offscreen)", "[contour][gui][qml][kit][toast]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto toast = createToast(engine, QVariantMap { { "text", QStringLiteral("Copied 3 lines") } });
    CHECK(toast->property("enterOffset").toReal() == 0.0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Toast: a hidden toast takes no hover from what lies beneath (offscreen)",
          "[contour][gui][qml][kit][toast]")
{
    // Main.qml's toast is always there, over the band's reveal edge in full screen. Hidden, it must let the
    // edge see the pointer, or the band never reveals. hoverEnabled is set as a desktop platform sets it;
    // the offscreen one's style hints leave it off, which would hide the fault.
    QQmlEngine engine;
    contour::test::installChromeStyle(engine, contour::test::defaultChromeSetup());
    contour::test::QmlMessageCapture const warnings;

    QQmlComponent component(&engine);
    component.setData("import QtQuick\n"
                      "import Contour.Ui\n"
                      "Item {\n"
                      "    width: 400; height: 200\n"
                      "    readonly property bool beneathHovered: beneath.hovered\n"
                      "    Item { anchors.fill: parent; HoverHandler { id: beneath } }\n"
                      "    Toast {\n"
                      "        objectName: \"hiddenToast\"; x: 0; y: 0; z: 1; text: \"Copied 3 lines\"\n"
                      "        hoverEnabled: true\n"
                      "    }\n"
                      "}\n",
                      QUrl());
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    DialogHost host;
    auto root = std::unique_ptr<QObject>(component.create());
    auto* item = qobject_cast<QQuickItem*>(root.get());
    REQUIRE(item != nullptr);
    item->setParentItem(host.window.contentItem());
    auto const* toast = item->findChild<QQuickItem*>(QStringLiteral("hiddenToast"));
    REQUIRE(toast != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return toast->width() > 0.0; }, 2000));

    auto const onToast = toast->mapToScene(QPointF(toast->width() / 2, toast->height() / 2)).toPoint();
    // Two moves, because a move to where the pointer already is sends nothing.
    QTest::mouseMove(&host.window, onToast + QPoint(0, 100));
    QTest::mouseMove(&host.window, onToast);
    CHECK(QTest::qWaitFor([&] { return item->property("beneathHovered").toBool(); }, 2000));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitDialog: a sheet on the macOS row, centred elsewhere (offscreen)",
          "[contour][gui][qml][kit][dialog]")
{
    using contour::config::ChromeStyle;
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = GENERATE(ChromeStyle::MacOS, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    DialogHost const host;
    auto dialog = openKitDialog(
        engine, host.window.contentItem(), QVariantMap { { "heading", QStringLiteral("Close this tab?") } });
    auto const overlayWidth = host.window.contentItem()->width();
    auto const overlayHeight = host.window.contentItem()->height();

    CHECK(dialog->property("width").toReal() == chromeToken(engine, "size", "dialogWidth"));
    CHECK(std::abs(dialog->property("x").toReal() - ((overlayWidth - dialog->property("width").toReal()) / 2))
          <= 1);
    if (setup.row == ChromeStyle::MacOS)
        CHECK(dialog->property("y").toReal() == 0);
    else
        CHECK(std::abs(dialog->property("y").toReal()
                       - ((overlayHeight - dialog->property("height").toReal()) / 2))
              <= 1);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitDialog: the heading is bold (offscreen)", "[contour][gui][qml][kit][dialog]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    DialogHost const host;
    auto dialog = openKitDialog(
        engine, host.window.contentItem(), QVariantMap { { "heading", QStringLiteral("Close this tab?") } });
    auto const* heading = dialog->findChild<QObject*>(QStringLiteral("kitDialogHeading"));
    REQUIRE(heading != nullptr);
    CHECK(heading->property("font").value<QFont>().weight() == QFont::Bold);
    // The body type otherwise: the bold is the only difference.
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const body = style->property("type").value<QObject*>()->property("body").value<QFont>();
    CHECK(heading->property("font").value<QFont>().pixelSize() == body.pixelSize());
}

TEST_CASE("KitDialog: accept and reject close it and emit (offscreen)", "[contour][gui][qml][kit][dialog]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    DialogHost const host;
    auto dialog = openKitDialog(engine,
                                host.window.contentItem(),
                                QVariantMap { { "heading", QStringLiteral("Close this tab?") },
                                              { "acceptText", QStringLiteral("Close Tab") } });
    auto* accept = dialog->findChild<QObject*>(QStringLiteral("kitDialogAccept"));
    auto* reject = dialog->findChild<QObject*>(QStringLiteral("kitDialogReject"));
    auto* heading = dialog->findChild<QObject*>(QStringLiteral("kitDialogHeading"));
    REQUIRE(accept != nullptr);
    REQUIRE(reject != nullptr);
    REQUIRE(heading != nullptr);
    CHECK(accept->property("text").toString() == QStringLiteral("Close Tab"));
    CHECK(heading->property("text").toString() == QStringLiteral("Close this tab?"));

    QSignalSpy const acceptedSpy(dialog.get(), SIGNAL(accepted()));
    QSignalSpy const rejectedSpy(dialog.get(), SIGNAL(rejected()));
    REQUIRE(QMetaObject::invokeMethod(accept, "clicked"));
    CHECK(acceptedSpy.count() == 1);
    CHECK(QTest::qWaitFor([&] { return !dialog->property("visible").toBool(); }, 2000));

    REQUIRE(QMetaObject::invokeMethod(dialog.get(), "open"));
    REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));
    REQUIRE(QMetaObject::invokeMethod(reject, "clicked"));
    CHECK(rejectedSpy.count() == 1);
    CHECK(acceptedSpy.count() == 1);
    CHECK(QTest::qWaitFor([&] { return !dialog->property("visible").toBool(); }, 2000));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitDialog: a destructive accept is painted in the danger colour (offscreen)",
          "[contour][gui][qml][kit][dialog]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);

    DialogHost const host;
    auto dialog =
        openKitDialog(engine,
                      host.window.contentItem(),
                      QVariantMap { { "heading", QStringLiteral("Close this tab?") },
                                    { "acceptRole", contour::window::UiStyleProvider::statusDanger() } });
    auto* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
    REQUIRE(accept != nullptr);
    auto const* background = accept->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("color").value<QColor>() == provider->statusColor(provider->statusDanger()));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitDialog: the Terminal row's heading and buttons read on its surface (offscreen)",
          "[contour][gui][qml][kit][dialog]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark", "contour");
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    QQmlEngine engine;
    contour::test::QmlMessageCapture const warnings;
    contour::test::installChromeStyle(engine, terminalRowSetup(schemeName));

    DialogHost const host;
    auto dialog = openKitDialog(engine,
                                host.window.contentItem(),
                                QVariantMap { { "heading", QStringLiteral("Close this tab?") },
                                              { "acceptText", QStringLiteral("Close Tab") } });
    auto const* surface = dialog->property("background").value<QQuickItem*>();
    auto const* heading = dialog->findChild<QObject*>(QStringLiteral("kitDialogHeading"));
    auto const* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
    auto const* reject = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogReject"));
    REQUIRE(surface != nullptr);
    REQUIRE(heading != nullptr);
    REQUIRE(accept != nullptr);
    REQUIRE(reject != nullptr);
    auto const surfaceFill = fillOf(surface);
    CHECK(contrastOn(heading, surfaceFill) >= 4.5);

    // The accept button carries a fill of its own, and labels on it; the reject button labels on the surface.
    auto const* acceptLabel = accept->property("contentItem").value<QQuickItem*>();
    auto const* acceptFill = accept->property("background").value<QQuickItem*>();
    REQUIRE(acceptLabel != nullptr);
    REQUIRE(acceptFill != nullptr);
    CHECK(contrastOn(acceptLabel, fillOf(acceptFill)) >= 4.5);
    auto const* rejectLabel = reject->property("contentItem").value<QQuickItem*>();
    REQUIRE(rejectLabel != nullptr);
    CHECK(contrastOn(rejectLabel, surfaceFill) >= 4.5);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a KitDialog without a reject text shows one button (offscreen)",
          "[contour][gui][qml][kit][dialog]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;
    DialogHost const host;

    SECTION("an empty reject text hides the reject button")
    {
        auto dialog = openKitDialog(
            engine,
            host.window.contentItem(),
            QVariantMap { { "heading", QStringLiteral("Contour") }, { "rejectText", QString {} } });
        auto const* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
        auto const* reject = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogReject"));
        REQUIRE(accept != nullptr);
        REQUIRE(reject != nullptr);
        CHECK(accept->isVisible());
        CHECK_FALSE(reject->isVisible());
    }

    SECTION("the default shows both")
    {
        auto dialog = openKitDialog(
            engine, host.window.contentItem(), QVariantMap { { "heading", QStringLiteral("Close?") } });
        auto const* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
        auto const* reject = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogReject"));
        REQUIRE(accept != nullptr);
        REQUIRE(reject != nullptr);
        CHECK(accept->isVisible());
        CHECK(reject->isVisible());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("ShowAbout opens the About dialog with the version (offscreen)", "[contour][gui][qml][about]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark", "contour");
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    QQmlEngine engine;
    contour::test::MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = contour::test::loadMainWindow(engine, controller, warnings, terminalRowSetup(schemeName));
    auto& window = *qobject_cast<QQuickWindow*>(root.get());
    window.resize(1024, 640);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));

    auto const* dialog = root->findChild<QObject*>(QStringLiteral("aboutDialog"));
    REQUIRE(dialog != nullptr);
    CHECK_FALSE(dialog->property("opened").toBool());
    controller.showAbout();
    REQUIRE(QTest::qWaitFor([&] { return dialog->property("opened").toBool(); }, 2000));

    auto const* message = dialog->findChild<QObject*>(QStringLiteral("kitDialogMessage"));
    auto const* heading = dialog->findChild<QObject*>(QStringLiteral("kitDialogHeading"));
    auto const* accept = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogAccept"));
    auto const* reject = dialog->findChild<QQuickItem*>(QStringLiteral("kitDialogReject"));
    REQUIRE(message != nullptr);
    REQUIRE(heading != nullptr);
    REQUIRE(accept != nullptr);
    REQUIRE(reject != nullptr);
    CHECK(message->property("text").toString().contains(QStringLiteral("0.0.0-test")));
    CHECK_FALSE(reject->isVisible());

    // Centred in the overlay, as the Terminal row places every dialog.
    auto const overlayHeight = window.contentItem()->height();
    CHECK(
        std::abs(dialog->property("y").toReal() - ((overlayHeight - dialog->property("height").toReal()) / 2))
        <= 1);

    // Every About text reads on the surface it sits on.
    auto const* surface = dialog->property("background").value<QQuickItem*>();
    REQUIRE(surface != nullptr);
    auto const surfaceFill = fillOf(surface);
    CHECK(contrastOn(heading, surfaceFill) >= 4.5);
    CHECK(contrastOn(message, surfaceFill) >= 4.5);
    auto const* acceptLabel = accept->property("contentItem").value<QQuickItem*>();
    auto const* acceptFill = accept->property("background").value<QQuickItem*>();
    REQUIRE(acceptLabel != nullptr);
    REQUIRE(acceptFill != nullptr);
    CHECK(contrastOn(acceptLabel, fillOf(acceptFill)) >= 4.5);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row draws swatches as two-cell blocks (offscreen)", "[contour][gui][qml][menus]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller { 2 };
    auto const setup = terminalRowSetup("contour-dark");
    auto const host = makeTabStripHost(engine, controller, setup);
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);

    auto* menu = openTabMenu(*host, controller, 1);
    auto const colors = openTabColorMenu(*menu);
    REQUIRE_FALSE(colors.swatches.empty());

    auto const swatchRows = colors.row->property("swatches").toList();
    auto const foreground = setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Foreground)];
    auto checked = 0;
    for (auto const [index, swatch]: core::views::enumerate(colors.swatches))
    {
        INFO("swatch " << index);
        CHECK(swatch->width() == Catch::Approx(2 * provider->cellWidth()));
        CHECK(swatch->height() == Catch::Approx(provider->cellHeight()));
        CHECK(swatch->property("radius").toReal() == 0.0);

        if (!swatchRows[static_cast<qsizetype>(index)].toMap().value(QStringLiteral("checked")).toBool())
            continue;
        ++checked;
        auto const* border = swatch->property("border").value<QObject*>();
        REQUIRE(border != nullptr);
        CHECK(border->property("width").toReal() > 0.0);
        auto const ring = contour::platform::toRGBColor(border->property("color").value<QColor>());
        CHECK(ring == foreground);
        // Never the 1:1 the banner once read at: the ring stands off the colour it rings.
        CHECK(vtbackend::contrastRatio(ring, fillOf(swatch)) > 1.0);
    }
    CHECK(checked == 1);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row's menu rows read on the menu surface (offscreen)", "[contour][gui][qml][menus]")
{
    // Every colour a row paints comes from the application palette, never from a style's default: a
    // KitDialog label that kept one read at under 2:1 on the dark palette.
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark", "contour");
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller { 2 };
    auto const host = makeTabStripHost(engine, controller, terminalRowSetup(schemeName));

    auto* menu = openTabMenu(*host, controller, 1);
    auto const* surface = menu->property("background").value<QObject*>();
    REQUIRE(surface != nullptr);
    auto const surfaceFill = fillOf(surface);

    auto enabledRows = 0;
    auto disabledRows = 0;
    for (auto* item: menuItems(*menu))
    {
        auto const title = item->property("text").toString();
        if (title.isEmpty())
            continue; // a separator
        INFO("row: " << title.toStdString());
        auto const* label = item->property("contentItem").value<QObject*>();
        REQUIRE(label != nullptr);
        if (item->isEnabled())
        {
            ++enabledRows;
            CHECK(contrastOn(label, surfaceFill) >= 4.5);
            if (auto const* arrow = item->property("arrow").value<QQuickItem*>(); arrow && arrow->isVisible())
                CHECK(contrastOn(arrow, surfaceFill) >= 4.5);
        }
        else
        {
            // A disabled row is exempt from the ratio, but still the palette's disabled text.
            ++disabledRows;
            CHECK(label->property("color").value<QColor>().rgb()
                  == QGuiApplication::palette().color(QPalette::Disabled, QPalette::WindowText).rgb());
        }
    }
    CHECK(enabledRows > 0);
    CHECK(disabledRows > 0); // "Close Tabs to the Right" over the last tab

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("pane end: an ended pane shows the footer below its cells (offscreen)",
          "[contour][gui][qml][paneend]")
{
    // Wide enough for each half's footer to say its message, Restart, and Close with its chord.
    auto host = PaneTreeHost { paneCardSetup(), QSize { 1200, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    auto const& card = host.cardOf(panes.left());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    CHECK_FALSE(footer.isVisible());

    endWithExitCode(panes.leftSession);
    host.settle();
    REQUIRE(footer.isVisible());
    CHECK(footer.height() == 34);
    CHECK(footer.y() + footer.height() == card.height());

    // The cells end where the footer starts: it is never over them.
    auto const& terminal = PaneTreeHost::terminalOf(card);
    CHECK(terminal.mapToItem(&card, QPointF { 0, terminal.height() }).y() == footer.y());
    CHECK(PaneTreeHost::partOf(card, "exitedFooterMessage").property("text").toString()
          == QStringLiteral("Process exited with code 1."));

    auto const& restart = PaneTreeHost::partOf(card, "exitedFooterRestart");
    CHECK(restart.isVisible());
    clickOn(host, restart);
    CHECK(panes.leftSession.footerActions == 1);

    auto const& close = PaneTreeHost::partOf(card, "exitedFooterClose");
    CHECK(close.property("text").toString() == QStringLiteral("Close Ctrl+Shift+X"));
    clickOn(host, close);
    CHECK(panes.leftSession.footerDismissals == 1);
    CHECK(panes.leftSession.footerActions == 1);

    // The other pane runs on, footless.
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "exitedFooter").isVisible());
}

TEST_CASE("pane end: a Disconnected pane offers no Restart (offscreen)", "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);

    session.setEnd(static_cast<int>(contour::session::EndReason::Disconnected),
                   QStringLiteral("Disconnected from the Contour daemon."),
                   static_cast<int>(contour::session::FooterAction::None));
    host.settle();
    auto const& card = host.cardOf(leaf);
    CHECK(PaneTreeHost::partOf(card, "exitedFooter").isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(card, "exitedFooterRestart").isVisible());
    CHECK(PaneTreeHost::partOf(card, "exitedFooterClose").isVisible());
}

TEST_CASE(
    "pane end: a restored shell's footer names the stopped command with Run Again and Dismiss (offscreen)",
    "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 1200, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    auto const& card = host.cardOf(panes.left());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    CHECK_FALSE(footer.isVisible());

    auto const message = QStringLiteral("\u201cmake test\u201d was stopped when the session ended.");
    panes.leftSession.setInterrupted(message, QStringLiteral("Run Again"));
    host.settle();
    REQUIRE(footer.isVisible());
    // Below the cells, as an ended pane's is.
    auto const& terminal = PaneTreeHost::terminalOf(card);
    CHECK(terminal.mapToItem(&card, QPointF { 0, terminal.height() }).y() == footer.y());
    CHECK(PaneTreeHost::partOf(card, "exitedFooterMessage").property("text").toString() == message);

    // Return belongs to the shell: no key is promised, so the button shows no glyph.
    auto const& primary = PaneTreeHost::partOf(card, "exitedFooterRestart");
    REQUIRE(primary.isVisible());
    CHECK(primary.property("text").toString() == QStringLiteral("Run Again"));
    clickOn(host, primary);
    CHECK(panes.leftSession.footerActions == 1);

    auto const& dismiss = PaneTreeHost::partOf(card, "exitedFooterClose");
    CHECK(dismiss.property("text").toString() == QStringLiteral("Dismiss"));
    clickOn(host, dismiss);
    CHECK(panes.leftSession.footerDismissals == 1);

    // Return is not mentioned to assistive technology either, nor a chord Dismiss does not have.
    auto const description = footer.property("description").toString();
    CHECK_FALSE(description.contains(QStringLiteral("Return")));
    CHECK_FALSE(description.contains(QStringLiteral("Ctrl+Shift+X")));

    // Before the shell shows a prompt there is nothing to run: Dismiss alone, no dead button.
    panes.leftSession.setInterrupted(message, QString {});
    host.settle();
    CHECK_FALSE(primary.isVisible());
    CHECK(dismiss.isVisible());
}

TEST_CASE("pane end: the Terminal row's restored-shell footer brackets its buttons (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    panes.leftSession.setInterrupted(QStringLiteral("\u201cmake test\u201d was stopped."),
                                     QStringLiteral("Run Again"));
    host.settle();

    auto const& card = host.cardOf(panes.left());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    REQUIRE(footer.isVisible());
    CHECK(footer.height() == host.style().cellHeight());
    CHECK(PaneTreeHost::partOf(card, "exitedFooterRestart").property("text").toString()
          == QStringLiteral("[Run Again]"));
    CHECK(PaneTreeHost::partOf(card, "exitedFooterClose").property("text").toString()
          == QStringLiteral("[Dismiss]"));
}

TEST_CASE("pane end: a Disconnected pane offers Reconnect, answered by Return (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 1200, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);

    session.setEnd(static_cast<int>(contour::session::EndReason::Disconnected),
                   QStringLiteral("Disconnected from the Contour daemon. Its sessions are still running."),
                   static_cast<int>(contour::session::FooterAction::Reconnect));
    host.settle();
    auto const& card = host.cardOf(leaf);
    auto const& primary = PaneTreeHost::partOf(card, "exitedFooterRestart");
    REQUIRE(primary.isVisible());
    CHECK(primary.property("text").toString()
          == QStringLiteral("Reconnect %1").arg(host.style().restartGlyph()));
    clickOn(host, primary);
    CHECK(session.footerActions == 1);
    CHECK(PaneTreeHost::partOf(card, "exitedFooterClose").isVisible());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    auto const* const attached = [&]() -> QObject* {
        for (auto* child: footer.children())
            if (child->inherits("QQuickAccessibleAttached"))
                return child;
        return nullptr;
    }();
    REQUIRE(attached != nullptr);
    CHECK(attached->property("description").toString().contains(QStringLiteral("Return presses Reconnect.")));
}

TEST_CASE("pane end: the footer reads at 4.5:1 (offscreen)", "[contour][gui][qml][paneend]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark");
    auto const row = GENERATE(contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal);
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    CAPTURE(static_cast<int>(row));
    contour::test::ScopedChromeAppearance const forced { appearance };
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
    auto setup = terminalRowSetup(schemeName);
    setup.row = row;
    auto const background = contour::platform::toQColor(palette.defaultBackground);

    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    MockSession session { background };
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    endWithExitCode(session);
    host.settle();

    auto const& card = host.cardOf(leaf);
    auto const footerFill = over(colorOf(PaneTreeHost::partOf(card, "exitedFooterFill")), background);
    CHECK(contrastOf(colorOf(PaneTreeHost::partOf(card, "exitedFooterMessage")), footerFill) >= 4.5);
    for (auto const* name: { "exitedFooterRestart", "exitedFooterClose" })
    {
        INFO("button: " << name);
        auto const& button = PaneTreeHost::partOf(card, name);
        auto const fill = over(colorOf(button), footerFill);
        auto const& label = PaneTreeHost::partOf(button, (std::string { name } + "Label").c_str());
        CHECK(contrastOf(colorOf(label), fill) >= 4.5);
        // And under the pointer, the footer's wash laid over the fill (native rows); pressed, the button's
        // pressed fill.
        if (row != contour::config::ChromeStyle::Terminal)
        {
            auto const hovered = over(colorOf(PaneTreeHost::partOf(card, "exitedFooterFill")), fill);
            CHECK(contrastOf(colorOf(label), hovered) >= 4.5);
        }
        auto const pressed = over(button.property("pressedFill").value<QColor>(), footerFill);
        CHECK(contrastOf(colorOf(label), pressed) >= 4.5);
    }
}

TEST_CASE("pane end: a reconnecting pane dims below its header and shows the pill (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto const contrast = GENERATE(contour::config::ContrastPreference::Standard,
                                   contour::config::ContrastPreference::Increased);
    CAPTURE(static_cast<int>(contrast));
    auto setup = paneCardSetup();
    setup.accessibility.contrast = contrast;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    auto const& card = host.cardOf(panes.left());
    auto const& overlay = PaneTreeHost::partOf(card, "reconnectingOverlay");
    auto const& pill = PaneTreeHost::partOf(card, "reconnectingPill");
    CHECK_FALSE(overlay.isVisible());

    panes.leftSession.setReconnecting(true);
    QCoreApplication::processEvents();
    REQUIRE(overlay.isVisible());
    CHECK(colorOf(overlay) == host.style().paneDimColor());
    if (contrast == contour::config::ContrastPreference::Increased)
        CHECK(colorOf(overlay).alphaF() == Catch::Approx(0.05).margin(0.005));
    // Over the grid only: the header above it stays as it is.
    auto const& header = PaneTreeHost::partOf(card, "paneHeader");
    CHECK(overlay.y() == header.y() + header.height());
    CHECK(overlay.y() + overlay.height() == card.height());

    CHECK(pill.isVisible());
    CHECK(pill.property("text").toString() == QStringLiteral("Reconnecting to home…"));
    // The Working badge's glyph, drawn still: nothing in the pill animates it.
    auto const look = host.style().badgeLook(static_cast<int>(contour::window::TabBadgeKind::Working));
    CHECK(pill.property("glyph").toString() == look.value("glyph").toString());
    auto const& glyph = PaneTreeHost::partOf(pill, "toastGlyph");
    // In the warning mark ink of the host chip's Reconnecting dot, on the pill's own fill.
    auto const warning = static_cast<int>(contour::config::StatusColorRole::Warning);
    auto const& hues =
        contour::config::StatusColors[static_cast<size_t>(warning)].hues(contour::config::StatusInkUse::Mark);
    CHECK((colorOf(glyph) == QColor::fromRgb(hues.dark) || colorOf(glyph) == QColor::fromRgb(hues.light)));
    CHECK(std::ranges::none_of(glyph.findChildren<QObject*>(), [](QObject const* child) {
        return child->inherits("QQuickAbstractAnimation");
    }));
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "reconnectingOverlay").isVisible());

    panes.leftSession.setReconnecting(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(overlay.isVisible());
    CHECK_FALSE(pill.isVisible());
}

TEST_CASE("pane end: the Terminal row's footer is one cell row with bracketed buttons (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    endWithExitCode(panes.leftSession);
    host.settle();

    auto const& card = host.cardOf(panes.left());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    REQUIRE(footer.isVisible());
    CHECK(footer.height() == host.style().cellHeight());
    CHECK(PaneTreeHost::partOf(card, "exitedFooterRestart").property("text").toString()
          == QStringLiteral("[Restart %1]").arg(host.style().restartGlyph()));
    CHECK(PaneTreeHost::partOf(card, "exitedFooterClose").property("text").toString()
          == QStringLiteral("[Close Ctrl+Shift+X]"));
}

TEST_CASE("pane end: a narrow pane's footer keeps its buttons inside the pane (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto setup = paneCardSetup();
    auto const terminalRow = GENERATE(false, true);
    if (terminalRow)
        setup.row = contour::config::ChromeStyle::Terminal;
    CAPTURE(terminalRow);
    // Two panes in a window too narrow for Restart and "Close Ctrl+Shift+X" side by side.
    auto host = PaneTreeHost { setup, QSize { 360, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    endWithExitCode(panes.rightSession);
    host.settle();

    auto const& card = host.cardOf(panes.right());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    REQUIRE(footer.isVisible());
    CHECK(footer.clip());
    // Close drops its chord rather than push Restart past the pane's leading edge.
    auto const& close = PaneTreeHost::partOf(card, "exitedFooterClose");
    CHECK_FALSE(close.property("text").toString().contains(QStringLiteral("Ctrl")));
    auto const& restart = PaneTreeHost::partOf(card, "exitedFooterRestart");
    CHECK(restart.mapToItem(&footer, QPointF {}).x() >= 0);

    // Wide enough again, it names the chord.
    host.resize(QSize { 1200, 400 });
    CHECK(close.property("text").toString().contains(QStringLiteral("Ctrl+Shift+X")));
}

TEST_CASE("pane end: a narrow pane's footer drops Close's chord before its message (offscreen)",
          "[contour][gui][qml][paneend]")
{
    // The order of sacrifice is the chord, then the message; the buttons stay. A pane wide enough for the
    // message, Restart and a plain "Close" -- but not for "Close Ctrl+Shift+X" besides -- shows the
    // message whole and Close without its chord, which assistive technology still hears.
    auto host = PaneTreeHost { paneCardSetup(), QSize { 1200, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    endWithExitCode(session);
    host.settle();

    auto const& card = host.cardOf(leaf);
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    auto const& message = PaneTreeHost::partOf(card, "exitedFooterMessage");
    auto const& restart = PaneTreeHost::partOf(card, "exitedFooterRestart");
    auto const& close = PaneTreeHost::partOf(card, "exitedFooterClose");
    REQUIRE(footer.isVisible());
    REQUIRE(close.property("text").toString().contains(QStringLiteral("Ctrl+Shift+X")));
    auto const chordedClose = close.width();
    auto const messageWidth = message.implicitWidth();

    // Close's plain width, read where nothing but the buttons fits.
    host.resize(QSize { 120, 400 });
    REQUIRE_FALSE(close.property("text").toString().contains(QStringLiteral("Ctrl")));
    auto const plainClose = close.width();

    auto const* space = host.style().property("space").value<QObject*>();
    REQUIRE(space != nullptr);
    auto const tokenOf = [space](char const* name) {
        return space->property(name).toReal();
    };
    auto const s4 = tokenOf("s4");
    auto const room =
        tokenOf("footerLeading") + messageWidth + s4 + restart.width() + s4 + plainClose + tokenOf("s5") + 2;
    REQUIRE(room - plainClose + chordedClose > room + 1); // the chord would not fit beside the message
    auto const chromeWidth = 120 - footer.width();
    host.resize(QSize { static_cast<int>(std::ceil(room + chromeWidth)), 400 });
    REQUIRE(footer.width() >= room);

    CHECK_FALSE(close.property("text").toString().contains(QStringLiteral("Ctrl")));
    CHECK(message.isVisible());
    CHECK_FALSE(message.property("truncated").toBool());
    CHECK(message.width() >= messageWidth);
    CHECK(restart.isVisible());
    CHECK(accessibleNameOf(footer).contains(QStringLiteral("exited")));
}

TEST_CASE("pane end: the footer's buttons answer hover and press as the kit's buttons do (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 1200, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);
    endWithExitCode(session);
    host.settle();

    auto const& card = host.cardOf(leaf);
    auto const background = session.property("backgroundColor").value<QColor>();
    auto const roleOn = [&host, &background](contour::config::ChromeColorRole role) {
        return host.style().colorOn(static_cast<int>(role), background);
    };
    for (auto const* name: { "exitedFooterRestart", "exitedFooterClose" })
    {
        INFO("button: " << name);
        auto const& button = PaneTreeHost::partOf(card, name);
        auto const& layer = PaneTreeHost::partOf(button, (std::string { name } + "State").c_str());
        // The footer may still be sliding in after the cards settle: take the pointer's spot only once the
        // button's centre lies inside the window, or the move lands outside it and hovers nothing.
        auto const centreOf = [&button] {
            return button.mapToScene(QPointF { button.width() / 2, button.height() / 2 }).toPoint();
        };
        auto const windowRect = QRect { QPoint {}, host.window.size() };
        REQUIRE(
            QTest::qWaitFor([&] { return windowRect.contains(centreOf()); }, PaneTreeHost::SettleTimeoutMs));
        auto const centre = centreOf();
        CHECK(colorOf(layer).alpha() == 0); // at rest, the button is its fill alone

        // Under the pointer, the footer's wash once more -- on the native rows; the Terminal row's opaque
        // bar wash would hide a bracketed button's own fill.
        QTest::mouseMove(&host.window, centre);
        QCoreApplication::processEvents();
        if (PaneTreeHost::partOf(card, "exitedFooter").property("bracketed").toBool())
        {
            CHECK(colorOf(layer).alpha() == 0);
        }
        else
        {
            auto const hovered = roleOn(contour::config::ChromeColorRole::PaneFooterFill);
            (void) QTest::qWaitFor([&] { return colorOf(layer) == hovered; }, PaneTreeHost::SettleTimeoutMs);
            CHECK(colorOf(layer) == hovered);
        }

        // Pressed, the hover layer goes and the button shows its pressed fill: Close the strongest fill in
        // place of its wash, Restart its accent.
        auto const restFill = colorOf(button);
        auto const pressedFill = std::string_view { name } == "exitedFooterClose"
                                     ? roleOn(contour::config::ChromeColorRole::PaneHeaderActionHover)
                                     : restFill;
        QTest::mousePress(&host.window, Qt::LeftButton, {}, centre);
        QCoreApplication::processEvents();
        (void) QTest::qWaitFor([&] { return colorOf(layer).alpha() == 0 && colorOf(button) == pressedFill; },
                               PaneTreeHost::SettleTimeoutMs);
        CHECK(colorOf(layer).alpha() == 0);
        CHECK(colorOf(button) == pressedFill);
        QTest::mouseRelease(&host.window, Qt::LeftButton, {}, centre);
        QCoreApplication::processEvents();

        QTest::mouseMove(&host.window, QPoint { 2, 2 });
        QCoreApplication::processEvents();
        (void) QTest::qWaitFor([&] { return colorOf(layer).alpha() == 0; }, PaneTreeHost::SettleTimeoutMs);
        CHECK(colorOf(layer).alpha() == 0);
    }
}

TEST_CASE("pane end: a local reconnect names no host (offscreen)", "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    // The local daemon (and --connect-tcp) attaches with no host label, and no host chip shows.
    host.window.controller().setHostLabel({});
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    panes.leftSession.setReconnecting(true);
    QCoreApplication::processEvents();

    auto const& pill = PaneTreeHost::partOf(host.cardOf(panes.left()), "reconnectingPill");
    REQUIRE(pill.isVisible());
    CHECK(pill.property("text").toString() == QStringLiteral("Reconnecting…"));
    CHECK(accessibleNameOf(pill) == QStringLiteral("Reconnecting…"));
}

TEST_CASE("pane end: the reconnecting dim lies under the header and never over an ended pane (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto setup = paneCardSetup();
    setup.density = contour::config::ChromeDensity::Minimal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2, static_cast<int>(contour::window::HeaderReveal::HeldModifier));
    auto& card = host.cardOf(panes.left());
    auto const& overlay = PaneTreeHost::partOf(card, "reconnectingOverlay");
    auto const& header = PaneTreeHost::partOf(card, "paneHeader");
    REQUIRE(header.isVisible());

    // Minimal's revealed header lies over the grid; the dim lies under the header, so its buttons stay
    // as they are.
    panes.leftSession.setReconnecting(true);
    QCoreApplication::processEvents();
    REQUIRE(overlay.isVisible());
    auto const children = card.childItems();
    auto const indexOf = [&](QQuickItem const& item) {
        return std::ranges::find(children, &item) - children.begin();
    };
    CHECK((overlay.z() < header.z() || (overlay.z() == header.z() && indexOf(overlay) < indexOf(header))));

    // A pane that ended before its connection dropped keeps its footer undimmed and says nothing more.
    endWithExitCode(panes.leftSession);
    QCoreApplication::processEvents();
    CHECK_FALSE(overlay.isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(card, "reconnectingPill").isVisible());
}

TEST_CASE("pane end: the pill casts a shadow only where the row's shadow is soft (offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto const row = GENERATE(contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    auto setup = paneCardSetup();
    setup.row = row;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    panes.leftSession.setReconnecting(true);
    QCoreApplication::processEvents();

    // The pill is a steady state in the pane, not a popover: the Terminal row's hard offset copy would
    // read as a second, empty row beneath it.
    auto const& pill = PaneTreeHost::partOf(host.cardOf(panes.left()), "reconnectingPill");
    REQUIRE(pill.isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(pill, "elevationHardShadow").isVisible());
    CHECK(PaneTreeHost::partOf(pill, "elevationSoftShadow").isVisible()
          == (row == contour::config::ChromeStyle::MacOS));
}

TEST_CASE("pane end: the footer tells assistive technology its keys, and a click on it activates the pane "
          "(offscreen)",
          "[contour][gui][qml][paneend]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    // Nothing asks for a chord while no footer shows.
    CHECK(host.window.controller().shortcutTextCalls == 0);

    endWithExitCode(panes.rightSession);
    host.settle();
    auto const& card = host.cardOf(panes.right());
    auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
    REQUIRE(footer.isVisible());
    CHECK(host.window.controller().shortcutTextCalls > 0);

    auto const* attached = [&]() -> QObject* {
        for (auto* child: footer.children())
            if (child->inherits("QQuickAccessibleAttached"))
                return child;
        return nullptr;
    }();
    REQUIRE(attached != nullptr);
    auto const description = attached->property("description").toString();
    CHECK(description.contains(QStringLiteral("Return")));
    CHECK(description.contains(QStringLiteral("Ctrl+Shift+X")));

    // A click beside the buttons makes the ended pane the active one, so Return then reaches it.
    auto const before = panes.right().activateCalls;
    clickOn(host, PaneTreeHost::partOf(card, "exitedFooterMessage"));
    CHECK(panes.right().activateCalls == before + 1);
    CHECK(panes.rightSession.footerActions == 0);
}

TEST_CASE("pane end: the pill's mark reads on its own fill (offscreen)", "[contour][gui][qml][paneend]")
{
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const schemeName = GENERATE(as<std::string_view> {}, "contour-light", "contour-dark");
    auto const row = GENERATE(contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal);
    INFO("Qt palette: " << appearanceName(appearance));
    INFO("scheme: " << schemeName);
    CAPTURE(static_cast<int>(row));
    contour::test::ScopedChromeAppearance const forced { appearance };
    auto setup = terminalRowSetup(schemeName);
    setup.row = row;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);
    panes.leftSession.setReconnecting(true);
    QCoreApplication::processEvents();

    // A mark, so WCAG 1.4.11's 3:1 against the pill it sits on, whichever way the row inks the pill.
    auto const& pill = PaneTreeHost::partOf(host.cardOf(panes.left()), "reconnectingPill");
    auto const* background = pill.property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(contrastOf(colorOf(PaneTreeHost::partOf(pill, "toastGlyph")), colorOf(*background))
          >= contour::config::MinimumMarkContrast);
}

// }}}

// {{{ The palette over the find bar: closing hands the keyboard back to the bar, not the terminal.
TEST_CASE("Use Selection for Find over an open find bar shows the new pattern (offscreen)",
          "[contour][gui][qml][search]")
{
    // I1: ⌘E with the bar already open installs the selection as the pattern; the field must say so, not keep
    // what it held before.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    MockSession session;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    engine.rootContext()->setContextProperty("mockSession", &session);
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  width: 800; height: 400; visible: true\n"
                                        "  property alias chrome: chromeItem\n"
                                        "  Item {\n"
                                        "    anchors.fill: parent\n"
                                        "    SessionChrome { id: chromeItem; session: mockSession }\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SearchSelectionHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO("errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto const window = std::unique_ptr<QObject>(component.create());
    REQUIRE(window != nullptr);
    auto* chrome = window->property("chrome").value<QObject*>();
    REQUIRE(chrome != nullptr);
    auto* bar = chrome->findChild<QObject*>(QStringLiteral("searchBar"));
    REQUIRE(bar != nullptr);

    session.searchSelection(QStringLiteral("foo"));
    QCoreApplication::processEvents();
    REQUIRE(bar->property("opened").toBool());
    auto* field = bar->findChild<QQuickItem*>(QStringLiteral("searchBarField"));
    REQUIRE(field != nullptr);
    REQUIRE(field->property("text").toString() == QStringLiteral("foo"));

    session.searchSelection(QStringLiteral("bar"));
    QCoreApplication::processEvents();
    CHECK(field->property("text").toString() == QStringLiteral("bar"));
    CHECK(session.searchPattern() == QStringLiteral("bar"));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Closing the palette over the find bar gives the find bar its focus back (offscreen)",
          "[contour][gui][qml][palette][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    MockPaletteController controller;
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("mockSession", &session);
    engine.rootContext()->setContextProperty("paletteController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  id: host\n"
                                        "  width: 800; height: 600; visible: true\n"
                                        "  property int focusRestores: 0\n"
                                        "  property alias searchBarItem: barItem\n"
                                        "  property alias commandPaletteItem: paletteItem\n"
                                        "  function restoreTerminalFocus() { host.focusRestores++; }\n"
                                        "  Item {\n"
                                        "    id: fakeDisplay\n"
                                        "    anchors.fill: parent\n"
                                        "  }\n"
                                        "  SearchBar {\n"
                                        "    id: barItem\n"
                                        "    session: mockSession\n"
                                        "    displayItem: fakeDisplay\n"
                                        "  }\n"
                                        "  CommandPalette {\n"
                                        "    id: paletteItem\n"
                                        "    controller: paletteController\n"
                                        "    window: host\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaletteOverFindBarHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO("errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto const window = std::unique_ptr<QObject>(component.create());
    REQUIRE(window != nullptr);
    auto* bar = window->property("searchBarItem").value<QObject*>();
    auto* palette = window->property("commandPaletteItem").value<QObject*>();
    REQUIRE(bar != nullptr);
    REQUIRE(palette != nullptr);
    auto* quickWindow = qobject_cast<QQuickWindow*>(window.get());
    REQUIRE(quickWindow != nullptr);

    QMetaObject::invokeMethod(bar, "open");
    QCoreApplication::processEvents();
    auto* field = bar->findChild<QQuickItem*>(QStringLiteral("searchBarField"));
    REQUIRE(field != nullptr);
    QMetaObject::invokeMethod(field, "forceActiveFocus");
    QCoreApplication::processEvents();
    REQUIRE(quickWindow->activeFocusItem() == field);

    QMetaObject::invokeMethod(palette, "open");
    waitForPopupOpened(*palette);
    QMetaObject::invokeMethod(palette, "close");
    waitForPopupClosed(*palette);

    CHECK(quickWindow->activeFocusItem() == field);
    CHECK(window->property("focusRestores").toInt() == 0);

    SECTION("a find bar that closed under the palette hands over to the terminal")
    {
        QMetaObject::invokeMethod(palette, "open");
        waitForPopupOpened(*palette);
        QMetaObject::invokeMethod(bar, "close");
        waitForPopupClosed(*bar);
        auto const before = window->property("focusRestores").toInt();
        QMetaObject::invokeMethod(palette, "close");
        waitForPopupClosed(*palette);
        CHECK(window->property("focusRestores").toInt() == before + 1);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("PermissionDialog: the dialog shows the session's question (offscreen)",
          "[contour][gui][qml][permission]")
{
    auto const capture = contour::test::QmlMessageCapture {};
    MockSession session;
    auto host = PermissionChrome { session };

    session.setPendingPermission(pasteQuestion(1));
    REQUIRE(host.waitOpened(true));
    CHECK(host.part("kitDialogHeading")->property("text").toString() == "Paste 2.4 MB into “zsh”?");
    auto const lines = permissionLines(*host.dialog);
    REQUIRE(lines.size() == 3);
    for (auto const* line: lines | std::views::take(2))
        CHECK(line->property("elide").toInt() == Qt::ElideRight);
    CHECK(lines.back()->property("text").toString() == "… 48211 more lines");
    CHECK(host.part("kitDialogReject")->property("text").toString() == "Cancel");
    CHECK(host.part("kitDialogAccept")->property("text").toString() == "Paste");
    CHECK(capture.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("PermissionDialog: every way to answer sends one answer, naming the question (offscreen)",
          "[contour][gui][qml][permission]")
{
    enum class Way : uint8_t
    {
        Accept,
        Reject,
        Escape,
    };
    enum class Box : uint8_t
    {
        Unticked,
        Ticked,
    };
    struct AnswerRow
    {
        Way way;
        Box box;
        PermissionDecision decision;
        PermissionMemory memory;
    };
    auto const row =
        GENERATE(AnswerRow { Way::Accept, Box::Unticked, PermissionDecision::Allow, PermissionMemory::Once },
                 AnswerRow { Way::Accept, Box::Ticked, PermissionDecision::Allow, PermissionMemory::Session },
                 AnswerRow { Way::Reject, Box::Unticked, PermissionDecision::Deny, PermissionMemory::Once },
                 AnswerRow { Way::Reject, Box::Ticked, PermissionDecision::Deny, PermissionMemory::Session },
                 AnswerRow { Way::Escape, Box::Ticked, PermissionDecision::Deny, PermissionMemory::Once });
    CAPTURE(static_cast<int>(row.way), static_cast<int>(row.box));

    MockSession session;
    auto host = PermissionChrome { session };
    constexpr auto QuestionId = 7;
    session.setPendingPermission(captureQuestion(QuestionId));
    REQUIRE(host.waitOpened(true));
    host.part("permissionRemember")->setProperty("checked", row.box == Box::Ticked);

    switch (row.way)
    {
        case Way::Accept: REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogAccept"), "clicked")); break;
        case Way::Reject: REQUIRE(QMetaObject::invokeMethod(host.part("kitDialogReject"), "clicked")); break;
        case Way::Escape: QTest::keyClick(&host.window, Qt::Key_Escape); break;
    }
    REQUIRE(host.waitOpened(false));
    // A second way out of the same dialog -- the reject that follows a button, a stray close -- answers
    // nothing more.
    REQUIRE(QMetaObject::invokeMethod(host.dialog, "rejected"));
    REQUIRE(QMetaObject::invokeMethod(host.dialog, "accepted"));
    REQUIRE(QMetaObject::invokeMethod(host.dialog, "close"));
    QCoreApplication::processEvents();

    CHECK(session.permissionAnswers == 1);
    REQUIRE(session.lastPermissionAnswer.has_value());
    CHECK(session.lastPermissionAnswer->id == QuestionId);
    CHECK(session.lastPermissionAnswer->decision == row.decision);
    CHECK(session.lastPermissionAnswer->memory == row.memory);
}

TEST_CASE("the title bar draws no window controls until its controller is bound (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // Main.qml's `win` is null until Component.onCompleted mints the controller, and the bar's
    // `useCustomWindowControls` flips on in the same pass: a WindowControls created in between would read
    // `controller.nativeMaximizeHovered` and report its caption rectangles to null.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, contour::test::defaultChromeSetup());

    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  property var ctl: null\n"
                                       "  width: 900; height: 400; visible: true\n"
                                       "  TitleBar {\n"
                                       "    objectName: \"titleBar\"\n"
                                       "    width: parent.width\n"
                                       "    controller: host.ctl\n"
                                       "    window: host\n"
                                       "    useCustomWindowControls: true\n"
                                       "  }\n"
                                       "}\n");
    auto const host = buildQmlHost(engine, source, QStringLiteral("TitleBarNullControllerHost.qml"));
    auto const* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);

    auto const controlsLoaded = [&] {
        auto const* const controls = host->findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
        return controls != nullptr && !controls->childItems().empty();
    };
    auto const trailing = [&]() -> QQuickItem* {
        return host->findChild<QQuickItem*>(QStringLiteral("trailingWindowControls"));
    };
    auto const* before = trailing();
    REQUIRE(before != nullptr);
    CHECK(before->childItems().isEmpty());
    CHECK_FALSE(controlsLoaded());

    host->setProperty("ctl", QVariant::fromValue<QObject*>(&controller));
    QCoreApplication::processEvents();
    REQUIRE(trailing() != nullptr);
    CHECK_FALSE(trailing()->childItems().isEmpty());

    // The controller going away (window teardown) takes the controls with it, quietly.
    host->setProperty("ctl", QVariant::fromValue<QObject*>(nullptr));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the find bar's buttons split their label from their shortcut (offscreen)",
          "[contour][gui][qml][kit][tooltip][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSession session;
    auto const host = openSearchBar(engine, session);
    REQUIRE(host.bar != nullptr);
    auto const style = contour::command::platformShortcutStyle();

    struct Case
    {
        char const* button;
        char const* label;
        char const* chord;
    };
    for (auto const& row: { Case { "searchBarPrevious", "Previous match", "FindPrevious" },
                            Case { "searchBarNext", "Next match", "FindNext" },
                            Case { "searchBarClose", "Close", "FindClose" } })
    {
        CAPTURE(row.button);
        auto* const button = host.bar->findChild<QQuickItem*>(QString::fromLatin1(row.button));
        REQUIRE(button != nullptr);
        auto* const toolTipHost = button->findChild<QObject*>(QStringLiteral("kitToolTipHost"));
        REQUIRE(toolTipHost != nullptr);
        CHECK(toolTipHost->property("text").toString() == QString::fromLatin1(row.label));
        auto const expected = contour::command::findBarShortcutText(row.chord, style);
        REQUIRE(expected.has_value());
        CHECK(toolTipHost->property("shortcut").toString() == QString::fromStdString(*expected));
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
