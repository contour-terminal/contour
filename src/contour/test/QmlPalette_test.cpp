// SPDX-License-Identifier: Apache-2.0
//
// QML component tests, continued: the command palette, the Save Layout prompt, the link pill and the window
// controls.

#include <contour/command/Shortcut.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/LinkPillPlacement.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>

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
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

/// The palette part named @p name (a QObject child of the popup), or null.
[[nodiscard]] QQuickItem* palettePart(PaletteHost const& host, QString const& name)
{
    return host.palette->findChild<QQuickItem*>(name);
}

/// The realized delegate of list row @p index, or null where the view has not built it.
[[nodiscard]] QQuickItem* paletteRowAt(QQuickItem* list, int index)
{
    QQuickItem* rowItem = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, rowItem), Q_ARG(int, index));
    return rowItem;
}

/// The model row whose `commandId` is @p id, or -1. Rows are found by id, never by rank.
[[nodiscard]] int paletteRowOf(contour::window::CommandPaletteModel const& model, QString const& id)
{
    for (auto const row: std::views::iota(0, model.rowCount()))
        if (model
                .data(model.index(row, 0),
                      static_cast<int>(contour::window::CommandPaletteModel::Roles::IdRole))
                .toString()
            == id)
            return row;
    return -1;
}

/// Role @p role of model row @p row, as a string.
[[nodiscard]] QString paletteRole(contour::window::CommandPaletteModel const& model,
                                  int row,
                                  contour::window::CommandPaletteModel::Roles role)
{
    return model.data(model.index(row, 0), static_cast<int>(role)).toString();
}

/// Gives the filter field the keyboard, as opening the palette does, and returns its window.
[[nodiscard]] QQuickWindow* focusPaletteField(PaletteHost const& host)
{
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    QMetaObject::invokeMethod(filter, "forceActiveFocus");
    QCoreApplication::processEvents();
    REQUIRE(filter->window() != nullptr);
    return filter->window();
}

/// Presses @p key in the palette's window and lets the event loop settle.
void pressPaletteKey(QQuickWindow* window, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QTest::keyClick(window, key, modifiers);
    QCoreApplication::processEvents();
}

/// The MacOS row, which the canvas's numbers are given for.
[[nodiscard]] contour::test::ChromeSetup macPaletteSetup()
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    return setup;
}

/// The enter transition's animation named @p name, or null.
[[nodiscard]] QObject* paletteEnterAnimation(PaletteHost const& host, QString const& name)
{
    return host.palette->findChild<QObject*>(name);
}

/// An 800 × 400 SessionChrome on @p row, its session naming a link target the follow modifier shows.
struct LinkPillScene
{
    QQmlEngine engine;
    MockTabController controller;
    std::unique_ptr<MockSession> session = std::make_unique<MockSession>();
    ChromeHost host;
    QQuickItem* pill {};

    explicit LinkPillScene(contour::config::ChromeStyle row = contour::config::ChromeStyle::MacOS)
    {
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        auto setup = contour::test::defaultChromeSetup();
        setup.row = row;
        contour::test::installChromeStyle(engine, setup);
        host = createChromeInWindow(engine, QSize { 800, 400 });
        session->setOverlayClearance(linkClearance(100, 20, 100));
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        QCoreApplication::processEvents();
        pill = host.chrome->findChild<QQuickItem*>(QStringLiteral("linkPill"));
        REQUIRE(pill != nullptr);
    }

    /// Shows the target with the hint, and waits until the pill has faded in.
    void show(bool shown = true) const
    {
        session->setLinkTarget(
            QStringLiteral("github.com › contour-terminal"), QStringLiteral("⌘-click to open"), shown);
        QCoreApplication::processEvents();
    }
};

/// A target far wider than any header slot, so a pill in one is cut.
[[nodiscard]] QString longLinkTarget()
{
    return QStringLiteral(
        "github.com › contour-terminal › contour › issues › 2175 › comments › a-very-long-anchor-name");
}

/// How a posted gesture finishes.
enum class GestureEnd : std::uint8_t
{
    ButtonStillDown, //!< Stop after the last move, leaving the gesture in flight.
    Released,        //!< Release the button, completing the gesture.
};

/// Drags the pointer across @p window with every event QUEUED first, then delivered in one go.
///
/// The queueing is not incidental. An automatic QDrag (TabItem's dragProxy) runs QDrag::exec() — a NESTED
/// event loop — from inside the very move event that activates its DragHandler. A test that sent the moves
/// synchronously (QTest::mouseMove) and only afterwards the release would therefore block forever: it
/// cannot send the release that the nested loop is waiting to receive. Posting everything up front leaves
/// the release already in the queue, so even a build that wrongly starts a drag completes the gesture
/// instead of hanging the suite. For that reason every assertion below drives a RELEASED gesture and reads
/// what happened from signal spies, rather than inspecting a gesture still in flight.
///
/// @param window The window to deliver to.
/// @param button The button held down for the drag.
/// @param from   Press position, in window coordinates.
/// @param to     Final move position, in window coordinates; one intermediate move is interpolated.
/// @param end    Whether to release the button at @p to.
void postGesture(
    QQuickWindow& window, Qt::MouseButton button, QPoint const& from, QPoint const& to, GestureEnd end)
{
    auto const post =
        [&](QEvent::Type type, QPoint const& pos, Qt::MouseButton eventButton, Qt::MouseButtons held) {
            QCoreApplication::postEvent(
                &window,
                new QMouseEvent(type, pos, window.mapToGlobal(pos), eventButton, held, Qt::NoModifier));
        };
    post(QEvent::MouseButtonPress, from, button, button);
    post(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, button);
    post(QEvent::MouseMove, to, Qt::NoButton, button);
    if (end == GestureEnd::Released)
        post(QEvent::MouseButtonRelease, to, button, Qt::NoButton);
    QCoreApplication::processEvents();
}

/// Skips the running test unless it runs on the offscreen platform, which ctest always selects.
///
/// For a test that presses a ResizeBorder zone: the zone hands the press to QWindow::startSystemResize(),
/// which on a real windowing system gives the pointer to the window manager -- on Windows a modal sizing
/// loop that waits for the user's own mouse or keyboard, which posted events never reach -- so the test
/// would hang until someone at the desk ended it. Offscreen, the platform declines the resize and the
/// press goes on to the items beneath, which is the overlap these tests are about.
void skipUnlessOffscreen()
{
    if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
        SKIP("presses a resize zone, which a real windowing system hands to the user's own pointer");
}

/// The resize border's hit zones in @p host, which all eight share one objectName for.
/// @param host The host window to search.
/// @return Its four edge zones and four corner zones.
[[nodiscard]] QList<QQuickItem*> resizeZonesOf(QObject& host)
{
    auto zones = host.findChildren<QQuickItem*>(QStringLiteral("resizeZone"));
    REQUIRE(zones.size() == 8); // four edges, four corners
    return zones;
}

/// Whether @p scenePoint lies inside one of the resize border's eight hit zones.
///
/// This is the NON-VACUITY check every assertion below needs: "no drag started" is equally true of a
/// gesture that missed the resize border altogether, which would prove nothing at all. Asserting the
/// GEOMETRY rather than the delivery on purpose — whether a MouseArea reports `pressed` around a
/// startSystemResize() call made from its own onPressed is a Qt implementation detail that differs
/// between 6.10 and 6.11, while the overlap this fix is about is a plain fact about the scene.
///
/// @param host       The host window to search.
/// @param scenePoint A point in window coordinates.
/// @return Whether a zone covers it.
[[nodiscard]] bool isOverResizeBorder(QObject& host, QPoint const& scenePoint)
{
    auto const zones = resizeZonesOf(host);
    return std::ranges::any_of(
        zones, [&](QQuickItem const* zone) { return sceneRectOf(*zone).contains(scenePoint); });
}

/// Hosts a REAL TabItem under the REAL ResizeBorder, at the window origin — which is where the tab strip
/// puts the first tab, and therefore where the top-left corner handle lands on it.
///
/// The tab is placed directly rather than through a TabStrip: a ListView realizes its delegates lazily
/// against a viewport, which offscreen leaves the tab absent and every assertion vacuous. What is under
/// test is two items' hit zones overlapping, and a delegate adds nothing to it.
///
/// @param engine     The engine to build in.
/// @param controller The mock the tab binds to.
/// @return The host Window; the tab is reachable as its `tabItem` property.
[[nodiscard]] QmlHost makeTabItemUnderResizeBorderHost(QQmlEngine& engine, MockTabController& controller)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: 400; height: 300; visible: true\n"
                                       "  property alias tabItem: item\n"
                                       "  TabItem {\n"
                                       "    id: item\n"
                                       "    x: 0; y: 0\n"
                                       "%1"
                                       "  }\n"
                                       "  ResizeBorder { window: host }\n"
                                       "}\n")
                            .arg(QLatin1StringView(TabItemBindings));

    return buildQmlHost(engine, source, QStringLiteral("TabItemBorderTestHost.qml"));
}

/// What one chrome style produces for the component under test, read out while its engine is still
/// alive -- the provider is parented to that engine, so nothing may outlive the call that made it.
struct ChromeMetrics
{
    qreal value; //!< The component geometry the test is about.
    qreal cellWidth;
    qreal cellHeight;
    qreal widthQuantum;
};

} // namespace

TEST_CASE("The palette survives controller destruction without QML errors (offscreen)",
          "[contour][gui][qml][palette]")
{
    // Same teardown hazard TabStrip guards against: the C++ controller is destroyed before the QML tree
    // on window close, and QML re-evaluates every dependent binding against null on the way out. An
    // unguarded `controller.` in the popup would raise a TypeError — which the run-wide gate turns into
    // a failure of the entire suite, not just this test.
    contour::test::QmlMessageCapture const capture;
    QQmlEngine engine;
    auto controller = std::make_unique<MockPaletteController>();

    auto host = openPalette(engine, *controller);
    REQUIRE(host.palette != nullptr);

    host.palette->setProperty("controller", QVariant::fromValue<QObject*>(nullptr));
    QCoreApplication::processEvents();
    engine.rootContext()->setContextProperty("paletteController", nullptr);
    controller.reset();
    QCoreApplication::processEvents();

    CHECK(capture.count([](QString const& m) { return m.contains(QStringLiteral("TypeError")); }) == 0);

    host.window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

TEST_CASE("The save-layout prompt opens on the controller's request (offscreen)",
          "[contour][gui][qml][layout]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSaveLayoutController controller;

    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    REQUIRE_FALSE(host.opened()); // nothing has asked for it yet

    openSaveLayoutPrompt(controller);
    CHECK(host.opened());
    CHECK(host.nameField() != nullptr);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Accepting the save-layout prompt saves the typed name and gives the keyboard back (offscreen)",
          "[contour][gui][qml][layout]")
{
    // The typed name has to route all the way to the controller — the field is decoration otherwise — and
    // the terminal has to get the keyboard back, or the user is left typing into a dismissed prompt.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSaveLayoutController controller;

    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(host.opened());
    REQUIRE(host.nameField() != nullptr);

    host.nameField()->setProperty("text", QStringLiteral("dev"));
    QMetaObject::invokeMethod(host.dialog, "pressDefault");
    QCoreApplication::processEvents();

    REQUIRE(controller.saved.size() == 1);
    CHECK(controller.saved.front() == QStringLiteral("dev"));
    CHECK_FALSE(host.opened());
    CHECK(host.focusRestores() == 1);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A blank save-layout name is a no-op that keeps the prompt open (offscreen)",
          "[contour][gui][qml][layout]")
{
    // Enter on an empty (or whitespace-only) field must neither save a nameless layout nor silently
    // dismiss the dialog — the user is asked to type a name or press Escape.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSaveLayoutController controller;

    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(host.opened());
    REQUIRE(host.nameField() != nullptr);

    host.nameField()->setProperty("text", QStringLiteral("   "));
    QMetaObject::invokeMethod(host.dialog, "pressDefault");
    QCoreApplication::processEvents();

    CHECK(controller.saved.isEmpty());
    CHECK(host.opened()); // still up, nothing saved
    CHECK_FALSE(host.part(QStringLiteral("kitDialogAccept"))->property("enabled").toBool());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Dismissing the save-layout prompt saves nothing and gives the keyboard back (offscreen)",
          "[contour][gui][qml][layout]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSaveLayoutController controller;

    auto host = makeSaveLayoutHost(engine, controller);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(controller);
    REQUIRE(host.opened());

    QMetaObject::invokeMethod(host.dialog, "close");
    QCoreApplication::processEvents();

    CHECK(controller.saved.isEmpty());
    CHECK_FALSE(host.opened());
    CHECK(host.focusRestores() == 1);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The save-layout prompt survives controller destruction without QML errors (offscreen)",
          "[contour][gui][qml][layout]")
{
    // Same teardown hazard the command palette guards against: the C++ controller is destroyed before the
    // QML tree on window close, and an unguarded `controller.` in the dialog would raise a TypeError that
    // the run-wide gate turns into a failure of the whole suite.
    contour::test::QmlMessageCapture const capture;
    QQmlEngine engine;
    auto controller = std::make_unique<MockSaveLayoutController>();

    auto host = makeSaveLayoutHost(engine, *controller);
    REQUIRE(host.dialog != nullptr);
    openSaveLayoutPrompt(*controller);

    host.dialog->setProperty("controller", QVariant::fromValue<QObject*>(nullptr));
    QCoreApplication::processEvents();
    engine.rootContext()->setContextProperty("saveLayoutController", nullptr);
    controller.reset();
    QCoreApplication::processEvents();

    CHECK(capture.count([](QString const& m) { return m.contains(QStringLiteral("TypeError")); }) == 0);

    host.window.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

TEST_CASE("Ctrl+J and Ctrl+K walk the palette selection, vim-style (offscreen)",
          "[contour][gui][qml][palette]")
{
    // The keyboard-only path: Ctrl+J/Ctrl+K move the selection down/up without the caret ever leaving
    // the filter box, mirroring the arrow keys the palette already handled.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    auto* list = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    REQUIRE(list->property("count").toInt() > 2);

    // The chords are handled on the filter field, so the caret must be there — as it is on open. Key
    // events route to the window's focus item, which forceActiveFocus makes the filter.
    QMetaObject::invokeMethod(filter, "forceActiveFocus");
    QCoreApplication::processEvents();
    REQUIRE(list->property("currentIndex").toInt() == 0);
    auto* const keyWindow = filter->window();
    REQUIRE(keyWindow != nullptr);

    QTest::keyClick(keyWindow, Qt::Key_J, Qt::ControlModifier);
    QCoreApplication::processEvents();
    CHECK(list->property("currentIndex").toInt() == 1);

    QTest::keyClick(keyWindow, Qt::Key_J, Qt::ControlModifier);
    QCoreApplication::processEvents();
    CHECK(list->property("currentIndex").toInt() == 2);

    QTest::keyClick(keyWindow, Qt::Key_K, Qt::ControlModifier);
    QCoreApplication::processEvents();
    CHECK(list->property("currentIndex").toInt() == 1);

    // The chord drives the list, it must not leak a character into the filter text.
    CHECK(filter->property("text").toString().isEmpty());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Matched characters are bold, in the row's own colour (offscreen)", "[contour][gui][qml][palette]")
{
    // The title label renders StyledText so the filter's matches stand out: bold on every row, and never
    // tinted (an accent tint does not read on the light surface with the default accent).
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    auto* list = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);

    // "spl" keeps several title-matched rows, the two Split commands on top; row 0 is the selection.
    filter->setProperty("text", QStringLiteral("spl"));
    QCoreApplication::processEvents();
    QMetaObject::invokeMethod(list, "forceLayout");
    QCoreApplication::processEvents();
    REQUIRE(list->property("count").toInt() > 1);
    REQUIRE(list->property("currentIndex").toInt() == 0);

    auto titleTextAt = [&](int index) -> QString {
        QQuickItem* rowItem = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, rowItem), Q_ARG(int, index));
        if (rowItem == nullptr)
            return {};
        auto* title = rowItem->findChild<QQuickItem*>(QStringLiteral("commandPaletteTitle"));
        return title != nullptr ? title->property("text").toString() : QString {};
    };

    auto const selectedTitle = titleTextAt(0);   // the current row
    auto const unselectedTitle = titleTextAt(1); // another matched row
    INFO("row0='" << selectedTitle.toStdString() << "' row1='" << unselectedTitle.toStdString() << "'");
    REQUIRE_FALSE(selectedTitle.isEmpty());
    REQUIRE_FALSE(unselectedTitle.isEmpty());

    CHECK(selectedTitle.contains(QStringLiteral("<b>")));
    CHECK(unselectedTitle.contains(QStringLiteral("<b>")));
    CHECK_FALSE(selectedTitle.contains(QStringLiteral("<font color")));
    CHECK_FALSE(unselectedTitle.contains(QStringLiteral("<font color")));

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The palette is 640 wide, 18 % down, with 32-tall rows (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller, macPaletteSetup(), QSize { 1280, 800 });
    REQUIRE(host.palette != nullptr);

    CHECK(host.palette->property("width").toReal() == 640.0);
    CHECK(host.palette->property("x").toReal() == 320.0);
    CHECK(host.palette->property("y").toReal() == 144.0);
    CHECK(host.palette->property("height").toReal() <= 420.0);
    CHECK(host.palette->property("height").toReal() > 0.0);

    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    auto* first = paletteRowAt(list, 0);
    REQUIRE(first != nullptr);
    auto* entry = first->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
    REQUIRE(entry != nullptr);
    CHECK(entry->height() == 32.0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Rows are grouped, and only the first row of a group draws a header (offscreen)",
          "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);

    // The groups the model lists, in order: Recently used (the mock remembers TogglePaneZoom), then the
    // table's groups that have a row.
    auto expected = QStringList {};
    auto const& model = controller.model();
    for (auto const row: std::views::iota(0, model.rowCount()))
        if (auto title = paletteRole(model, row, contour::window::CommandPaletteModel::Roles::GroupTitleRole);
            !title.isEmpty())
            expected << title;
    REQUIRE(expected.size() > 2);
    CHECK(expected.at(0) == QString::fromUtf8(contour::command::RecentGroupTitle));
    CHECK(expected.at(1) == QString::fromUtf8(contour::command::PaletteGroups.front().title));

    // The visible headers are a prefix of that sequence: a virtualized list builds only what it shows.
    auto shown = QStringList {};
    for (auto const row: std::views::iota(0, list->property("count").toInt()))
    {
        auto* item = paletteRowAt(list, row);
        if (item == nullptr)
            continue;
        auto* header = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteGroupHeader"));
        REQUIRE(header != nullptr);
        if (header->isVisible())
        {
            CHECK(header->height() > 0.0);
            shown << item->property("groupTitle").toString();
        }
        else
            CHECK(header->height() == 0.0);
    }
    REQUIRE_FALSE(shown.isEmpty());
    CHECK(shown == expected.mid(0, shown.size()));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A group header is drawn semibold at the footnote size (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    auto* first = paletteRowAt(list, 0);
    REQUIRE(first != nullptr);
    auto* label = first->findChild<QQuickItem*>(QStringLiteral("commandPaletteGroupHeaderLabel"));
    REQUIRE(label != nullptr);

    // A copy of the footnote token with its weight set in QML keeps the token's weight; the header reads
    // the semibold token instead.
    auto const font = label->property("font").value<QFont>();
    CHECK(font.weight() == QFont::DemiBold);
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const footnote = style->property("type").value<QObject*>()->property("footnote").value<QFont>();
    CHECK(font.pixelSize() == footnote.pixelSize());
    CHECK(font.pointSizeF() == footnote.pointSizeF());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The footer shows the selected row's description only (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* description = palettePart(host, QStringLiteral("commandPaletteFooterDescription"));
    auto* hints = palettePart(host, QStringLiteral("commandPaletteFooterHints"));
    REQUIRE(list != nullptr);
    REQUIRE(description != nullptr);
    REQUIRE(hints != nullptr);

    using Roles = contour::window::CommandPaletteModel::Roles;
    auto const& model = controller.model();
    REQUIRE(list->property("currentIndex").toInt() == 0);
    CHECK(description->property("text").toString() == paletteRole(model, 0, Roles::DescriptionRole));
    CHECK_FALSE(description->property("text").toString().isEmpty());
    CHECK(
        hints->property("text").toString()
        == paletteRole(controller.model(), 0, contour::window::CommandPaletteModel::Roles::FooterHintsRole));

    pressPaletteKey(focusPaletteField(host), Qt::Key_Down);
    REQUIRE(list->property("currentIndex").toInt() == 1);
    CHECK(description->property("text").toString() == paletteRole(model, 1, Roles::DescriptionRole));

    // No row repeats it: the footer is the one place a description shows.
    CHECK(host.palette->findChildren<QQuickItem*>(QStringLiteral("commandPaletteDescription")).isEmpty());

    SECTION("the shortcut stays on the row")
    {
        controller.model().setFilter(QStringLiteral("SplitVertical"));
        QCoreApplication::processEvents();
        auto const row = paletteRowOf(model, QStringLiteral("SplitVertical"));
        REQUIRE(row >= 0);
        auto* item = paletteRowAt(list, row);
        REQUIRE(item != nullptr);
        auto* shortcut = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteShortcut"));
        REQUIRE(shortcut != nullptr);
        CHECK(shortcut->property("text").toString()
              == QString::fromStdString(contour::test::paneChordText(contour::actions::SplitVertical {})));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A drill-in row shows its glyph and Enter opens its level (offscreen)",
          "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    auto* level = palettePart(host, QStringLiteral("commandPaletteLevel"));
    REQUIRE(list != nullptr);
    REQUIRE(filter != nullptr);
    REQUIRE(level != nullptr);
    auto const drillInGlyph = providerOf(engine).drillInGlyph();

    filter->setProperty("text", QStringLiteral("profile"));
    QCoreApplication::processEvents();
    auto const parent = paletteRowOf(controller.model(), QStringLiteral("ChangeProfile"));
    REQUIRE(parent >= 0);
    list->setProperty("currentIndex", parent);
    QCoreApplication::processEvents();
    auto* parentItem = paletteRowAt(list, parent);
    REQUIRE(parentItem != nullptr);
    auto* glyph = parentItem->findChild<QQuickItem*>(QStringLiteral("commandPaletteDrillIn"));
    REQUIRE(glyph != nullptr);
    CHECK(glyph->isVisible());
    CHECK(glyph->property("text").toString() == drillInGlyph);
    CHECK_FALSE(level->isVisible());

    auto* window = focusPaletteField(host);
    pressPaletteKey(window, Qt::Key_Return);

    CHECK(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));
    CHECK(level->isVisible());
    CHECK(level->property("text").toString() == QStringLiteral("Change Profile ") + drillInGlyph);
    CHECK(filter->property("text").toString().isEmpty());
    REQUIRE(list->property("count").toInt() == 2);
    using Roles = contour::window::CommandPaletteModel::Roles;
    CHECK(paletteRole(controller.model(), 0, Roles::TitleRole) == QStringLiteral("dark"));
    CHECK(paletteRole(controller.model(), 1, Roles::TitleRole) == QStringLiteral("light"));
    CHECK(list->property("currentIndex").toInt() == 0);
    CHECK(controller.ran.isEmpty());

    pressPaletteKey(window, Qt::Key_Return);
    waitForPopupClosed(*host.palette);
    CHECK(controller.ran == QStringList { QStringLiteral("ChangeProfile:dark") });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Escape in a level goes back one level, then closes (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(list != nullptr);
    REQUIRE(filter != nullptr);

    filter->setProperty("text", QStringLiteral("profile"));
    QCoreApplication::processEvents();
    list->setProperty("currentIndex", paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
    auto* window = focusPaletteField(host);
    pressPaletteKey(window, Qt::Key_Return);
    REQUIRE(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));

    pressPaletteKey(window, Qt::Key_Escape);
    CHECK(host.palette->property("opened").toBool());
    CHECK(controller.model().depth() == controller.model().topDepth());
    CHECK(filter->property("text").toString() == QStringLiteral("profile"));
    CHECK(list->property("currentIndex").toInt()
          == paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));

    pressPaletteKey(window, Qt::Key_Escape);
    waitForPopupClosed(*host.palette);
    CHECK(controller.ran.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Backspace in an empty field leaves the level (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);

    controller.model().enter(paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));
    auto* window = focusPaletteField(host);

    SECTION("with text, Backspace edits the text")
    {
        pressPaletteKey(window, Qt::Key_D);
        REQUIRE(filter->property("text").toString() == QStringLiteral("d"));
        pressPaletteKey(window, Qt::Key_Backspace);
        CHECK(filter->property("text").toString().isEmpty());
        CHECK(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));
    }

    SECTION("empty, Backspace goes back")
    {
        pressPaletteKey(window, Qt::Key_Backspace);
        CHECK(controller.model().depth() == controller.model().topDepth());
        CHECK(host.palette->property("opened").toBool());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A filter surfaces a child, which runs directly (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(list != nullptr);
    REQUIRE(filter != nullptr);

    filter->setProperty("text", QStringLiteral("split"));
    QCoreApplication::processEvents();

    using Roles = contour::window::CommandPaletteModel::Roles;
    auto const& model = controller.model();
    auto const child = paletteRowOf(model, QStringLiteral("LaunchLayout:split-dev"));
    REQUIRE(child >= 0);
    CHECK(paletteRole(model, child, Roles::TitleRole) == QStringLiteral("Launch Layout · split-dev"));
    auto group = QString {};
    for (auto const row: std::views::iota(0, child + 1))
        if (auto title = paletteRole(model, row, Roles::GroupTitleRole); !title.isEmpty())
            group = title;
    CHECK(group == QStringLiteral("Layouts"));

    list->setProperty("currentIndex", child);
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, child), Q_ARG(int, 0));
    QCoreApplication::processEvents();
    auto* childItem = paletteRowAt(list, child);
    REQUIRE(childItem != nullptr);
    auto* glyph = childItem->findChild<QQuickItem*>(QStringLiteral("commandPaletteDrillIn"));
    REQUIRE(glyph != nullptr);
    CHECK_FALSE(glyph->isVisible());

    pressPaletteKey(focusPaletteField(host), Qt::Key_Return);
    waitForPopupClosed(*host.palette);
    CHECK(controller.ran == QStringList { QStringLiteral("LaunchLayout:split-dev") });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A 200-row level keeps the selection in view (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto inventory = PaletteInventory {};
    inventory.profiles.clear();
    for (auto const index: std::views::iota(0, 200))
        inventory.profiles.push_back(std::format("p{:03}", index));
    MockPaletteController controller { inventory };

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);

    controller.model().enter(paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
    QCoreApplication::processEvents();
    REQUIRE(list->property("count").toInt() == 200);

    auto* window = focusPaletteField(host);
    constexpr auto Steps = 150;
    for ([[maybe_unused]] auto const step: std::views::iota(0, Steps))
        QTest::keyClick(window, Qt::Key_Down);
    QCoreApplication::processEvents();

    REQUIRE(list->property("currentIndex").toInt() == Steps);
    auto* current = list->property("currentItem").value<QQuickItem*>();
    REQUIRE(current != nullptr);
    auto const top = list->property("contentY").toReal();
    auto const bottom = top + list->height();
    CHECK(current->y() >= top);
    CHECK(current->y() + current->height() <= bottom);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A long or right-to-left title elides inside its row (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    auto inventory = PaletteInventory {};
    inventory.profiles = { "שלום" };
    inventory.tabs = { std::string(300, 'q') };
    MockPaletteController controller { inventory };

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(list != nullptr);
    REQUIRE(filter != nullptr);

    struct Case
    {
        QString filter;
        QString id;
    };
    auto const cases =
        std::array { Case { .filter = QStringLiteral("qqqq"), .id = QStringLiteral("SwitchToTab:1") },
                     Case { .filter = QString::fromUtf8("של"),
                            .id = QString::fromUtf8("ChangeProfile:שלום") } };
    for (auto const& entry: cases)
    {
        INFO("row " << entry.id.toStdString());
        filter->setProperty("text", entry.filter);
        QCoreApplication::processEvents();
        auto const row = paletteRowOf(controller.model(), entry.id);
        REQUIRE(row >= 0);
        QMetaObject::invokeMethod(list, "forceLayout");
        QCoreApplication::processEvents();
        auto* item = paletteRowAt(list, row);
        REQUIRE(item != nullptr);
        auto* entryItem = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
        auto* title = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteTitle"));
        auto* accessory = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteAccessory"));
        REQUIRE(entryItem != nullptr);
        REQUIRE(title != nullptr);
        REQUIRE(accessory != nullptr);

        CHECK(title->x() + title->width() <= accessory->x());
        CHECK(accessory->x() + accessory->width() <= entryItem->width());
        CHECK(title->property("effectiveHorizontalAlignment").toInt() == Qt::AlignLeft);
        if (entry.id.startsWith(QStringLiteral("SwitchToTab")))
            CHECK(title->property("truncated").toBool());
    }

    SECTION("inside a level, a right-to-left title alone still starts at the row's leading edge")
    {
        // "Change Profile · שלום" is left-to-right by its first strong character; "שלום" alone is not, and
        // without the explicit alignment Qt would set it against the row's right edge.
        filter->setProperty("text", QString {});
        QCoreApplication::processEvents();
        controller.model().enter(paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
        QCoreApplication::processEvents();
        REQUIRE(list->property("count").toInt() == 1);
        QMetaObject::invokeMethod(list, "forceLayout");
        QCoreApplication::processEvents();
        auto* item = paletteRowAt(list, 0);
        REQUIRE(item != nullptr);
        REQUIRE(item->property("title").toString() == QString::fromUtf8("שלום"));
        auto* title = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteTitle"));
        REQUIRE(title != nullptr);
        CHECK(title->property("effectiveHorizontalAlignment").toInt() == Qt::AlignLeft);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A popup surface without an elevation builds no elevation shadow (offscreen)",
          "[contour][gui][qml][palette]")
{
    // Every popup but the palette keeps the style's popup shadow, and carries no part of the elevation
    // path: an idle ElevationShadow inside the surface was seen washing a dialog's surface grey.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import Contour.Ui\nPopupSurface { width: 100; height: 100 }\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PopupSurfaceTestHost.qml")));
    INFO("errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto const surface = std::unique_ptr<QObject>(component.create());
    REQUIRE(surface != nullptr);

    auto* loader = surface->findChild<QObject*>(QStringLiteral("popupElevationShadow"));
    REQUIRE(loader != nullptr);
    CHECK_FALSE(loader->property("active").toBool());
    CHECK(loader->property("item").value<QObject*>() == nullptr);
    CHECK(surface->findChildren<QQuickItem*>(QStringLiteral("elevationSoftShadow")).isEmpty());
    CHECK(surface->findChildren<QQuickItem*>(QStringLiteral("elevationHardShadow")).isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The Terminal row draws the palette in cells (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto setup = contour::test::defaultChromeSetup(contour::config::UiStyle::Terminal);
    REQUIRE(setup.row == contour::config::ChromeStyle::Terminal);
    auto const host = openPalette(engine, controller, setup, QSize { 1280, 800 });
    REQUIRE(host.palette != nullptr);
    auto const& style = providerOf(engine);

    CHECK(host.palette->property("width").toReal() == Catch::Approx(48 * style.widthQuantum()));
    auto* background = host.palette->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == 0.0);
    // A popover keeps its hard one-cell shadow (Step 2's I3).
    auto* shadow = background->findChild<QQuickItem*>(QStringLiteral("elevationHardShadow"));
    REQUIRE(shadow != nullptr);
    CHECK(shadow->isVisible());

    auto* prompt = palettePart(host, QStringLiteral("commandPalettePrompt"));
    REQUIRE(prompt != nullptr);
    CHECK(prompt->isVisible());
    CHECK(prompt->property("text").toString() == QStringLiteral("❯"));

    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    auto* first = paletteRowAt(list, 0);
    REQUIRE(first != nullptr);
    auto* entry = first->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
    REQUIRE(entry != nullptr);
    CHECK(entry->height() == Catch::Approx(style.cellHeight()));

    controller.model().setFilter(QStringLiteral("ChangeProfile"));
    QCoreApplication::processEvents();
    auto const parent = paletteRowOf(controller.model(), QStringLiteral("ChangeProfile"));
    REQUIRE(parent >= 0);
    auto* parentItem = paletteRowAt(list, parent);
    REQUIRE(parentItem != nullptr);
    auto* glyph = parentItem->findChild<QQuickItem*>(QStringLiteral("commandPaletteDrillIn"));
    REQUIRE(glyph != nullptr);
    CHECK(glyph->property("text").toString() == QStringLiteral("▸"));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("accessibility: the palette's field, rows and group headers declare a role and a name",
          "[contour][gui][qml][palette][a11y]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    // The field itself, so there is one editable node.
    auto* query = accessibleOf(palettePart(host, QStringLiteral("commandPaletteFilter")));
    REQUIRE(query != nullptr);
    CHECK(query->property("role").toInt() == QAccessible::EditableText);
    CHECK(query->property("name").toString() == QStringLiteral("Command"));

    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);
    auto* first = paletteRowAt(list, 0);
    REQUIRE(first != nullptr);
    auto* header = accessibleOf(first->findChild<QQuickItem*>(QStringLiteral("commandPaletteGroupHeader")));
    REQUIRE(header != nullptr);
    CHECK(header->property("role").toInt() == QAccessible::Heading);
    CHECK(header->property("name").toString() == first->property("groupTitle").toString());
    auto* row = accessibleOf(first->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow")));
    REQUIRE(row != nullptr);
    CHECK(row->property("role").toInt() == QAccessible::ListItem);
    CHECK(row->property("name").toString() == first->property("title").toString());

    // A drill-in row says that it opens a list.
    controller.model().setFilter(QStringLiteral("ChangeProfile"));
    QCoreApplication::processEvents();
    auto* parentItem = paletteRowAt(list, paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
    REQUIRE(parentItem != nullptr);
    auto* parentRow = accessibleOf(parentItem->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow")));
    REQUIRE(parentRow != nullptr);
    CHECK(parentRow->property("description").toString() == QStringLiteral("Opens a list"));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Keys typed straight after open reach the field (offscreen)", "[contour][gui][qml][palette]")
{
    // The enter transition takes its time; the field must have the keyboard from the moment the palette
    // starts to show, or the first keys of a fast typist are lost.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    auto* window = filter->window(); // taken while open: a closed popup's items leave the window
    REQUIRE(window != nullptr);
    QMetaObject::invokeMethod(host.palette, "close");
    waitForPopupClosed(*host.palette);

    QMetaObject::invokeMethod(host.palette, "open");
    REQUIRE_FALSE(host.palette->property("opened").toBool()); // the enter transition is still running

    SECTION("Escape before the palette has finished opening closes it")
    {
        QTest::keyClick(window, Qt::Key_Escape);
        waitForPopupClosed(*host.palette);
        CHECK_FALSE(host.palette->property("visible").toBool());
        CHECK(controller.ran.isEmpty());
    }

    SECTION("typed keys land in the field and stay there")
    {
        for (auto const key: { 's', 'p', 'l' })
            QTest::keyClick(window, key);
        CHECK(filter->property("text").toString() == QStringLiteral("spl"));
        // ... and are still there once the palette has finished opening.
        waitForPopupOpened(*host.palette);
        CHECK(filter->property("text").toString() == QStringLiteral("spl"));
        CHECK(controller.model().filter() == QStringLiteral("spl"));

        QTest::keyClick(window, Qt::Key_Escape);
        waitForPopupClosed(*host.palette);
        CHECK(controller.ran.isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The footer's hints follow the row under the selection (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    auto* hints = palettePart(host, QStringLiteral("commandPaletteFooterHints"));
    REQUIRE(filter != nullptr);
    REQUIRE(hints != nullptr);

    using contour::command::PaletteDepth;
    using contour::command::PaletteEntryKind;
    auto const text = [](PaletteEntryKind kind) {
        return QString::fromStdString(contour::command::paletteFooterHints(
            kind, PaletteDepth::Top, contour::command::ShortcutStyle::Text));
    };

    // One row each time, under the same index: a drill-in row, then a command row. The count does not
    // change, so the hints must follow the row itself.
    filter->setProperty("text", QStringLiteral("changeprofile"));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().rowCount() == 1);
    REQUIRE(paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")) == 0);
    CHECK(hints->property("text").toString() == text(PaletteEntryKind::DrillIn));

    filter->setProperty("text", QStringLiteral("splitvertical"));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().rowCount() == 1);
    REQUIRE(paletteRowOf(controller.model(), QStringLiteral("SplitVertical")) == 0);
    CHECK(hints->property("text").toString() == text(PaletteEntryKind::Command));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A resting pointer does not take the selection after the list changes (offscreen)",
          "[contour][gui][qml][palette]")
{
    // Qt sends a synthetic hover at the last pointer position after every model reset. The selection
    // follows only a pointer that moves; a resting one leaves row 0 (or the anchor row) selected.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(list != nullptr);
    REQUIRE(filter != nullptr);
    auto* window = focusPaletteField(host);

    // Rest the pointer over the fourth row: a real move, so it selects that row.
    auto const restOn = [&](int row) {
        auto* item = paletteRowAt(list, row);
        REQUIRE(item != nullptr);
        auto* entry = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
        REQUIRE(entry != nullptr);
        // Two moves, because a move to where the pointer already is (an earlier case left it) sends nothing.
        QTest::mouseMove(window, entry->mapToScene(QPointF(1, 1)).toPoint());
        QTest::mouseMove(window,
                         entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2)).toPoint());
        QCoreApplication::processEvents();
    };
    constexpr auto RestingRow = 3;
    restOn(RestingRow);
    REQUIRE(list->property("currentIndex").toInt() == RestingRow);

    // Let any synthetic hover the reset causes arrive before asserting.
    auto const settle = [] {
        QCoreApplication::processEvents();
        QTest::qWait(contour::test::PopupSettleMs / 20);
    };

    SECTION("a palette opening under a resting pointer keeps row 0")
    {
        // While the palette is closed the pointer comes to rest where the sixth row will be: the first
        // hover the open brings is where the pointer already was, not a move.
        auto* sixth = paletteRowAt(list, RestingRow + 2);
        REQUIRE(sixth != nullptr);
        auto* sixthEntry = sixth->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
        REQUIRE(sixthEntry != nullptr);
        auto const restingPoint =
            sixthEntry->mapToScene(QPointF(sixthEntry->width() / 2, sixthEntry->height() / 2)).toPoint();
        QMetaObject::invokeMethod(host.palette, "close");
        waitForPopupClosed(*host.palette);
        QTest::mouseMove(window, QPoint(1, 1));
        QTest::mouseMove(window, restingPoint);
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(host.palette, "open");
        waitForPopupOpened(*host.palette);
        settle();
        CHECK(list->property("currentIndex").toInt() == 0);
    }

    SECTION("typing selects the best match, row 0")
    {
        for (auto const key: { 's', 'p', 'l' })
            QTest::keyClick(window, key);
        settle();
        CHECK(list->property("currentIndex").toInt() == 0);
    }

    SECTION("entering a level selects its first row, and going back the parent's")
    {
        // The pointer rests over the level's second row as the level opens.
        restOn(1);
        controller.model().enter(paletteRowOf(controller.model(), QStringLiteral("ChangeProfile")));
        settle();
        REQUIRE(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));
        CHECK(list->property("currentIndex").toInt() == 0);

        restOn(1);
        REQUIRE(list->property("currentIndex").toInt() == 1);
        pressPaletteKey(window, Qt::Key_Escape);
        settle();
        REQUIRE(controller.model().depth() == controller.model().topDepth());
        CHECK(list->property("currentIndex").toInt() == controller.model().anchorRow());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The field and the list agree on the query whoever changes it (offscreen)",
          "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    filter->setProperty("text", QStringLiteral("spl"));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().filter() == QStringLiteral("spl"));

    SECTION("a second open asked for while the palette is up keeps the query")
    {
        CHECK(controller.model().shown());
        controller.model().refresh({});
        QCoreApplication::processEvents();
        CHECK(controller.model().filter() == QStringLiteral("spl"));
        CHECK(filter->property("text").toString() == QStringLiteral("spl"));
    }

    SECTION("a query the model is given shows in the field")
    {
        controller.model().setFilter(QStringLiteral("profile"));
        QCoreApplication::processEvents();
        CHECK(filter->property("text").toString() == QStringLiteral("profile"));
    }

    SECTION("once closed, the next open starts afresh")
    {
        QMetaObject::invokeMethod(host.palette, "close");
        waitForPopupClosed(*host.palette);
        CHECK_FALSE(controller.model().shown());
        controller.model().refresh({});
        CHECK(controller.model().filter().isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A click on a drill-in row opens its level (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = palettePart(host, QStringLiteral("commandPaletteFilter"));
    auto* list = palettePart(host, QStringLiteral("commandPaletteList"));
    REQUIRE(filter != nullptr);
    REQUIRE(list != nullptr);
    filter->setProperty("text", QStringLiteral("ChangeProfile"));
    QCoreApplication::processEvents();
    auto const parent = paletteRowOf(controller.model(), QStringLiteral("ChangeProfile"));
    REQUIRE(parent >= 0);
    auto* item = paletteRowAt(list, parent);
    REQUIRE(item != nullptr);
    auto* entry = item->findChild<QQuickItem*>(QStringLiteral("commandPaletteRow"));
    REQUIRE(entry != nullptr);

    auto* window = entry->window();
    REQUIRE(window != nullptr);
    QTest::mouseClick(window,
                      Qt::LeftButton,
                      Qt::NoModifier,
                      entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2)).toPoint());
    QCoreApplication::processEvents();

    CHECK(controller.model().depth() == static_cast<int>(contour::command::PaletteDepth::Level));
    CHECK(list->property("count").toInt() == 2);
    CHECK(host.palette->property("opened").toBool());
    CHECK(controller.ran.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The palette opens with a 160 ms scale and fade (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller, macPaletteSetup(), QSize { 1280, 800 });
    REQUIRE(host.palette != nullptr);

    auto* scale = paletteEnterAnimation(host, QStringLiteral("commandPaletteEnterScale"));
    auto* fade = paletteEnterAnimation(host, QStringLiteral("commandPaletteEnterFade"));
    auto* exit = host.palette->property("_exitFade").value<QObject*>();
    REQUIRE(scale != nullptr);
    REQUIRE(fade != nullptr);
    REQUIRE(exit != nullptr);
    CHECK(scale->property("from").toReal() == Catch::Approx(0.97));
    CHECK(scale->property("duration").toInt() == 160);
    CHECK(fade->property("duration").toInt() == 160);
    CHECK(exit->property("duration").toInt() == 100);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Under Reduce Motion the palette only fades (offscreen)", "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto setup = macPaletteSetup();
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;
    auto const host = openPalette(engine, controller, setup, QSize { 1280, 800 });
    REQUIRE(host.palette != nullptr);

    auto* scale = paletteEnterAnimation(host, QStringLiteral("commandPaletteEnterScale"));
    auto* fade = paletteEnterAnimation(host, QStringLiteral("commandPaletteEnterFade"));
    REQUIRE(scale != nullptr);
    REQUIRE(fade != nullptr);
    CHECK(scale->property("from").toReal() == 1.0);
    CHECK(scale->property("duration").toInt() == contour::config::ReducedMotionCrossFadeMs);
    CHECK(fade->property("duration").toInt() == contour::config::ReducedMotionCrossFadeMs);

    // Re-open and watch the scale while the enter transition runs: it never leaves 1.
    QMetaObject::invokeMethod(host.palette, "close");
    waitForPopupClosed(*host.palette);
    auto minimumScale = 1.0;
    QMetaObject::invokeMethod(host.palette, "open");
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 10))
    {
        minimumScale = std::min(minimumScale, host.palette->property("scale").toReal());
        QTest::qWait(contour::config::ReducedMotionCrossFadeMs / 5);
    }
    CHECK(minimumScale == 1.0);
    CHECK(host.palette->property("opened").toBool());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: there is no hyperlink tooltip any more (offscreen)", "[contour][gui][qml][linkpill]")
{
    auto scene = LinkPillScene {};
    CHECK(scene.host.chrome->findChild<QQuickItem*>(QStringLiteral("hyperlinkTooltip")) == nullptr);
}

TEST_CASE("link pill: a single pane's pill sits top right over blank cells (offscreen)",
          "[contour][gui][qml][linkpill]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene {};
    scene.show();
    auto* pill = scene.pill;
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool() && pill->isVisible(); }));
    CHECK(pill->y() == chromeToken(scene.engine, "space", "s4"));
    CHECK(pill->x() + pill->width()
          == Catch::Approx(scene.host.chrome->width() - chromeToken(scene.engine, "space", "s5")));
    CHECK(pill->height() == chromeToken(scene.engine, "size", "linkPill"));
    CHECK(pill->property("background").value<QQuickItem*>()->property("radius").toReal()
          == chromeToken(scene.engine, "radii", "linkPill"));
    auto* detail = pill->findChild<QQuickItem*>(QStringLiteral("toastDetail"));
    REQUIRE(detail != nullptr);
    CHECK(detail->property("text").toString() == QStringLiteral("⌘-click to open"));
    // Room enough: the whole target, not cut.
    auto* text = pill->findChild<QQuickItem*>(QStringLiteral("toastText"));
    REQUIRE(text != nullptr);
    CHECK_FALSE(text->property("truncated").toBool());
    CHECK(text->width() >= text->property("implicitWidth").toReal());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: it moves off a prompt row and hides over a full-screen app (offscreen)",
          "[contour][gui][qml][linkpill]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene {};
    scene.session->setOverlayClearance(linkClearance(100, 20, 100, QVariantList { 0 }));
    scene.show();
    auto* pill = scene.pill;
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));
    CHECK(pill->y() > scene.host.chrome->height() / 2);

    // A full-screen program paints every cell: nowhere to go.
    scene.session->setOverlayClearance(linkClearance(100, 20, 0));
    QCoreApplication::processEvents();
    CHECK_FALSE(pill->property("shown").toBool());
    CHECK(QTest::qWaitFor([&] { return !pill->isVisible(); }));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: not shown means not visible (offscreen)", "[contour][gui][qml][linkpill]")
{
    auto scene = LinkPillScene {};
    scene.show(false);
    QTest::qWait(static_cast<int>(chromeToken(scene.engine, "motion", "standard")) * 2);
    CHECK_FALSE(scene.pill->property("shown").toBool());
    CHECK_FALSE(scene.pill->isVisible());
}

TEST_CASE("link pill: the Terminal row draws it in cells (offscreen)", "[contour][gui][qml][linkpill]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene { contour::config::ChromeStyle::Terminal };
    scene.show();
    REQUIRE(QTest::qWaitFor([&] { return scene.pill->property("shown").toBool(); }));
    auto* style =
        scene.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(scene.pill->height() == style->property("cellHeight").toReal());
    CHECK(scene.pill->property("background").value<QQuickItem*>()->property("radius").toReal() == 0.0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a split's pill sits in the pane header (offscreen)",
          "[contour][gui][qml][linkpill][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOverlayClearance(linkClearance(40, 18, 40));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto const& terminal = PaneTreeHost::partOf(card, "paneCardOverlays"); // laid exactly over the grid
    auto const geometry = QRectF(terminal.x(), terminal.y(), terminal.width(), terminal.height());
    auto* slot = card.findChild<QQuickItem*>(QStringLiteral("paneHeaderLinkSlot"));
    REQUIRE(slot != nullptr);
    auto* pill = card.findChild<QQuickItem*>(QStringLiteral("linkPill"));
    REQUIRE(pill != nullptr);
    auto const& actions = PaneTreeHost::partOf(card, "paneHeaderActions");

    panes.leftSession.setLinkTarget(
        QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), true);
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));
    CHECK(pill->parentItem() == slot);
    CHECK(pill->x() + pill->width() == Catch::Approx(slot->width()));
    CHECK(QTest::qWaitFor([&] { return !actions.isVisible(); }));
    CHECK(QRectF(terminal.x(), terminal.y(), terminal.width(), terminal.height()) == geometry);
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() == 1.0; }));
    auto const shownX = pill->x();
    auto const* header = card.findChild<QQuickItem*>(QStringLiteral("paneHeader"));
    REQUIRE(header != nullptr);
    auto const occupant = [&] {
        return header->property("trailingOccupant").toInt();
    };

    panes.leftSession.setLinkTarget(
        QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), false);
    // Mid-fade, it stays in the slot where it stood, and still holds the header: the actions under it come
    // back only once it is gone, so a click there cannot land on a pill fading over them.
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() > 0.0 && pill->opacity() < 1.0; }));
    CHECK(pill->parentItem() == slot);
    CHECK(pill->x() == shownX);
    CHECK(occupant() == static_cast<int>(contour::window::HeaderTrailingOccupant::LinkPill));
    CHECK_FALSE(actions.isVisible());
    CHECK(QTest::qWaitFor([&] { return !pill->isVisible(); }));
    CHECK(occupant() == static_cast<int>(contour::window::HeaderTrailingOccupant::None));
    // The header's actions come back once the pill has left.
    CHECK(QTest::qWaitFor([&] { return actions.isVisible(); }));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a header pill whose slot goes shows nowhere rather than over the grid (offscreen)",
          "[contour][gui][qml][linkpill][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOverlayClearance(linkClearance(40, 18, 40));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto* slot = card.findChild<QQuickItem*>(QStringLiteral("paneHeaderLinkSlot"));
    auto* pill = card.findChild<QQuickItem*>(QStringLiteral("linkPill"));
    REQUIRE(slot != nullptr);
    REQUIRE(pill != nullptr);
    panes.leftSession.setLinkTarget(
        QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), true);
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() == 1.0 && pill->parentItem() == slot; }));

    // The pill starts to fade, and the pane loses its header slot while it does.
    panes.leftSession.setLinkTarget(
        QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), false);
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() < 1.0; }));
    REQUIRE(pill->isVisible()); // still fading
    PaneTreeHost::terminalOf(card).setProperty("headerLinkSlot", QVariant::fromValue<QQuickItem*>(nullptr));
    CHECK_FALSE(pill->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a placement the provider refuses shows no pill (offscreen)",
          "[contour][gui][qml][linkpill]")
{
    // An input out of range answers an empty map, whose anchor is undefined: that is no place, not a pill of
    // undefined width at the grid's corner.
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene {};
    scene.pill->setProperty("findBar", static_cast<int>(contour::window::FindBarPresenceCount));
    scene.show();
    QCoreApplication::processEvents();
    CHECK_FALSE(scene.pill->property("shown").toBool());
    CHECK(scene.pill->property("livePlacement").toMap().value(QStringLiteral("anchor")).toString()
          == QStringLiteral("hidden"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a hiding pill fades where it stood (offscreen)", "[contour][gui][qml][linkpill]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene {};
    scene.show();
    auto* pill = scene.pill;
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() == 1.0; }));
    auto* const parent = pill->parentItem();
    auto const where = QRectF(pill->x(), pill->y(), pill->width(), pill->height());
    REQUIRE(where.width() > 0.0);

    scene.show(false);
    // Mid-fade: still where it was, at its width, under the same parent -- not at the grid's corner.
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() > 0.0 && pill->opacity() < 1.0; }));
    CHECK(pill->parentItem() == parent);
    CHECK(QRectF(pill->x(), pill->y(), pill->width(), pill->height()) == where);

    // Faded out, it takes up the hidden placement.
    REQUIRE(QTest::qWaitFor([&] { return pill->opacity() == 0.0; }));
    CHECK(pill->property("placement").toMap().value(QStringLiteral("anchor")).toString()
          == QStringLiteral("hidden"));
    CHECK_FALSE(pill->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a narrow header slot cuts the target in its middle (offscreen)",
          "[contour][gui][qml][linkpill][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOverlayClearance(linkClearance(40, 18, 40));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto& slot = PaneTreeHost::partOf(card, "paneHeaderLinkSlot");
    auto* pill = card.findChild<QQuickItem*>(QStringLiteral("linkPill"));
    REQUIRE(pill != nullptr);
    panes.leftSession.setLinkTarget(longLinkTarget(), QStringLiteral("Ctrl+Click to open"), true);
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool() && pill->parentItem() == &slot; }));
    REQUIRE(pill->property("naturalWidth").toReal() > slot.width());
    // Inside the slot, flush with its trailing edge, its target cut.
    CHECK(pill->x() >= 0.0);
    CHECK(pill->x() + pill->width() == Catch::Approx(slot.width()));
    auto* text = pill->findChild<QQuickItem*>(QStringLiteral("toastText"));
    REQUIRE(text != nullptr);
    CHECK(QTest::qWaitFor([&] { return text->property("truncated").toBool(); }));
    CHECK(text->property("elide").toInt() == Qt::ElideMiddle);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: a header slot too narrow for the pill sends it to the grid (offscreen)",
          "[contour][gui][qml][linkpill][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOverlayClearance(linkClearance(40, 18, 40));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto& slot = PaneTreeHost::partOf(card, "paneHeaderLinkSlot");
    auto* pill = card.findChild<QQuickItem*>(QStringLiteral("linkPill"));
    REQUIRE(pill != nullptr);
    // A hint wider than the slot: the pill could show none of the target there.
    auto const hint =
        QStringLiteral("Ctrl+Click to open this link in the browser, as the follow binding says");
    panes.leftSession.setLinkTarget(longLinkTarget(), hint, true);
    REQUIRE(QTest::qWaitFor([&] {
        return pill->property("livePlacement").toMap().value(QStringLiteral("anchor")).toString()
               != QStringLiteral("hidden");
    }));
    REQUIRE(pill->property("minimumWidth").toReal() > slot.width());
    CHECK(pill->parentItem() != &slot);
    CHECK(pill->property("inHeader").toBool() == false);
    // The header keeps its actions.
    CHECK(card.findChild<QQuickItem*>(QStringLiteral("paneHeader"))->property("trailingOccupant").toInt()
          == static_cast<int>(contour::window::HeaderTrailingOccupant::None));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: an open find bar at the top right sends the pill to the bottom right (offscreen)",
          "[contour][gui][qml][linkpill]")
{
    contour::test::QmlMessageCapture const warnings;
    auto scene = LinkPillScene {};
    auto* bar = scene.host.chrome->findChild<QObject*>(QStringLiteral("searchBar"));
    REQUIRE(bar != nullptr);
    QMetaObject::invokeMethod(bar, "open");
    REQUIRE(QTest::qWaitFor([bar] { return bar->property("visible").toBool(); }));
    REQUIRE(bar->property("placement").toMap().value(QStringLiteral("anchor")).toString()
            == QStringLiteral("topRight"));

    scene.show();
    auto* pill = scene.pill;
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool() && pill->isVisible(); }));
    CHECK(pill->property("placement").toMap().value(QStringLiteral("anchor")).toString()
          == QStringLiteral("bottomRight"));
    // Below the bar, never over it.
    CHECK(pill->y() >= bar->property("y").toReal() + bar->property("height").toReal());

    // Closed, the bar gives the top right back.
    QMetaObject::invokeMethod(bar, "close");
    CHECK(QTest::qWaitFor([&] {
        return pill->property("placement").toMap().value(QStringLiteral("anchor")).toString()
               == QStringLiteral("topRight");
    }));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("link pill: the find bar in the header row keeps the pill out of the header (offscreen)",
          "[contour][gui][qml][linkpill][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOverlayClearance(linkClearance(40, 18, 40));
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto& bar = openFindBarOf(card);
    REQUIRE(parentOf(bar) == &PaneTreeHost::partOf(card, "paneHeaderFindSlot"));
    auto const& header = PaneTreeHost::partOf(card, "paneHeader");
    CHECK(header.property("trailingOccupant").toInt()
          == static_cast<int>(contour::window::HeaderTrailingOccupant::Find));

    auto* pill = card.findChild<QQuickItem*>(QStringLiteral("linkPill"));
    REQUIRE(pill != nullptr);
    panes.leftSession.setLinkTarget(
        QStringLiteral("github.com › contour-terminal"), QStringLiteral("Ctrl+Click to open"), true);
    REQUIRE(QTest::qWaitFor([&] { return pill->property("shown").toBool(); }));
    CHECK(pill->property("placement").toMap().value(QStringLiteral("anchor")).toString()
          == QStringLiteral("bottomRight"));
    CHECK(pill->parentItem() != &PaneTreeHost::partOf(card, "paneHeaderLinkSlot"));
    // The bar keeps the header: the occupant stays the find bar while the pill shows over the grid.
    CHECK(header.property("trailingOccupant").toInt()
          == static_cast<int>(contour::window::HeaderTrailingOccupant::Find));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

// }}}

TEST_CASE("ActionContextMenu shows a header row as a disabled label", "[contour][gui][qml][blocks]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto const row = [](char const* kind, char const* title) {
        return QVariantMap { { QStringLiteral("kind"), QString::fromLatin1(kind) },
                             { QStringLiteral("title"), QString::fromLatin1(title) },
                             { QStringLiteral("enabled"), true },
                             { QStringLiteral("checkable"), false },
                             { QStringLiteral("checked"), false },
                             { QStringLiteral("actionId"), 0 } };
    };
    auto const model = QVariantList { row("header", "make"),
                                      row("header", "Failed · exit 2"),
                                      row("separator", ""),
                                      row("command", "Copy Output") };

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ActionContextMenu.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> menu(component.createWithInitialProperties({ { "entries", model } }));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->property("count").toInt() == model.size());

    auto const itemAt = [&](int index) {
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(
            menu.get(), "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index)));
        REQUIRE(item != nullptr);
        return item;
    };
    auto const* subject = itemAt(0);
    auto const* detail = itemAt(1);
    CHECK(subject->objectName() == QStringLiteral("contextMenuHeader"));
    CHECK(subject->property("text").toString() == QStringLiteral("make"));
    CHECK_FALSE(subject->property("enabled").toBool());
    CHECK(detail->objectName() == QStringLiteral("contextMenuHeader"));
    CHECK_FALSE(detail->property("enabled").toBool());
    // The subject is the larger face and the primary ink; the detail the smaller and the secondary one.
    auto& style = providerOf(engine);
    CHECK(subject->property("font").value<QFont>() == style.type()->property("footnote").value<QFont>());
    CHECK(detail->property("font").value<QFont>() == style.type()->property("caption").value<QFont>());
    CHECK(itemAt(3)->property("enabled").toBool());
    CHECK(itemAt(3)->objectName() != QStringLiteral("contextMenuHeader"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome puts the block's ⋯ button in its strip, and the tooltip is gone",
          "[contour][gui][qml][blocks]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    auto setup = contour::test::defaultChromeSetup();
    setup.row = row;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto* button = host.chrome->findChild<QQuickItem*>(QStringLiteral("blockActionsButton"));
    REQUIRE(button != nullptr);
    // The block's text lives in its menu now.
    CHECK(host.chrome->findChild<QQuickItem*>(QStringLiteral("commandBlockTooltip")) == nullptr);
    // No block hovered: no button.
    CHECK_FALSE(button->isVisible());

    auto const& size = providerOf(engine);
    auto const edge = size.property("size").value<QObject*>()->property("blockActions").toReal();
    auto const anchor = QRectF(6, 40, edge, 17);
    session->setBlockHover(7, anchor);
    QCoreApplication::processEvents();

    SECTION("it sits in the strip, at its top, and does not go past it")
    {
        REQUIRE(button->isVisible());
        CHECK(button->width() == Catch::Approx(edge));
        CHECK(button->y() == Catch::Approx(anchor.y()));
        CHECK(button->x() >= anchor.x());
        CHECK(button->x() + button->width() <= anchor.right() + 1e-6);
        // A square on the native rows; the Terminal row's is the cell.
        if (row == ChromeStyle::Terminal)
            CHECK(button->height() == Catch::Approx(anchor.height()));
        else
            CHECK(button->height() == Catch::Approx(button->width()));
        if (row == ChromeStyle::MacOS)
        {
            // The canvas's `blocks` board: 22 x 22 at a radius of 6.
            CHECK(button->width() == 22);
            auto* face = button->findChild<QQuickItem*>(QStringLiteral("blockActionsFace"));
            REQUIRE(face != nullptr);
            CHECK(face->property("radius").toReal() == 6);
        }
    }

    SECTION("a click asks the session for the block's menu, once")
    {
        auto* window = qobject_cast<QQuickWindow*>(host.window.get());
        REQUIRE(window != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(*button));
        QCoreApplication::processEvents();
        CHECK(session->blockActionOpens == 1);
    }

    SECTION("no hovered block hides it again")
    {
        REQUIRE(button->isVisible());
        session->setBlockHover(0, QRectF());
        QCoreApplication::processEvents();
        CHECK_FALSE(button->isVisible());
    }

    SECTION("a button beside the focused block is ringed, one beside another is not")
    {
        auto* ring = button->findChild<QQuickItem*>(QStringLiteral("blockActionsFocusRing"));
        REQUIRE(ring != nullptr);
        CHECK_FALSE(ring->isVisible());
        session->setFocusedCommandBlock(3); // another block
        QCoreApplication::processEvents();
        CHECK_FALSE(ring->isVisible());
        session->setFocusedCommandBlock(7); // the hovered one
        QCoreApplication::processEvents();
        CHECK(ring->isVisible());
        session->setFocusedCommandBlock(0);
        QCoreApplication::processEvents();
        CHECK_FALSE(ring->isVisible());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome draws a running tick hollow and a failure's solid", "[contour][gui][qml][blocks]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto* layer = host.bar->findChild<QQuickItem*>(QStringLiteral("scrollbarMarkLayer"));
    REQUIRE(layer != nullptr);
    auto session = createScrollableSession();
    session->setScrollbarMarks(QVariantList { markEntry(0.2, vtbackend::ScrollbarMarkKind::Running, 1),
                                              markEntry(0.6, vtbackend::ScrollbarMarkKind::Failure, 2) },
                               markColors());
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto const ticks = markTicksOf(*layer);
    REQUIRE(ticks.size() == 2);
    auto const& tokens = providerOf(engine);
    auto const* sizes = tokens.property("size").value<QObject*>();
    REQUIRE(sizes != nullptr);

    auto const* running = ticks[0];
    CHECK(running->property("color").value<QColor>().alpha() == 0);
    CHECK(running->property("border").value<QObject*>()->property("width").toReal() >= 1);
    CHECK(running->property("border").value<QObject*>()->property("color").value<QColor>()
          == QColor(Qt::yellow));
    CHECK(running->height() == Catch::Approx(sizes->property("scrollTick").toReal()));

    auto const* failure = ticks[1];
    CHECK(failure->property("color").value<QColor>() == QColor(Qt::red));
    CHECK(failure->property("border").value<QObject*>()->property("width").toReal() == 0);
    CHECK(failure->height() == Catch::Approx(sizes->property("scrollTick").toReal()));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the new-tab menu lists the profiles (offscreen)", "[contour][gui][qml][menus]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    // The menu is the title bar's, which pops it below the strip's "▾" (Fluent's joined strip draws one).
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Fluent;
    auto const host = makeTitleBarHostWith(engine, controller, setup);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(window));

    auto const* button = host->findChild<QQuickItem*>(QStringLiteral("newTabMenuButton"));
    auto* menu = host->findChild<QObject*>(QStringLiteral("newTabMenu"));
    REQUIRE(button != nullptr);
    REQUIRE(menu != nullptr);

    QTest::mouseClick(window,
                      Qt::LeftButton,
                      {},
                      button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    CHECK(controller.openNewTabMenuCalls == 1);
    REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
    CHECK(menu->property("count").toInt() == controller.newTabMenuModel().size());
    // One row per profile.
    CHECK(menu->property("count").toInt() == static_cast<int>(contour::test::MockProfileNames.size()));

    // A picked profile reaches the controller, once the menu has closed.
    auto* first = menuItemAt(*menu, 0);
    REQUIRE(first != nullptr);
    REQUIRE(QMetaObject::invokeMethod(first, "triggered"));
    REQUIRE(QTest::qWaitFor([&] { return !menu->property("visible").toBool(); }, 2000));
    CHECK(controller.newTabMenuPicks == std::vector { 0 });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A TabItem grabs its drag image only while a drag can start from it (offscreen)",
          "[contour][gui][qml]")
{
    // A tab's drag image is an asynchronous grab. Each one holds its image until the garbage collector
    // frees it, and one still in flight when its window or engine goes away is never freed at all. A tab
    // the pointer is not on has no drag to start, so its title changing grabs nothing; with the pointer
    // on it, the next change does.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);

    // A frame drawn after each change, as a live window draws one: a grab started lands on it.
    auto const retitle = [&](int build) {
        tab->setProperty("tabTitle", QStringLiteral("build %1").arg(build));
        std::ignore = window->grabWindow();
        QCoreApplication::processEvents();
    };

    // The pointer off the window. Where an earlier test left it the tab may sit under it, and so have
    // grabbed once already: that grab is let land, and its image set aside.
    auto const away = QPoint(-1000, -1000);
    QTest::mouseMove(window, away);
    std::ignore = window->grabWindow();
    QCoreApplication::processEvents();
    tab->setProperty("dragImage", QUrl());

    for (auto const build: std::views::iota(0, 5))
        retitle(build);
    CHECK(tab->property("dragImage").toUrl().isEmpty());

    QTest::mouseMove(window, sceneRectOf(*tab).center().toPoint());
    retitle(5);
    CHECK_FALSE(tab->property("dragImage").toUrl().isEmpty());

    // The pointer leaves the window, or the next test's tab could open under it and grab.
    QTest::mouseMove(window, away);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A resize-border press over a tab does not start a tab drag (offscreen)",
          "[contour][gui][qml][resize]")
{
    skipUnlessOffscreen();

    // Issue #1997. ResizeBorder fills the window at z:1000, so its 6px handles lie over the chrome a
    // frameless window draws for itself, and the top-left corner one lands exactly on the first tab. A
    // DragHandler takes a PASSIVE grab on press, so it sees the moves whoever is on top; while it could
    // also take over from an item it then stole the exclusive grab from the handle that had just asked the
    // window manager to resize, and TabItem's automatic QDrag rendered the tab under the cursor for the
    // length of the gesture. Releasing away from a strip would have gone on to call tearOffTab().
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemUnderResizeBorderHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto* handler = tab->findChild<QObject*>(QStringLiteral("tabDragHandler"));
    REQUIRE(handler != nullptr);
    // Activation is transient (`active` goes false again on release), so the question is "did it ever
    // activate", not "is it active now".
    QSignalSpy const activations(handler, SIGNAL(activeChanged()));

    // Every section presses a point that lies over BOTH a resize handle and the tab -- the overlap the
    // bug lived in. The two REQUIRE checks are what stop a missed press from passing the CHECK
    // vacuously.
    auto const drag = [&](QPoint const& from, QPoint const& to) {
        REQUIRE(isOverResizeBorder(*host, from));
        REQUIRE(sceneRectOf(*tab).contains(from));
        postGesture(*window, Qt::LeftButton, from, to, GestureEnd::Released);
    };

    SECTION("the top-left corner handle, which lands on the first tab")
    {
        drag(QPoint(3, 3), QPoint(200, 120));
        CHECK(activations.isEmpty());
    }

    SECTION("the top edge handle, which covers the top of every tab")
    {
        drag(QPoint(60, 3), QPoint(240, 120));
        CHECK(activations.isEmpty());
    }

    SECTION("the left edge handle, which covers the left of the first tab")
    {
        drag(QPoint(3, 20), QPoint(200, 140));
        CHECK(activations.isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A resize-border press over the title bar does not start a window move (offscreen)",
          "[contour][gui][qml][resize]")
{
    skipUnlessOffscreen();

    // The bar's empty stretch drags the window (startSystemMove), and it too lies under the border's top
    // edge zone — so the same press that began a resize went on to ask for a move as well. Same cause as
    // the tab-drag half of #1997, different victim.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    // The REAL ResizeBorder stacked over the bar, in Main.qml's own order (bar at z:1, border filling
    // the window at z:1000), so the bar's window-move region really does sit beneath the border's top
    // edge zone as it does in the running application. 800 wide, as this test has always been.
    auto const host = makeTitleBarHost(engine,
                                       controller,
                                       contour::config::UiStyle::Native,
                                       contour::config::WindowControlStyle::Windows,
                                       QStringLiteral("  ResizeBorder { window: host }\n"),
                                       800);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);

    auto* moveHandler = host->findChild<QObject*>(QStringLiteral("titleBarDragHandler"));
    REQUIRE(moveHandler != nullptr);
    // Read the region's real geometry rather than guessing an x: it starts after however wide the tab strip
    // and the new-tab button happen to be, and ends before the window controls.
    auto* dragRegion = host->findChild<QQuickItem*>(QStringLiteral("dragRegion"));
    REQUIRE(dragRegion != nullptr);
    REQUIRE(dragRegion->width() > 0);
    auto const regionCentreX =
        static_cast<int>(dragRegion->mapToScene(QPointF(0, 0)).x() + (dragRegion->width() / 2));

    QSignalSpy const activations(moveHandler, SIGNAL(activeChanged()));

    // Over both a resize handle and the bar's draggable stretch, which is the overlap under test.
    auto const press = QPoint(regionCentreX, 3);
    REQUIRE(isOverResizeBorder(*host, press));
    REQUIRE(sceneRectOf(*dragRegion).contains(press));

    postGesture(*window, Qt::LeftButton, press, QPoint(regionCentreX + 120, 120), GestureEnd::Released);
    CHECK(activations.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The resize border is exactly as thick as the gutter's lead-in clears",
          "[contour][gui][qml][resize][gutter]")
{
    // Both read geometry::ResizeBorderThickness -- the border through chromeStyle -- or a thicker border
    // would quietly put the gutter's block column back under its left zone (@see geometry::gutterLeadIn).
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTabItemUnderResizeBorderHost(engine, controller);

    auto const zones = resizeZonesOf(*host);
    auto const leftEdge = std::ranges::find_if(
        zones, [](QQuickItem const* zone) { return zone->property("edges").toInt() == Qt::LeftEdge; });
    REQUIRE(leftEdge != zones.end());

    CHECK((*leftEdge)->width() == contour::geometry::ResizeBorderThickness);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Resize handles are inactive outside the normal window state", "[contour][gui][qml][resize]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTabItemUnderResizeBorderHost(engine, controller);
    auto const zones = resizeZonesOf(*host);
    REQUIRE(zones.size() == 8);
    for (auto const visibility:
         { QWindow::Windowed, QWindow::Maximized, QWindow::FullScreen, QWindow::Windowed })
    {
        host->setProperty("visibility", visibility);
        QCoreApplication::processEvents();
        for (auto const* zone: zones)
            CHECK(zone->isEnabled() == (visibility == QWindow::Windowed));
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A right-button drag over a tab does not start a tab drag (offscreen)",
          "[contour][gui][qml][resize]")
{
    // A tab is dragged with the left button; the right one belongs to its context menu. That is already a
    // pointer handler's default, so this pins it rather than fixing anything — TitleBar's own drag handler
    // states the same thing explicitly, and TabItem simply never got the same treatment.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto* handler = tab->findChild<QObject*>(QStringLiteral("tabDragHandler"));
    REQUIRE(handler != nullptr);

    QSignalSpy const activations(handler, SIGNAL(activeChanged()));

    // Well inside the tab (it is at the window origin, at least 120x32), dragged clear of it. There is no
    // ResizeBorder in this host, so nothing but acceptedButtons can hold the drag back.
    postGesture(*window, Qt::RightButton, QPoint(30, 16), QPoint(200, 90), GestureEnd::Released);
    CHECK(activations.isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The terminal chrome style quantizes the tab strip to whole cells (offscreen)",
          "[contour][gui][qml]")
{
    // The end-to-end proof that the QML actually reads its metrics from the style tokens rather than
    // from the literals it used to carry: the SAME component, loaded twice, differs only by which
    // UiStyleProvider its engine was given.
    contour::test::QmlMessageCapture const warnings;

    auto const tabWidthUnder = [](contour::config::UiStyle style) {
        QQmlEngine engine;
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, style);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
        INFO("TabItem errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());

        QVariantMap initial;
        initial.insert("controller", QVariant::fromValue(&controller));
        initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
        initial.insert("tabIndex", 0);
        initial.insert("tabTitle", QStringLiteral("a rather long tab title"));
        initial.insert("tabRawTitle", QString {});
        initial.insert("tabColor", QColor(Qt::transparent));
        initial.insert("tabActive", true);
        initial.insert("tabPaneCount", 1);
        initial.insert("tabZoomed", false);
        initial.insert("tabProgressState", 0);
        initial.insert("tabProgressPercentage", 0);
        std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
        REQUIRE(tab != nullptr);

        auto const* provider = engine.rootContext()
                                   ->contextProperty(QStringLiteral("chromeStyle"))
                                   .value<contour::window::UiStyleProvider*>();
        REQUIRE(provider != nullptr);
        return ChromeMetrics { .value = tab->property("implicitWidth").toReal(),
                               .cellWidth = provider->cellWidth(),
                               .cellHeight = provider->cellHeight(),
                               .widthQuantum = provider->widthQuantum() };
    };

    SECTION("the native style leaves the historical pixel width alone")
    {
        auto const native = tabWidthUnder(contour::config::UiStyle::Native);
        // Native's quantum is one logical pixel, so the rounding in TabItem is a no-op and the width
        // stays inside the 120..240 band the strip has always used.
        CHECK(native.widthQuantum == 1.0);
        CHECK(native.value >= 120.0);
        CHECK(native.value <= 240.0);
    }

    SECTION("the terminal style snaps the tab onto the character grid")
    {
        auto const terminal = tabWidthUnder(contour::config::UiStyle::Terminal);
        REQUIRE(terminal.cellWidth > 0.0);
        CHECK(terminal.widthQuantum == terminal.cellWidth);

        // The point of the style: the tab is a whole number of cells wide, so a strip of them lines
        // up as ASCII art rather than landing wherever the label happened to end.
        auto const cells = terminal.value / terminal.cellWidth;
        CHECK(std::abs(cells - std::round(cells)) < 0.001);
        CHECK(cells >= 10.0); // minTabUnits
        CHECK(cells <= 24.0); // maxTabUnits
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A tab's close button sits in clear space at both ends (offscreen)", "[contour][gui][qml]")
{
    // The trailing gap is the one that has to be stated. A style that rules between tabs ends every
    // tab with that rule, and with the close button anchored straight onto it "×│" sat side by side --
    // at one cell each, in the same monospace font, that reads as a single box-drawing figure rather
    // than as a button beside a border. (The native rows' tabs are pills or joined tabs, laid out by the
    // kit; their gaps are asserted where those shapes are.)
    contour::test::QmlMessageCapture const warnings;

    struct TabGaps
    {
        qreal leading = 0.0;  ///< From the label's trailing edge to the close button.
        qreal trailing = 0.0; ///< From the close button to whatever ends the tab.
        qreal cellWidth = 0.0;
    };

    auto const gapsUnder = [](contour::config::UiStyle style) {
        QQmlEngine engine;
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, style);

        // A real window rather than a bare component: these are anchor-resolved positions, and the
        // hosted-and-realized path is the one the rest of this file has proven offscreen.
        auto const host = buildQmlHost(engine,
                                       QStringLiteral("import QtQuick\n"
                                                      "import QtQuick.Window\n"
                                                      "import Contour.Ui\n"
                                                      "Window {\n"
                                                      "  id: host\n"
                                                      "  width: 600; height: 120; visible: true\n"
                                                      "  TabItem {\n"
                                                      "    objectName: \"probeTab\"\n"
                                                      "%1"
                                                      "  }\n"
                                                      "}\n")
                                           .arg(QLatin1StringView(TabItemBindings)),
                                       QStringLiteral("TabGapTestHost.qml"));
        QCoreApplication::processEvents();

        auto const* tab = host->findChild<QQuickItem*>(QStringLiteral("probeTab"));
        REQUIRE(tab != nullptr);
        auto const* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
        auto const* closeButton = tab->findChild<QQuickItem*>(QStringLiteral("tabCloseButton"));
        auto const* separator = tab->findChild<QQuickItem*>(QStringLiteral("tabSeparator"));
        REQUIRE(label != nullptr);
        REQUIRE(closeButton != nullptr);
        REQUIRE(separator != nullptr);

        auto const* provider = engine.rootContext()
                                   ->contextProperty(QStringLiteral("chromeStyle"))
                                   .value<contour::window::UiStyleProvider*>();
        REQUIRE(provider != nullptr);

        // The separator is anchored to the tab's trailing edge and collapses to zero width in a style
        // that draws none, so its left edge IS "wherever the tab ends" under either style.
        return TabGaps { .leading = closeButton->x() - (label->x() + label->width()),
                         .trailing = separator->x() - (closeButton->x() + closeButton->width()),
                         .cellWidth = provider->cellWidth() };
    };

    SECTION("the terminal style keeps a whole cell either side")
    {
        auto const terminal = gapsUnder(contour::config::UiStyle::Terminal);
        REQUIRE(terminal.cellWidth > 0.0);
        CHECK(terminal.leading == Catch::Approx(terminal.cellWidth));
        CHECK(terminal.trailing == Catch::Approx(terminal.cellWidth));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The terminal style rules BETWEEN tabs, not after the last one (offscreen)", "[contour][gui][qml]")
{
    // The separator lives inside each tab -- the strip's drop math walks itemAtIndex() and needs one
    // ListView item per tab -- so nothing stops the trailing tab from drawing one. There it is not a
    // separator at all: it hangs off the end of the strip, immediately before the "+" button, reading
    // as a tab cut in half. It also costs that tab a cell of title, because naturalWidth sums it.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Terminal);

    // A ListView of its own rather than TabStrip itself: the strip sizes its list from the list's own
    // contentWidth, which is 0 until delegates exist, so offscreen it converges over more incubation
    // cycles than a test should wait on. What is under test here is TabItem's own question -- "is
    // there a tab after me?" -- and being a horizontal ListView delegate is the whole of the context
    // it asks it in, which this host reproduces exactly.
    constexpr auto TabCount = 3;
    auto const host = buildQmlHost(engine,
                                   QStringLiteral("import QtQuick\n"
                                                  "import QtQuick.Window\n"
                                                  "import Contour.Ui\n"
                                                  "Window {\n"
                                                  "  id: host\n"
                                                  "  width: 900; height: 120; visible: true\n"
                                                  "  ListView {\n"
                                                  "    objectName: \"probeStrip\"\n"
                                                  "    anchors.fill: parent\n"
                                                  "    orientation: ListView.Horizontal\n"
                                                  "    model: %1\n"
                                                  "    delegate: TabItem {\n"
                                                  "      required property int index\n"
                                                  "      height: 40\n"
                                                  "      controller: terminalSessions\n"
                                                  "      window: host\n"
                                                  "      tabIndex: index\n"
                                                  "      tabTitle: \"tab \" + index\n"
                                                  "      tabRawTitle: \"\"\n"
                                                  "      tabColor: \"transparent\"\n"
                                                  "      tabActive: index === 0\n"
                                                  "      tabPaneCount: 1\n"
                                                  "      tabZoomed: false\n"
                                                  "      tabProgressState: 0\n"
                                                  "      tabProgressPercentage: 0\n"
                                                  "    }\n"
                                                  "  }\n"
                                                  "}\n")
                                       .arg(TabCount),
                                   QStringLiteral("TabSeparatorHost.qml"));
    QCoreApplication::processEvents();

    // itemAtIndex() rather than findChildren(): it asks the view where a given row actually went,
    // which is the same handle TabStrip's own drop math uses, and it does not depend on a delegate's
    // QObject parentage being anything in particular.
    auto* strip = host->findChild<QQuickItem*>(QStringLiteral("probeStrip"));
    REQUIRE(strip != nullptr);

    for (auto const index: std::views::iota(0, TabCount))
    {
        INFO("tab " << index << " of " << TabCount);
        QQuickItem* tab = nullptr;
        REQUIRE(QMetaObject::invokeMethod(
            strip, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, tab), Q_ARG(int, index)));
        REQUIRE(tab != nullptr);

        auto const* separator = tab->findChild<QQuickItem*>(QStringLiteral("tabSeparator"));
        REQUIRE(separator != nullptr);
        CHECK(separator->property("text").toString().isEmpty() == (index == TabCount - 1));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The terminal chrome style makes the title bar one character row tall (offscreen)",
          "[contour][gui][qml]")
{
    // The chrome height is what Main.qml declares to the WindowController, which turns it into the
    // window's size hints -- so this is also what shrinks the terminal viewport. Native (on Linux, the
    // ContourLinux row) is the Linux board's 46-pixel header bar; Terminal must be exactly one row of the
    // chrome font.
    contour::test::QmlMessageCapture const warnings;

    auto const chromeHeightUnder = [](contour::config::UiStyle style) {
        QQmlEngine engine;
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, style);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TitleBar.qml")));
        INFO("TitleBar errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());

        QVariantMap initial;
        initial.insert("controller", QVariant::fromValue(&controller));
        initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
        std::unique_ptr<QObject> bar(component.createWithInitialProperties(initial));
        REQUIRE(bar != nullptr);

        auto const* provider = engine.rootContext()
                                   ->contextProperty(QStringLiteral("chromeStyle"))
                                   .value<contour::window::UiStyleProvider*>();
        REQUIRE(provider != nullptr);
        return ChromeMetrics { .value = bar->property("implicitHeight").toReal(),
                               .cellWidth = provider->cellWidth(),
                               .cellHeight = provider->cellHeight(),
                               .widthQuantum = provider->widthQuantum() };
    };

    SECTION("native is the Linux header bar's height")
    {
        CHECK(chromeHeightUnder(contour::config::UiStyle::Native).value == 46.0);
    }

    SECTION("terminal is exactly one cell row")
    {
        auto const terminal = chromeHeightUnder(contour::config::UiStyle::Terminal);
        CHECK(terminal.cellHeight > 0.0);
        CHECK(terminal.value == terminal.cellHeight);
        // A status line is short: whatever the font, one row must be less than the GUI bar it replaces
        // at any sane size -- otherwise the style is not buying the vertical space it promises.
        CHECK(terminal.value < 34.0);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The tab strip's \"+\" and \"▾\" are padded wider than a tab's close button (offscreen)",
          "[contour][gui][qml]")
{
    // Both are permanently visible, frequently used targets sitting at the end of the strip, so a
    // width borrowed from the close glyph inside a tab makes them needlessly hard to hit. They take
    // their own token instead; this pins that they read it, in both styles, rather than falling back
    // to controlWidth. The native strip that draws them is Fluent's joined one (the pill rows put a "+" in
    // the band instead).
    contour::test::QmlMessageCapture const warnings;
    auto const style = GENERATE(contour::config::UiStyle::Native, contour::config::UiStyle::Terminal);

    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    auto setup = contour::test::defaultChromeSetup(style);
    if (style == contour::config::UiStyle::Native)
        setup.row = contour::config::ChromeStyle::Fluent;
    contour::test::installChromeStyle(engine, setup);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabStrip.qml")));
    INFO("TabStrip errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    std::unique_ptr<QObject> strip(component.createWithInitialProperties(initial));
    REQUIRE(strip != nullptr);

    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    REQUIRE(provider->stripButtonWidth() > provider->controlWidth());

    // The joined strip's caret takes its own, narrower token (size.profileButton); 0 elsewhere.
    auto const profileButton = chromeToken(engine, "size", "profileButton");
    for (auto const* name: { "newTabButton", "newTabMenuButton" })
    {
        INFO("button: " << name);
        auto const* button = strip->findChild<QQuickItem*>(QLatin1StringView(name));
        REQUIRE(button != nullptr);
        auto const caret = std::string_view { name } == "newTabMenuButton" && profileButton > 0;
        CHECK(button->width() == (caret ? profileButton : provider->stripButtonWidth()));
        CHECK(button->width() > provider->controlWidth());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The window controls sit on the side their style asks for (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // The bug this whole setting exists for: on macOS the controls drew in the top-RIGHT corner,
    // Windows-fashion. Placement is not an anchor here but a pair of Loaders the configured side
    // switches between, so what is pinned is that the ACTIVE one is the right one and that the
    // inactive one reserves no width at all -- an inactive Loader that still had an implicit size
    // would push the tab strip across by the width of three buttons.
    //
    // Run under both chrome styles: the two are orthogonal by design, and a style that only worked
    // in the native chrome would be exactly the regression this split was meant to prevent.
    contour::test::QmlMessageCapture const warnings;
    auto const uiStyle = GENERATE(contour::config::UiStyle::Native, contour::config::UiStyle::Terminal);

    struct Placement
    {
        contour::config::WindowControlStyle style;
        char const* active;   //!< The Loader that must hold the controls.
        char const* inactive; //!< The one that must hold nothing and take no width.
        char const* inward;   //!< The control whose edge faces the rest of the bar, not the corner.
    };

    auto const placement = GENERATE(Placement { contour::config::WindowControlStyle::MacOS,
                                                "leadingWindowControls",
                                                "trailingWindowControls",
                                                "maximize" },
                                    Placement { contour::config::WindowControlStyle::Windows,
                                                "trailingWindowControls",
                                                "leadingWindowControls",
                                                "minimize" },
                                    Placement { contour::config::WindowControlStyle::Plasma,
                                                "trailingWindowControls",
                                                "leadingWindowControls",
                                                "minimize" });

    INFO("ui style: " << static_cast<int>(uiStyle));
    INFO("window control style: " << static_cast<int>(placement.style));

    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTitleBarHost(engine, controller, uiStyle, placement.style);
    auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);

    auto* active = bar->findChild<QQuickItem*>(QLatin1StringView(placement.active));
    auto* inactive = bar->findChild<QQuickItem*>(QLatin1StringView(placement.inactive));
    REQUIRE(active != nullptr);
    REQUIRE(inactive != nullptr);

    // The active slot really loaded something, and it is wide enough to be three controls rather
    // than a collapsed remnant.
    CHECK(active->property("active").toBool());
    CHECK(active->width() > 0.0);
    // The other one holds nothing and costs nothing.
    CHECK(!inactive->property("active").toBool());
    CHECK(inactive->width() == 0.0);

    // And it is on the side it claimed. The drag region is the elastic middle, so being left of it
    // versus right of it is the whole difference between the two corners.
    auto* dragRegion = bar->findChild<QQuickItem*>(QStringLiteral("dragRegion"));
    REQUIRE(dragRegion != nullptr);
    auto const controls = sceneRectOf(*active);
    auto const drag = sceneRectOf(*dragRegion);

    if (placement.style == contour::config::WindowControlStyle::MacOS)
        CHECK(controls.right() <= drag.left());
    else
        CHECK(controls.left() >= drag.right());

    // And the group keeps a gutter of clear space between its innermost control and whatever the bar
    // puts beside it. Under the macOS style that neighbour is the TAB STRIP rather than the elastic
    // drag region, so without it the green light ends exactly where the first tab begins.
    //
    // Measured as the slack inside the group's own box rather than against the neighbour's rect,
    // because that is what says the space is INERT: it belongs to no control, so a click in it
    // activates nothing. The margin of a pixel is the layout's own snap. @see
    // UiStyleTokens::windowControlGutterUnits.
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);

    auto const* inward =
        findItemNamed(*bar, QStringLiteral("windowControl_") + QLatin1StringView(placement.inward));
    REQUIRE(inward != nullptr);
    auto const inwardRect = sceneRectOf(*inward);

    auto const gutter = placement.style == contour::config::WindowControlStyle::MacOS
                            ? controls.right() - inwardRect.right()
                            : inwardRect.left() - controls.left();
    INFO("gutter: " << gutter << ", expected at least: " << provider->windowControlGutter());
    CHECK(gutter >= provider->windowControlGutter() - 1.0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Every window control takes its width from the active chrome style (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // The requirement that made the extents live in UiStyleTokens rather than in the window-control
    // row: whatever the window-control style, its controls must be measured by whichever chrome is
    // active, so `ui_style: terminal` sizes them in cells and `native` in pixels. Pinned per control
    // rather than over the group's total, because Qt Quick Layouts snap each child to a whole pixel
    // -- so the SUM is never an exact multiple of a fractional cell, and asserting that it were would
    // be asserting away a property the tab strip's own buttons have had all along.
    contour::test::QmlMessageCapture const warnings;
    auto const uiStyle = GENERATE(contour::config::UiStyle::Native, contour::config::UiStyle::Terminal);
    auto const style = GENERATE(contour::config::WindowControlStyle::Windows,
                                contour::config::WindowControlStyle::MacOS,
                                contour::config::WindowControlStyle::Plasma);
    INFO("ui style: " << static_cast<int>(uiStyle));
    INFO("window control style: " << static_cast<int>(style));

    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTitleBarHost(engine, controller, uiStyle, style);
    auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);

    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);

    // A traffic light is a dot plus its gap -- the gap is part of the control so that it is
    // clickable -- and a button is a full window-control width. Both come from the chrome style, so
    // under Terminal both are counts of cells and under Native both are pixels.
    //
    // Except for the LAST traffic light, which is a bare dot: its gap would have no neighbour on the
    // far side of it, so it would be a clickable strip in what reads as bare title bar. macOS draws
    // close, minimize, maximize in that order, so maximize is the one; the Windows order ends in close.
    //
    // The Terminal row draws every style's controls as cells (config::presentationFor).
    auto const trafficLights =
        style == contour::config::WindowControlStyle::MacOS || uiStyle == contour::config::UiStyle::Terminal;
    auto const lastAction = style == contour::config::WindowControlStyle::MacOS ? "maximize" : "close";
    auto const expectedFor = [&](std::string_view action) {
        if (!trafficLights)
            return provider->windowControlWidth();
        return action == lastAction ? provider->trafficLightDotSize()
                                    : provider->trafficLightDotSize() + provider->trafficLightGap();
    };

    // A traffic light's HEIGHT is bounded too, and that is a hit-target fact rather than a cosmetic
    // one: a title bar is mostly drag region, its top edge belongs to the resize border, and the
    // group's hover reveal covers exactly the extent its controls occupy. Were a control stretched to
    // the full bar height, a click well above or below a 12px dot would close the window. A button
    // presentation is the opposite case -- its hover fill IS a full-height rectangle.
    auto const expectedHeight = trafficLights
                                    ? std::min(provider->chromeHeight(),
                                               provider->trafficLightDotSize() + provider->trafficLightGap())
                                    : bar->height();

    for (auto const* action: { "close", "minimize", "maximize" })
    {
        auto const name = QStringLiteral("windowControl_") + QLatin1StringView(action);
        auto const* control = findItemNamed(*bar, name);
        REQUIRE(control != nullptr);
        auto const expected = expectedFor(action);
        INFO("control: " << action << ", width: " << control->width() << ", expected: " << expected);
        // Within a pixel, which is the layout's own snap and not slack in the token arithmetic.
        CHECK(control->width() == Catch::Approx(expected).margin(1.0));
        INFO("control: " << action << ", height: " << control->height() << ", expected: " << expectedHeight);
        CHECK(control->height() == Catch::Approx(expectedHeight).margin(1.0));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The window controls change corner when the style does, without rebuilding (offscreen)",
          "[contour][gui][qml][titlebar][windowcontrols]")
{
    // The branch's headline promise: the settings page moves the controls with no restart. That is
    // the ONE thing every other window-control test cannot see, because each builds its engine with
    // the final style already installed -- so a `side` property quietly turned CONSTANT, or a Loader
    // whose `active:` binding became a one-shot assignment, would leave all of them passing while
    // the live path silently stopped working. Which is the same class of gap that let the
    // ReloadConfig path ship unwired.
    contour::test::QmlMessageCapture const warnings;

    QQmlEngine engine;
    MockTabController controller;
    // Fluent's band, whose trailing end holds the controls alone (the header bar's items sit there too).
    auto setup = contour::test::defaultChromeSetup(contour::config::UiStyle::Native,
                                                   contour::config::WindowControlStyle::Windows);
    setup.row = contour::config::ChromeStyle::Fluent;
    auto const host = makeTitleBarHostWith(engine, controller, setup);
    auto* bar = host->findChild<QQuickItem*>(QStringLiteral("titleBar"));
    REQUIRE(bar != nullptr);

    auto* leading = bar->findChild<QQuickItem*>(QStringLiteral("leadingWindowControls"));
    auto* trailing = bar->findChild<QQuickItem*>(QStringLiteral("trailingWindowControls"));
    REQUIRE(leading != nullptr);
    REQUIRE(trailing != nullptr);

    // Windows: on the right, as Contour has always drawn them.
    CHECK(!leading->property("active").toBool());
    CHECK(trailing->property("active").toBool());

    auto* provider = engine.rootContext()
                         ->contextProperty(QStringLiteral("windowControls"))
                         .value<contour::window::WindowControlStyleProvider*>();
    REQUIRE(provider != nullptr);

    // Exactly what ContourGuiApp::applyWindowControlStyle() does on Apply.
    provider->update(
        contour::config::WindowControlStyle::MacOS,
        contour::config::windowButtonPlacement(contour::config::WindowControlStyle::MacOS, std::nullopt));

    // The `active` bindings re-evaluate the moment the provider says so, but the WIDTHS follow a
    // layout polish, which is a render-loop pass rather than an event -- so this waits for the
    // geometry rather than assuming one processEvents() is enough. A failure here times out and then
    // fails on the assertions below with the stale numbers, which is what says what went wrong.
    CHECK(QTest::qWaitFor([&] { return leading->width() > 0.0 && trailing->width() == 0.0; }, 2000));

    // They moved corner, and the vacated slot gave its width back rather than holding it.
    CHECK(leading->property("active").toBool());
    CHECK(!trailing->property("active").toBool());
    CHECK(leading->width() > 0.0);
    CHECK(trailing->width() == 0.0);

    // The vacated corner gave its width back rather than holding it: an inactive Loader keeps the
    // implicit size of whatever it last held, so without a Layout.preferredWidth of its own the bar
    // would go on reserving three buttons' worth of space in the corner the controls just left --
    // which the drag region, being the elastic middle, is exactly what measures.
    auto const* dragRegion = bar->findChild<QQuickItem*>(QStringLiteral("dragRegion"));
    REQUIRE(dragRegion != nullptr);
    CHECK(sceneRectOf(*dragRegion).right() == Catch::Approx(sceneRectOf(*bar).right()));

    // And the group that moved is the macOS one, in the macOS order -- so what followed the style is
    // the whole row, not just which Loader was switched on.
    auto const* close = findItemNamed(*leading, QStringLiteral("windowControl_close"));
    auto const* maximize = findItemNamed(*leading, QStringLiteral("windowControl_maximize"));
    REQUIRE(close != nullptr);
    REQUIRE(maximize != nullptr);
    CHECK(sceneRectOf(*close).left() < sceneRectOf(*maximize).left());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
