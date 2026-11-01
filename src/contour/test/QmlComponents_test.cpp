// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end smoke tests for the GUI frontend's QML components.
//
// These run under the Qt "offscreen" platform plugin (no display server needed, so they work in
// CI). They instantiate each tab/title-bar QML component against a lightweight mock controller that
// mirrors the TerminalSessionManager interface the components rely on, and assert the component
// loads with no QML errors. This catches QML syntax/binding regressions deterministically without
// having to boot a full terminal session.
//
// QmlComponents_test.cpp holds the tab components, menus, the scrollbar and the chrome host. The remaining
// areas live in QmlPaneCards_test.cpp, QmlPalette_test.cpp, QmlTitleBar_test.cpp and
// QmlDialogs_test.cpp, split so that no single translation unit compiles the whole suite.

#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/DisplayPixels.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/ChannelPty.hpp>

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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour::test::qmlSupport;

namespace
{

using contour::test::ScrollingMockSession;

/// Loads a QML component from the contour resources and returns its error string list (empty when
/// the component is valid). The mock controller and a dummy window are exposed as context
/// properties so components that require them instantiate.
[[nodiscard]] QStringList loadErrors(QQmlEngine& engine, QString const& url)
{
    QQmlComponent const component(&engine, QUrl(url));
    QStringList errors;
    for (auto const& e: component.errors())
        errors << e.toString();
    return errors;
}

/// Asserts that @p menu is open at a size that can actually show its entries.
///
/// Shared by both menu-open cases, because the rule they exist to enforce is one rule: `width > 0`
/// is NOT enough. A menu collapsed to a two-pixel sliver -- which is what a background with no
/// implicit size produced -- passes that while being, on screen, a vertical line. Its height was
/// right throughout, since the entries stack vertically.
void checkMenuOpensAtUsableSize(QObject& menu)
{
    CHECK(menu.property("opened").toBool());
    CHECK(menu.property("count").toInt() > 0);

    QQuickItem* firstItem = nullptr;
    REQUIRE(QMetaObject::invokeMethod(&menu, "itemAt", Q_RETURN_ARG(QQuickItem*, firstItem), Q_ARG(int, 0)));
    REQUIRE(firstItem != nullptr);
    auto const widest = firstItem->property("implicitWidth").toReal();
    REQUIRE(widest > 0.0);

    INFO("menu " << menu.property("width").toReal() << "x" << menu.property("height").toReal()
                 << ", widest item " << widest);
    CHECK(menu.property("width").toReal() >= widest);
    CHECK(menu.property("height").toReal() > 0.0);

    // A null background is what a failed surface leaves behind, and a menu with no background is an
    // unreadable stack of text over the terminal.
    auto* background = menu.property("background").value<QObject*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("width").toReal() > 0.0);
    CHECK(background->property("height").toReal() > 0.0);
}

/// The rectangle the flyout actually occupies in its window — which is NOT what its `y` property says.
/// A Popup's x/y are the position REQUESTED against its parent; Qt then clamps the popup item itself back
/// inside the window (for a popup whose `margins` are >= 0) without touching those properties. Only the
/// item's own geometry says what the user can see, so every placement assertion below reads it.
[[nodiscard]] QRectF flyoutWindowRect(QObject* flyout)
{
    auto* content = flyout->property("contentItem").value<QQuickItem*>();
    if (content == nullptr || content->parentItem() == nullptr)
        return {};
    auto* popupItem = content->parentItem(); // the Popup's own item; the content sits inside it
    return { popupItem->mapToScene(QPointF(0, 0)), QSizeF(popupItem->width(), popupItem->height()) };
}

/// The tab delegate the strip in @p host shows at @p index, or null.
[[nodiscard]] QQuickItem* tabDelegate(QObject& host, int index)
{
    auto* list = host.findChild<QObject*>(QStringLiteral("tabList"));
    if (list == nullptr)
        return nullptr;
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
    return item;
}

/// The colour @p role resolves to on a pane of @p background, as the chrome's colorOn() answers it.
[[nodiscard]] QColor colorOnPane(QQmlEngine& engine,
                                 contour::config::ChromeColorRole role,
                                 QColor const& background)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto color = QColor {};
    REQUIRE(QMetaObject::invokeMethod(style,
                                      "colorOn",
                                      Q_RETURN_ARG(QColor, color),
                                      Q_ARG(int, static_cast<int>(role)),
                                      Q_ARG(QColor, background)));
    return color;
}

/// A MockSession whose viewport glides, as TerminalSession's does: the session moves its own view frame by
/// frame and announces each move, and a write from the chrome is the user's scroll, which ends a glide.
class GlidingMockSession final: public MockSession
{
    Q_OBJECT
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)

  public:
    [[nodiscard]] int scrollOffset() const { return _scrollOffset; }

    /// The chrome's write: the user's scroll, as TerminalSession::setScrollOffset() takes it.
    void setScrollOffset(int value)
    {
        written.push_back(value);
        moveTo(value);
    }

    /// One frame of a glide: the session moves its own view, and announces it.
    void glideTo(int value) { moveTo(value); }

    std::vector<int> written; ///< Every offset the chrome wrote, in order.

  signals:
    void scrollOffsetChanged();

  private:
    void moveTo(int value)
    {
        if (value == _scrollOffset)
            return;
        _scrollOffset = value;
        emit scrollOffsetChanged();
    }

    int _scrollOffset = 0;
};

/// A live session in an offscreen window, wired as TerminalPane.qml wires one: a DisplayHarness's real
/// TerminalDisplay bound to a real TerminalSession (over a ChannelPty, so whatever the terminal answers can
/// be read back), with the real SessionChrome laid over it.
///
/// Torn down in the reverse of its members' order: the chrome first, while the session it names still runs;
/// then the display harness, in the order production uses; then the engine the chrome came from, once
/// nothing it made is left.
struct LiveChromeHarness
{
    QQmlEngine engine;
    MockTabController tabs;
    contour::test::DisplayHarness live { contour::test::HarnessWindow::Unrendered };
    std::unique_ptr<QObject> chrome;
    QQuickItem* bar = nullptr;

    LiveChromeHarness()
    {
        engine.rootContext()->setContextProperty("terminalSessions", &tabs);
        contour::test::installChromeStyle(engine);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO("SessionChrome errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        chrome.reset(component.createWithInitialProperties(
            { { QStringLiteral("session"), QVariant::fromValue(static_cast<QObject*>(live.session.get())) },
              { QStringLiteral("displayItem"),
                QVariant::fromValue(static_cast<QObject*>(live.display.get())) } }));
        auto* chromeItem = qobject_cast<QQuickItem*>(chrome.get());
        REQUIRE(chromeItem != nullptr);
        chromeItem->setParentItem(live.display.get());

        QCoreApplication::processEvents();
        bar = chromeItem->findChild<QQuickItem*>(QStringLiteral("verticalScrollBar"));
        REQUIRE(bar != nullptr);
    }

    /// Whether the terminal has a selection under way or made.
    [[nodiscard]] bool hasSelection() const
    {
        auto const lock = std::scoped_lock { live.session->terminal() };
        return live.session->terminal().selector() != nullptr;
    }

    /// Presses the left button at @p point, in the window's coordinates, and lets the press land.
    void press(QPoint point) const
    {
        QTest::mousePress(live.window.get(), Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
    }

    /// Releases the left button at @p point, in the window's coordinates.
    void release(QPoint point) const
    {
        QTest::mouseRelease(live.window.get(), Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
    }

    /// Whether the scrollbar holds the press.
    [[nodiscard]] bool isBarPressed() const { return bar->property("pressed").toBool(); }
};

/// @p count numbered lines of output, each terminated: enough of them push rows into the history.
[[nodiscard]] std::string numberedLines(int count)
{
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, count))
        lines += std::format("line {}\r\n", i);
    return lines;
}

} // namespace

TEST_CASE("GUI QML tab components load without errors (offscreen)", "[contour][gui][qml]")
{
    // The Catch2 main builds a QGuiApplication for us (see test_main.cpp).
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    // The Settings pages' preview is a terminal display, which only the GUI action registers: without the
    // stub ProfilePage and SettingsPage fail to load whenever no earlier case of the run registered one.
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    // Each component is registered in resources.qrc under qrc:/qt/qml/Contour/Ui/.
    auto const components = std::array {
        QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/TabColorFlyout.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/TabStrip.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/WindowControls.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ResizeBorder.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/TitleBar.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SearchBar.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/CommandPalette.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SaveLayoutDialog.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ActionContextMenu.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/MenuSwatchRow.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsSidebar.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsGroup.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsRow.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsGroupsPage.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsSectionPage.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ProfilePage.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ConfirmDialog.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ColorSchemesPage.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SchemeCard.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/LinkPatternsPage.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsPage.qml"),
    };

    for (auto const& url: components)
    {
        INFO("component: " << url.toStdString());
        auto const errors = loadErrors(engine, url);
        // In one message: an INFO inside a loop ends with its iteration, before the CHECK could print it.
        INFO("errors: " << errors.join(QStringLiteral("\n")).toStdString());
        CHECK(errors.isEmpty());
    }
}

TEST_CASE("The terminal context menu opens, sub-menus and all (offscreen)", "[contour][gui][qml]")
{
    // The gap that let a broken right-click menu ship. The case below builds this same menu and
    // asserts its rows, but says outright that it never popup()s it -- so everything that can only
    // fail once a menu is OPEN went uncovered: its background, its margins, and the runtime-built
    // sub-menus, which are popups of their own and get none of the parent's treatment for free.
    //
    // Opening one offscreen needs a window to open INTO, which is the whole difference from the case
    // below: hosted in a Window, popup() works under the offscreen platform.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);

    // A state that produces sub-menus: profileNames is what "Change Profile" expands into.
    auto const state = contour::command::ContextMenuState {
        .hasSelection = true,
        .clipboardHasText = true,
        .hasLastCommand = false,
        .hasLocalWorkingDirectory = true,
        .hasSplits = false,
        .hyperlinkUnderCursor = "",
        .foldLine = std::nullopt,
        .activeProfile = "dark",
        .profileNames = { "dark", "light" },
    };
    auto const tree = contour::command::buildMenuTree(
        contour::command::buildContextMenu(state), {}, contour::command::ShortcutStyle::Text);
    auto const model = contour::window::toContextMenuModel(tree.nodes);
    REQUIRE_FALSE(model.isEmpty());

    QQmlComponent hostComponent(&engine);
    hostComponent.setData(QByteArrayLiteral("import QtQuick\n"
                                            "import QtQuick.Window\n"
                                            "import Contour.Ui\n"
                                            "Window {\n"
                                            "  width: 800; height: 600; visible: true\n"
                                            "  property alias menu: contextMenu\n"
                                            "  ActionContextMenu { id: contextMenu }\n"
                                            "}\n"),
                          QUrl(QStringLiteral("qrc:/contour/test/TerminalMenuHost.qml")));
    INFO("host errors: " << hostComponent.errorString().toStdString());
    REQUIRE(hostComponent.isReady());

    std::unique_ptr<QObject> host(hostComponent.create());
    REQUIRE(host != nullptr);
    auto* menu = host->property("menu").value<QObject*>();
    REQUIRE(menu != nullptr);
    menu->setProperty("entries", model);
    QCoreApplication::processEvents();

    REQUIRE(QMetaObject::invokeMethod(menu, "popup"));
    QCoreApplication::processEvents();

    checkMenuOpensAtUsableSize(*menu);
    CHECK(menu->property("count").toInt() == model.size());

    // And the sub-menus this case exists for. They are ContourMenus of their own, built at runtime,
    // and get none of the parent's treatment for free -- which is exactly how one of them shipped
    // with a background but no margin, casting a shadow into the window edge where nobody saw it.
    auto submenus = 0;
    for (auto const i: std::views::iota(0, menu->property("count").toInt()))
    {
        QQuickItem* row = nullptr;
        REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, i)));
        if (row != nullptr && row->property("subMenu").value<QObject*>() != nullptr)
            ++submenus;
    }
    INFO("sub-menus found: " << submenus);
    CHECK(submenus > 0);

    QMetaObject::invokeMethod(menu, "close");
    QCoreApplication::processEvents();

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Terminal context menu builds its rows from the C++ model (offscreen)",
          "[contour][gui][qml][menus]")
{
    // End-to-end over the real bridge: the pure table (ContextMenu.h) -> the Qt model (ContextMenuModel.h)
    // -> the QML that turns rows into menu entries. Nothing is hand-rolled here except the state, so a row
    // that the table stops producing, or an actionId that stops lining up, fails HERE rather than in a
    // silent no-op at runtime.
    //
    // The menu is never popup()'d: offscreen there is no overlay to open into. That is exactly why
    // ActionContextMenu.qml populates on Component.onCompleted / model change rather than in an
    // about-to-show hook — the rows are there to be asserted the moment the component is complete.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    auto const state = contour::command::ContextMenuState {
        .hasSelection = false, // Copy must come out present-but-disabled
        .clipboardHasText = true,
        .hasLastCommand = false, // the three "last command" rows must be absent
        .hasLocalWorkingDirectory = true,
        .hasSplits = false,
        .hyperlinkUnderCursor = "", // the two hyperlink rows must be absent
        .foldLine = std::nullopt,   // the fold row must be present but grayed
        .activeProfile = "dark",
        .profileNames = { "dark", "light" },
    };

    auto const tree = contour::command::buildMenuTree(
        contour::command::buildContextMenu(state), {}, contour::command::ShortcutStyle::Text);
    auto const model = contour::window::toContextMenuModel(tree.nodes);
    REQUIRE_FALSE(model.isEmpty());

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ActionContextMenu.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("entries", model);
    std::unique_ptr<QObject> menu(component.createWithInitialProperties(initial));
    REQUIRE(menu != nullptr);

    // The component announces a pick rather than calling a controller, which is what lets one component
    // serve both menu surfaces; its host decides where that goes.
    QSignalSpy const pickedSpy(menu.get(), SIGNAL(picked(int)));

    // Every row became an entry: separators and the "Advanced"/"Change Profile" sub-menus included. A
    // native popup (the Windows trap ContourMenu documents) would have come up empty.
    auto const count = menu->property("count");
    REQUIRE(count.isValid());
    CHECK(count.toInt() == model.size());

    // Picking "Copy" must reach C++ carrying the action the row was BUILT with — the whole reason the row
    // carries an actionId rather than a name to be looked up later. Copy is row 0, hence actionId 0.
    QQuickItem* copyItem = nullptr;
    REQUIRE(
        QMetaObject::invokeMethod(menu.get(), "itemAt", Q_RETURN_ARG(QQuickItem*, copyItem), Q_ARG(int, 0)));
    REQUIRE(copyItem != nullptr);
    CHECK(copyItem->property("text").toString() == QStringLiteral("Copy"));
    CHECK_FALSE(copyItem->property("enabled").toBool()); // no selection

    // MenuItem exposes triggered() as a SIGNAL, and emitting it by name is what a click would do.
    //
    // A row triggered on a menu that is not showing is picked AT ONCE: only an open menu defers its pick
    // until it has closed, and a closed one has no close to wait for.
    REQUIRE(QMetaObject::invokeMethod(copyItem, "triggered"));
    REQUIRE(pickedSpy.count() == 1);
    CHECK(pickedSpy.at(0).at(0).toInt() == 0);
    REQUIRE_FALSE(tree.actions.empty());
    CHECK(contour::command::commandId(tree.actions[0]) == "CopySelection");

    SECTION("rebuilding it, as every right-click does, leaks nothing")
    {
        // WindowController republishes the model on EVERY right-click, and each republish rebuilds the
        // menu. A sub-menu does not sit in the content model itself — a Menu is a Popup, not an Item — so
        // Qt represents it there with a proxy MenuItem that addMenu() creates and owns. takeItem() hands
        // that proxy back WITHOUT destroying it, and it is not among the objects the QML tracks: nothing
        // would ever destroy it. Two fully-built controls leaked per right-click, for the life of the
        // window. takeMenu() is the call that disposes of it.
        auto const settle = [] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        };

        settle();
        auto const baseline = menu->children().size();
        REQUIRE(baseline > 0); // the two sub-menus are parented here

        for (auto i = 0; i < 25; ++i)
        {
            REQUIRE(QMetaObject::invokeMethod(menu.get(), "rebuild"));
            settle();
        }

        // The menu still holds exactly the rows it was built with...
        CHECK(menu->property("count").toInt() == model.size());
        // ...and not one object more than the first build left behind. Before the fix this grew by two on
        // every single iteration.
        CHECK(menu->children().size() == baseline);
    }
}

TEST_CASE("an in-scene menu takes its row's radii (offscreen)", "[contour][gui][qml][contextmenu]")
{
    using contour::config::ChromeStyle;
    struct Expected
    {
        ChromeStyle row;
        qreal menu;
        qreal menuRow;
    };
    // The kit's `radius.menu · row`: 8 · 4 on Windows, square on the Terminal row.
    for (auto const& expected: { Expected { .row = ChromeStyle::Fluent, .menu = 8, .menuRow = 4 },
                                 Expected { .row = ChromeStyle::Terminal, .menu = 0, .menuRow = 0 } })
    {
        INFO("row: " << static_cast<int>(expected.row));
        contour::test::QmlMessageCapture const warnings;
        QQmlEngine engine;
        contour::test::installChromeStyle(engine,
                                          contour::test::ChromeSetup {
                                              .row = expected.row,
                                              .font = QGuiApplication::font(),
                                              .windowControls = contour::config::WindowControlStyle::Windows,
                                          });
        CHECK(chromeToken(engine, "radii", "menu") == expected.menu);
        CHECK(chromeToken(engine, "radii", "menuRow") == expected.menuRow);

        auto const state = contour::command::ContextMenuState {
            .hasSelection = true,
            .clipboardHasText = true,
            .hasLastCommand = false,
            .hasLocalWorkingDirectory = true,
            .hasSplits = false,
            .hyperlinkUnderCursor = "",
            .foldLine = std::nullopt,
            .activeProfile = "dark",
            .profileNames = { "dark", "light" },
        };
        auto const tree = contour::command::buildMenuTree(
            contour::command::buildContextMenu(state), {}, contour::command::ShortcutStyle::Text);
        auto const model = contour::window::toContextMenuModel(tree.nodes);

        QQmlComponent hostComponent(&engine);
        hostComponent.setData(QByteArrayLiteral("import QtQuick\n"
                                                "import QtQuick.Window\n"
                                                "import Contour.Ui\n"
                                                "Window {\n"
                                                "  width: 800; height: 600; visible: true\n"
                                                "  property alias menu: contextMenu\n"
                                                "  ActionContextMenu { id: contextMenu }\n"
                                                "}\n"),
                              QUrl(QStringLiteral("qrc:/contour/test/MenuRadiiHost.qml")));
        INFO("host errors: " << hostComponent.errorString().toStdString());
        REQUIRE(hostComponent.isReady());
        std::unique_ptr<QObject> host(hostComponent.create());
        REQUIRE(host != nullptr);
        auto* menu = host->property("menu").value<QObject*>();
        REQUIRE(menu != nullptr);
        menu->setProperty("entries", model);
        REQUIRE(QMetaObject::invokeMethod(menu, "popup"));
        QCoreApplication::processEvents();

        // The surface.
        auto* surface = menu->property("background").value<QQuickItem*>();
        REQUIRE(surface != nullptr);
        CHECK(surface->property("radius").toReal() == expected.menu);

        // A highlighted command row, and a sub-menu's row, which the menu builds itself.
        menu->setProperty("currentIndex", 0);
        QCoreApplication::processEvents();
        auto rowsChecked = 0;
        for (auto const i: std::views::iota(0, menu->property("count").toInt()))
        {
            QQuickItem* row = nullptr;
            REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, i)));
            auto const* highlight =
                row != nullptr ? row->property("background").value<QQuickItem*>() : nullptr;
            auto const isHighlighted = row != nullptr && row->property("highlighted").toBool();
            auto const isSubMenu = row != nullptr && row->property("subMenu").value<QObject*>() != nullptr;
            if (highlight == nullptr || !(isHighlighted || isSubMenu))
                continue;
            INFO("row " << i);
            CHECK(highlight->property("radius").toReal() == expected.menuRow);
            ++rowsChecked;
        }
        // The highlighted row and at least one sub-menu row.
        CHECK(rowsChecked >= 2);

        QMetaObject::invokeMethod(menu, "close");
        QCoreApplication::processEvents();
        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("GUI tab strip instantiates and binds against a populated model (offscreen)", "[contour][gui][qml]")
{
    // Catching delegate binding errors (e.g. an undefined `model` reference) requires actually
    // instantiating the strip against a model with rows, not just loading the component.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabStrip.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    // TabStrip now requires a `window` (threaded to delegates so a closed rename can restore terminal
    // focus). This test never triggers a rename, so any non-null QObject satisfies the required property.
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    std::unique_ptr<QObject> strip(component.createWithInitialProperties(initial));
    REQUIRE(strip != nullptr);

    auto* item = qobject_cast<QQuickItem*>(strip.get());
    REQUIRE(item != nullptr);
    // The strip realizes its three mock tabs without QML runtime errors (which would otherwise be
    // reported on the component as creation errors).
    CHECK(controller.rowCount() == 3);
}

TEST_CASE("a tab drag carries the node payload (offscreen)", "[contour][gui][qml][drop]")
{
    // The tab drags as the node it is (`application/x-contour-node`), as a pane card does; the controller
    // writes the JSON and reads it back, so nothing in the strip names a window or a row.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 3);
    initial.insert("tabTitle", QStringLiteral("shell"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", QColor {});
    initial.insert("tabActive", false);
    initial.insert("tabPaneCount", 1);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);
    // In a shown window, as a drag needs one to run in.
    auto window = QQuickWindow {};
    auto* const tabItem = qobject_cast<QQuickItem*>(tab.get());
    REQUIRE(tabItem != nullptr);
    tabItem->setParentItem(window.contentItem());
    tabItem->setSize(QSizeF { 120, 30 });
    window.resize(200, 60);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));

    // Starting it asks the controller, which writes the payload, and puts exactly that payload on the drag.
    // The offscreen platform's drag manager returns from QDrag::exec() at once, with nothing having taken the
    // drop, so the whole drag runs inside carry().
    auto* const dragProxy = tab->findChild<QObject*>(QStringLiteral("dragProxy"));
    REQUIRE(dragProxy != nullptr);
    // With the button held, as it is for a drag that starts by dragging.
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, QPoint { 60, 15 });
    REQUIRE(QMetaObject::invokeMethod(dragProxy, "carry"));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint { 60, 15 });
    CHECK(controller.beganDrags == std::vector { controller.tabDragPayload(3) });
    auto const drag = contour::window::parseNodeDrag(controller.tabDragPayload(3));
    REQUIRE(drag.has_value());
    CHECK(drag->payload == contour::window::DropPayload::Tab);
    CHECK(drag->tabIndex == 3);
    auto mimeValue = QQmlProperty(dragProxy, QStringLiteral("Drag.mimeData"), qmlContext(dragProxy)).read();
    if (mimeValue.canConvert<QJSValue>())
        mimeValue = mimeValue.value<QJSValue>().toVariant();
    CHECK(mimeValue.toMap().value(QString::fromLatin1(contour::window::NodeDragMime)).toString()
          == controller.tabDragPayload(3));
    CHECK(mimeValue.toMap().size() == 1);

    // And its end is the controller's to decide: what the OS drag returned, never "cancelled" from here.
    REQUIRE(controller.finishedDrags.size() == 1);
    CHECK(controller.finishedDrags.front().dropAction == static_cast<int>(Qt::IgnoreAction));
    CHECK(controller.finishedDrags.front().payload == controller.tabDragPayload(3));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// A tab strip of @c count tabs in a window, laid out the way the title bar does, and a drag to send it.
struct StripHost
{
    QQmlEngine engine;
    MockTabController controller;
    QQuickWindow window;
    std::unique_ptr<QObject> strip;
    DragSource source;

    static constexpr auto WindowWidth = 1400;
    static constexpr auto WindowHeight = 60;

    /// @param count How many tabs the strip has.
    /// @param row   The chrome row the strip is drawn in; the default one when empty.
    explicit StripHost(int count = 3, std::optional<contour::config::ChromeStyle> row = std::nullopt):
        controller { count }, source { window }
    {
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        auto setup = contour::test::defaultChromeSetup();
        if (row)
            setup.row = *row;
        contour::test::installChromeStyle(engine, setup);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabStrip.qml")));
        INFO("TabStrip.qml: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        auto const initial =
            QVariantMap { { "controller", QVariant::fromValue(&controller) },
                          { "window", QVariant::fromValue(static_cast<QObject*>(&controller)) },
                          { "availableWidth", WindowWidth } };
        strip.reset(component.createWithInitialProperties(initial));
        REQUIRE(strip != nullptr);
        auto& item = *qobject_cast<QQuickItem*>(strip.get());
        item.setHeight(WindowHeight);
        item.setParentItem(window.contentItem());
        window.resize(WindowWidth, WindowHeight);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        auto const* const list = findItemNamed(item, QStringLiteral("tabList"));
        REQUIRE(list != nullptr);
        REQUIRE(QTest::qWaitFor([&] { return list->property("count").toInt() == count; }));
        // The strip tells the controller where its tabs stand only for a drag over it, which enters with
        // the first move: none came yet.
        QCoreApplication::processEvents();
        CHECK(controller.tabSpans.empty());
        source.mime.setData(contour::window::NodeDragMime, controller.tabDragPayload(2).toUtf8());
    }

    [[nodiscard]] QQuickItem& item(char const* name) const
    {
        auto* const found = findItemNamed(*qobject_cast<QQuickItem*>(strip.get()), QString::fromUtf8(name));
        INFO("item: " << name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] QQuickItem& tab(int index) const
    {
        auto* const list = &item("tabList");
        QQuickItem* delegate = nullptr;
        REQUIRE(QMetaObject::invokeMethod(
            list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate), Q_ARG(int, index)));
        REQUIRE(delegate != nullptr);
        return *delegate;
    }

    /// @return The point of tab @p index at @p fraction of its width, in window coordinates.
    [[nodiscard]] QPointF onTab(int index, qreal fraction) const
    {
        auto& delegate = tab(index);
        return delegate.mapToScene(QPointF { delegate.width() * fraction, delegate.height() / 2 });
    }

    /// @return The left edge of tab @p index (the boundary before it) in window coordinates; for an index
    /// past
    ///         the last, the right edge of the last.
    [[nodiscard]] QPointF boundary(int index) const
    {
        auto const count = controller.rowCount();
        auto& delegate = tab(std::min(index, count - 1));
        auto const x = index >= count ? delegate.width() : 0.0;
        return delegate.mapToScene(QPointF { x, delegate.height() / 2 });
    }

    /// @return The chrome's accent, which every "lands here" cue is drawn in.
    [[nodiscard]] QColor accent() const
    {
        auto* const style =
            engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        return style->property("accentColor").value<QColor>();
    }

    [[nodiscard]] QQuickItem& dwellBar(int index) const
    {
        auto* const found = findItemNamed(tab(index), QStringLiteral("tabDwellBar"));
        REQUIRE(found != nullptr);
        return *found;
    }
};
} // namespace

TEST_CASE("hovering another tab for the dwell opens it (offscreen)", "[contour][gui][qml][drop][tabdrag]")
{
    auto f = StripHost {};
    f.source.enter(f.onTab(1, 0.5));
    f.source.move(f.onTab(1, 0.5));
    CHECK(f.source.accepted);
    // The bar fills along the tab's foot meanwhile, and nothing has opened yet.
    CHECK(f.dwellBar(1).isVisible());
    // The tab is outlined 1.5 px in the accent, and the bar is the accent too.
    CHECK(f.dwellBar(1).property("color").value<QColor>() == f.accent());
    auto* const mark = findItemNamed(f.tab(1), QStringLiteral("tabDropMark"));
    REQUIRE(mark != nullptr);
    CHECK(QQmlProperty(mark, "border.color").read().value<QColor>() == f.accent());
    CHECK(QQmlProperty(mark, "border.width").read().toDouble() == Catch::Approx(1.5));
    CHECK(f.controller.activatedTabs.empty());

    constexpr auto Slack = 2000;
    REQUIRE(QTest::qWaitFor([&] { return !f.controller.activatedTabs.empty(); },
                            contour::window::UiStyleProvider::tabDwellMs() + Slack));
    CHECK(f.controller.activatedTabs == std::vector<int> { 1 });
    f.source.leave();
    CHECK(QTest::qWaitFor([&] { return !f.dwellBar(1).isVisible(); }));
}

TEST_CASE("leaving a tab before the dwell is up opens nothing (offscreen)",
          "[contour][gui][qml][drop][tabdrag]")
{
    auto f = StripHost {};
    f.source.enter(f.onTab(1, 0.5));
    f.source.move(f.onTab(1, 0.5));
    f.source.leave();
    QTest::qWait(contour::window::UiStyleProvider::tabDwellMs() + 300);
    CHECK(f.controller.activatedTabs.empty());
}

TEST_CASE("hovering the source tab never opens it (offscreen)", "[contour][gui][qml][drop][tabdrag]")
{
    auto f = StripHost {};
    f.controller.springClosedTab = 1; // the controller refuses to open the tab the drag came from
    f.source.enter(f.onTab(1, 0.5));
    f.source.move(f.onTab(1, 0.5));
    // Accepted all the same, as every recognised drop is, but no bar fills and nothing opens.
    CHECK(f.source.accepted);
    CHECK_FALSE(f.dwellBar(1).isVisible());
    QTest::qWait(contour::window::UiStyleProvider::tabDwellMs() + 300);
    CHECK(f.controller.activatedTabs.empty());
    f.source.leave();
}

TEST_CASE("releasing on a tab calls dropNodeOnTab (offscreen)", "[contour][gui][qml][drop][tabdrag]")
{
    auto f = StripHost {};
    f.source.enter(f.onTab(1, 0.5));
    f.source.move(f.onTab(1, 0.5));
    CHECK(f.source.drop(f.onTab(1, 0.5)) == Qt::MoveAction);
    CHECK(f.controller.tabDrops
          == std::vector<std::pair<int, QString>> { { 1, f.controller.tabDragPayload(2) } });
    CHECK(f.controller.gapDrops.empty());
    // Released before the dwell was up, so nothing opened under the drop.
    QTest::qWait(contour::window::UiStyleProvider::tabDwellMs() + 300);
    CHECK(f.controller.activatedTabs.empty());
}

TEST_CASE("a gap shows the insertion slot and calls dropNodeInGap (offscreen)",
          "[contour][gui][qml][drop][tabdrag]")
{
    auto f = StripHost {};
    auto& slot = f.item("tabInsertionSlot");
    CHECK_FALSE(slot.isVisible());

    SECTION("between two tabs")
    {
        f.source.enter(f.onTab(0, 0.5));
        f.source.move(f.onTab(0, 0.5));
        CHECK_FALSE(slot.isVisible()); // over a tab, not a gap
        auto const resting = f.boundary(1).x();
        auto const secondRests = f.tab(1).mapToScene(QPointF {}).x();
        f.source.move(f.boundary(1));
        CHECK(f.source.accepted);
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        // Named for what is dragged, standing where the tabs after it have moved away from.
        CHECK(PaneTreeHost::partOf(slot, "tabInsertionSlotLabel").property("text").toString()
              == f.controller.dragName);
        CHECK(slot.mapToScene(QPointF {}).x() == Catch::Approx(resting).margin(1.0));

        // The tabs from the gap on make room: the slot's width, and their spacing, to the right.
        auto const room = slot.width() + f.item("tabList").property("spacing").toDouble();
        REQUIRE(
            QTest::qWaitFor([&] { return f.tab(1).mapToScene(QPointF {}).x() >= secondRests + room - 0.5; }));
        CHECK(f.tab(1).mapToScene(QPointF {}).x() == Catch::Approx(secondRests + room).margin(1.0));

        // Nothing overlaps a tab's title.
        auto const slotRect = QRectF { slot.mapToScene(QPointF {}), QSizeF { slot.width(), slot.height() } };
        for (auto const index: std::views::iota(0, 3))
        {
            auto* const label = findItemNamed(f.tab(index), QStringLiteral("tabLabel"));
            REQUIRE(label != nullptr);
            auto const titleRect =
                QRectF { label->mapToScene(QPointF {}), QSizeF { label->width(), label->height() } };
            INFO("tab " << index);
            CHECK_FALSE(titleRect.intersects(slotRect));
        }

        // The hit test is made against the geometry the pointer sees: the strip reported the shifted spans,
        // so the slot is the gap, and the tab after it starts after the slot.
        REQUIRE(QTest::qWaitFor([&] {
            return f.controller.tabSpans.size() == 3
                   && f.controller.tabSpans[1].left >= secondRests + room - 1.0;
        }));
        CHECK(f.controller.tabBarHit(slotRect.center().x()).hit().spot == contour::window::TabBarSpot::Gap);
        CHECK(f.controller.tabBarHit(slotRect.center().x()).hit().index == 1);
        CHECK(f.controller.tabBarHit(f.onTab(1, 0.5).x()).hit().spot == contour::window::TabBarSpot::OnTab);

        CHECK(f.source.drop(f.boundary(1)) == Qt::MoveAction);
        CHECK(f.controller.gapDrops
              == std::vector<std::pair<int, QString>> { { 1, f.controller.tabDragPayload(2) } });
        CHECK(f.controller.tabDrops.empty());
        CHECK(QTest::qWaitFor([&] { return !slot.isVisible(); }));
    }

    SECTION("in the empty bar after the last tab")
    {
        auto const beyond = f.boundary(3) + QPointF { 60, 0 };
        f.source.enter(beyond);
        f.source.move(beyond);
        CHECK(f.source.accepted);
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        // It runs on from the last tab.
        CHECK(slot.mapToScene(QPointF {}).x() >= f.boundary(3).x());
        CHECK(f.source.drop(beyond) == Qt::MoveAction);
        CHECK(f.controller.gapDrops
              == std::vector<std::pair<int, QString>> { { 3, f.controller.tabDragPayload(2) } });
    }
}

namespace
{
/// @return A frame of @p window once the scene has stopped changing by itself.
[[nodiscard]] QImage settledFrame(QQuickWindow& window)
{
    constexpr auto FrameSettleMs = 50;
    constexpr auto FrameSettleTimeoutMs = 10000;
    auto image = QImage {};
    REQUIRE(QTest::qWaitFor(
        [&] {
            QTest::qWait(FrameSettleMs);
            auto const next = window.grabWindow();
            auto const settled = !next.isNull() && next == image;
            image = next;
            return settled;
        },
        FrameSettleTimeoutMs));
    return image;
}

/// @return Where @p item stands in the pixels of a frame of @p window, rounded outward.
[[nodiscard]] QRect pixelsOf(QQuickWindow const& window, QQuickItem const& item)
{
    auto const dpr = window.devicePixelRatio();
    auto const rect = item.mapRectToScene(QRectF { QPointF {}, item.size() });
    return QRectF { rect.topLeft() * dpr, rect.size() * dpr }.toAlignedRect();
}

[[nodiscard]] vtbackend::RGBColor rgbOfColor(QColor const& color)
{
    return vtbackend::RGBColor { static_cast<uint8_t>(color.red()),
                                 static_cast<uint8_t>(color.green()),
                                 static_cast<uint8_t>(color.blue()) };
}
} // namespace

TEST_CASE("display: the insertion slot is drawn where the gap is, in the accent, and moves no tab before it",
          "[display][tabdrag][drag]")
{
    using namespace contour::test::pixel;
    REQUIRE_DISPLAY_OR_SKIP();
    auto f = StripHost {};
    auto const before = settledFrame(f.window);
    auto& slot = f.item("tabInsertionSlot");
    auto const accent = rgbOfColor(f.accent());
    // Probes and slack are stated in device-independent pixels, so they hold at any device-pixel ratio.
    auto const dpr = f.window.devicePixelRatio();

    SECTION("between two tabs")
    {
        f.source.enter(f.onTab(0, 0.5));
        f.source.move(f.onTab(0, 0.5));
        f.source.move(f.boundary(1));
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        auto const during = settledFrame(f.window);
        auto const slotPx = pixelsOf(f.window, slot);

        // The tab before the gap has not moved or changed; the slot's edge is the accent, dashed.
        auto const firstTab = pixelsOf(f.window, f.tab(0));
        CHECK(changedBounds(before, during, firstTab.adjusted(0, 0, -slotPx.width(), 0)).isNull());
        auto const band = probeAt({ slotPx.center().x(), slotPx.top() }, { -20, 0, 40, 3 }, dpr);
        auto const dashes = regionColors(during, band);
        INFO("dash ink " << hexOf(dashes.ink) << " accent " << hexOf(accent));
        CHECK(nearlyEqual(dashes.ink, accent, 24));
        CHECK(dashes.groundPixels < dashes.pixels);

        // The slot's tint is the accent's, not the bar's: the same spot was not that colour.
        auto const inside = probeAt({ slotPx.left(), slotPx.center().y() }, { 3, -2, 4, 4 }, dpr);
        CHECK(distanceBetween(regionColors(during, inside).ground, regionColors(before, inside).ground) > 0);
    }

    SECTION("in the empty bar after the last tab")
    {
        auto const beyond = f.boundary(3) + QPointF { 60, 0 };
        f.source.enter(beyond);
        f.source.move(beyond);
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        auto const during = settledFrame(f.window);
        auto const slotPx = pixelsOf(f.window, slot);

        // Everything that changed is the slot's: the tabs and the rest of the bar are as they were.
        auto const changed = changedBounds(before, during, QRect { QPoint {}, during.size() });
        REQUIRE_FALSE(changed.isNull());
        auto const slack = devicePixels(2, dpr);
        CHECK(slotPx.adjusted(-slack, -slack, slack, slack).contains(changed));
        CHECK(changed.width() >= slotPx.width() - devicePixels(6, dpr));

        auto const dashes =
            regionColors(during, probeAt({ slotPx.center().x(), slotPx.top() }, { -20, 0, 40, 3 }, dpr));
        CHECK(nearlyEqual(dashes.ink, accent, 24));
        CHECK(dashes.groundPixels < dashes.pixels);
    }
    f.source.leave();
}

TEST_CASE("display: the dwell bar fills along the hovered tab's foot in the accent, and the tab is outlined",
          "[display][tabdrag][drag]")
{
    using namespace contour::test::pixel;
    REQUIRE_DISPLAY_OR_SKIP();
    auto f = StripHost {};
    auto const before = settledFrame(f.window);
    auto const accent = rgbOfColor(f.accent());

    f.source.enter(f.onTab(1, 0.5));
    f.source.move(f.onTab(1, 0.5));
    auto& bar = f.dwellBar(1);
    constexpr auto Slack = 2000;
    REQUIRE(QTest::qWaitFor([&] { return bar.isVisible() && bar.width() > 8; }, Slack));
    auto const during = f.window.grabWindow();
    auto const tabPx = pixelsOf(f.window, f.tab(1));
    auto const barPx = pixelsOf(f.window, bar);
    INFO("bar " << barPx.width() << " of tab " << tabPx.width());

    // The bar is part of the way along the tab's foot, and drawn in the accent.
    CHECK(barPx.width() < tabPx.width());
    auto const dpr = f.window.devicePixelRatio();
    auto const barProbe = QRect { barPx.left() + devicePixels(1, dpr),
                                  barPx.center().y(),
                                  std::max(1, barPx.width() / 2),
                                  devicePixels(1, dpr) };
    CHECK(nearlyEqual(regionColors(during, barProbe).ground, accent, 8));

    // The tab is outlined in the accent, and no other tab changed.
    auto const top = probeAt({ tabPx.center().x(), tabPx.top() }, { -2, 0, 5, 1 }, dpr);
    CHECK(isBlendOf(accent, regionColors(before, top).ground, regionColors(during, top).ground));
    for (auto const index: { 0, 2 })
    {
        INFO("tab " << index);
        CHECK(changedBounds(before, during, pixelsOf(f.window, f.tab(index))).isNull());
    }
    f.source.leave();
}

TEST_CASE("a joined strip's new-tab button makes way for the insertion slot (offscreen)",
          "[contour][gui][qml][drop][tabdrag]")
{
    // The Fluent row's "+" follows the last tab, so there is no empty bar for the slot to stand in: the strip
    // grows by the room the slot takes, and the button and the tabs after the gap move on instead of being
    // drawn under it.
    auto f = StripHost { 3, contour::config::ChromeStyle::Fluent };
    auto& slot = f.item("tabInsertionSlot");
    auto& plus = f.item("newTabButton");
    auto const rectOf = [](QQuickItem& item) {
        return QRectF { item.mapToScene(QPointF {}), QSizeF { item.width(), item.height() } };
    };
    auto const resting = rectOf(plus).left();

    SECTION("after the last tab")
    {
        auto const beyond = f.boundary(3) + QPointF { 30, 0 };
        f.source.enter(beyond);
        f.source.move(beyond);
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        REQUIRE(QTest::qWaitFor([&] { return rectOf(plus).left() > resting + 1.0; }));
        CHECK(rectOf(slot).right() <= rectOf(plus).left() + 0.5);
        f.source.leave();
        CHECK(QTest::qWaitFor([&] { return rectOf(plus).left() == Catch::Approx(resting).margin(0.5); }));
    }

    SECTION("between two tabs")
    {
        f.source.enter(f.onTab(0, 0.5));
        f.source.move(f.boundary(1));
        REQUIRE(QTest::qWaitFor([&] { return slot.isVisible(); }));
        // The last tab is whole, and stands before the button.
        REQUIRE(QTest::qWaitFor([&] { return rectOf(plus).left() > resting + 1.0; }));
        auto const last = rectOf(f.tab(2));
        CHECK(last.right() <= rectOf(plus).left() + 0.5);
        auto& clip = f.item("tabList");
        CHECK(clip.width() >= last.right() - rectOf(clip).left() - 0.5);
        f.source.leave();
    }
}

TEST_CASE("a tab drag reorders through a gap (offscreen)", "[contour][gui][qml][drop][tabdrag]")
{
    // The third tab, taken to the gap before the first: the strip asks for that gap, and the controller (not
    // the strip) reorders. A drop in the gap is accepted whatever the controller then makes of it.
    auto f = StripHost {};
    auto const first = f.boundary(0) + QPointF { 3, 0 };
    f.source.enter(first);
    f.source.move(first);
    CHECK(f.source.accepted);
    CHECK(f.source.drop(first) == Qt::MoveAction);
    CHECK(f.controller.gapDrops
          == std::vector<std::pair<int, QString>> { { 0, f.controller.tabDragPayload(2) } });
}

TEST_CASE("A colored TabItem fills with the user color and picks contrasting text (offscreen)",
          "[contour][gui][qml]")
{
    // Drives the colored-tab branch of TabItem: when tabColor is non-transparent, the fill is the
    // user's color (faded per state, via the controller's TabColorScheme helpers) and the label
    // contrasts against it. The uncolored branch is covered by the load/strip tests above. Fluent's row,
    // whose tabs take the palette's (TabColorSource::Palette); the pill rows draw the roles.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Fluent;
    contour::test::installChromeStyle(engine, setup);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    // A dark palette red: active fill == the color itself; text must be white for contrast.
    auto const red = QColor(0xCC, 0x33, 0x33);
    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 0);
    initial.insert("tabTitle", QStringLiteral("shell"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", red);
    initial.insert("tabActive", true);
    initial.insert("tabPaneCount", 1);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);

    CHECK(tab->property("colored").toBool());
    // Active tab of a (nominally) focused window keeps the full color.
    CHECK(tab->property("effectiveBackground").value<QColor>() == red);
    // Red is dark in OKLab terms -> white label.
    CHECK(tab->property("foreground").value<QColor>() == QColor(Qt::white));

    // Deactivating fades the fill toward the title-bar background (so it differs from the raw color)
    // while the contrast decision still yields a legible label.
    tab->setProperty("tabActive", false);
    auto const inactiveFill = tab->property("effectiveBackground").value<QColor>();
    CHECK(inactiveFill != red);

    // The tab hands its color down to its color picker, which is what lands the keyboard cursor on the
    // swatch the tab already wears when the flyout opens (see the h/j/k/l navigation test).
    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);
    CHECK(flyout->property("currentColor").value<QColor>() == red);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The SetTabColor action opens the tab's color flyout (offscreen)", "[contour][gui][qml]")
{
    // The keyboard half of the tab-color feature, end to end through the QML: the controller emits
    // tabColorPickRequested(row) and the delegate whose row matches opens the ONE flyout it owns. This
    // is the reason the flyout was hoisted out of the tab's menu — there is no menu open here at all.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* tab = host->property("tabItem").value<QObject*>();
    REQUIRE(tab != nullptr);

    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);
    REQUIRE_FALSE(flyout->property("opened").toBool());

    SECTION("the request reaches the delegate whose row it names, and picking there colors that row")
    {
        // What the action does: WindowController::beginActiveTabColorPick() -> tabColorPickRequested(0).
        controller.beginActiveTabColorPick();
        QCoreApplication::processEvents();

        CHECK(flyout->property("opened").toBool());

        // It drops BELOW the tab. A Popup positions itself against its parent, which is now the tab
        // rather than the menu that used to own it, so without a placement of its own it would open at the
        // tab's own origin — covering the very tab strip the user is picking a color for. (This is the
        // strip-at-the-top case, where there is room below; the placement test below drives the other.)
        auto* tabItem = qobject_cast<QQuickItem*>(tab);
        REQUIRE(tabItem != nullptr);
        CHECK(tabItem->height() > 0);
        CHECK(flyout->property("y").toReal() == tabItem->height());

        // And a color picked in it lands on THIS tab, through the very same controller call the mouse
        // path makes — the flyout the keyboard opened is the one the context menu opens, bound to row 0.
        auto* hexField = flyout->findChild<QObject*>(QStringLiteral("customColorField"));
        REQUIRE(hexField != nullptr);
        hexField->setProperty("text", QStringLiteral("FF0000"));
        QMetaObject::invokeMethod(flyout, "applyCustomColor");
        QCoreApplication::processEvents();

        CHECK(controller.setTabColorCalls == 1);
        CHECK(controller.lastTabColorIndex == 0);
        CHECK(controller.lastTabColor == QColor(0xFF, 0x00, 0x00));
        CHECK_FALSE(flyout->property("opened").toBool()); // applying closes it
    }

    SECTION("a request naming ANOTHER row leaves this tab's flyout shut")
    {
        // Every realized delegate hears the same signal, so the row check is the only thing keeping the
        // wrong tab from popping its picker in the user's face.
        emit controller.tabColorPickRequested(1); // this TabItem is row 0
        QCoreApplication::processEvents();

        CHECK_FALSE(flyout->property("opened").toBool());
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("the tab menu opens with the table's rows (offscreen)", "[contour][gui][qml][menus]")
{
    // The strip's ONE menu, filled from the rows the controller publishes. It replaces two cases: one that
    // counted the hand-written TabContextMenu.qml's rows, and one that first OPENED a menu, since a menu
    // whose background or margins fail only at popup() time passed every test that merely built it.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller { 2 };
    auto const host = makeTabStripHost(engine, controller);

    // Empty until the controller publishes: the rows are the table's, not the QML's.
    auto const* unopened = host->findChild<QObject*>(QStringLiteral("tabContextMenu"));
    REQUIRE(unopened != nullptr);
    CHECK(unopened->property("count").toInt() == 0);

    auto* menu = openTabMenu(*host, controller, 1);
    CHECK(menu->property("count").toInt() == controller.tabContextMenuModel().size());
    checkMenuOpensAtUsableSize(*menu);

    // Below the right-clicked tab, not at the live pointer.
    auto const* tab = tabDelegate(*host, 1);
    REQUIRE(tab != nullptr);
    auto const tabBottomLeft = tab->mapToScene(QPointF(0, tab->height()));
    auto const opened = flyoutWindowRect(menu);
    INFO("menu at " << opened.x() << "," << opened.y() << "; tab 1 bottom-left at " << tabBottomLeft.x()
                    << "," << tabBottomLeft.y());
    CHECK(opened.top() >= tabBottomLeft.y() - 1);
    CHECK(std::abs(opened.left() - tabBottomLeft.x()) <= 1);

    QMetaObject::invokeMethod(menu, "close");
    QCoreApplication::processEvents();

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a swatch pick reaches the controller once the menu has closed (offscreen)",
          "[contour][gui][qml][menus]")
{
    // A pick is announced on CLOSE: what it does may open a popup of its own, which a menu still
    // dismissing would take the grab back from.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller { 2 };
    auto const host = makeTabStripHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);

    auto* menu = openTabMenu(*host, controller, 1);
    auto const colors = openTabColorMenu(*menu);
    REQUIRE(colors.swatches.size() == contour::test::MockTabPalette.size());

    // Whether the menu was still showing when each pick reached the controller.
    auto showingAtPick = std::vector<bool> {};
    controller.onTabMenuPick = [&](int) {
        showingAtPick.push_back(menu->property("visible").toBool());
    };

    auto const swatches = colors.row->property("swatches").toList();
    REQUIRE(swatches.size() == static_cast<qsizetype>(colors.swatches.size()));
    auto const expected = swatches[1].toMap().value(QStringLiteral("actionId")).toInt();

    auto const* swatch = colors.swatches[1];
    QTest::mouseClick(window,
                      Qt::LeftButton,
                      {},
                      swatch->mapToScene(QPointF(swatch->width() / 2, swatch->height() / 2)).toPoint());
    REQUIRE(QTest::qWaitFor([&] { return !menu->property("visible").toBool(); }, 2000));

    CHECK(controller.tabMenuPicks == std::vector { expected });
    CHECK(showingAtPick == std::vector { false });

    SECTION("a menu dismissed with no pick announces nothing")
    {
        QSignalSpy const pickedSpy(menu, SIGNAL(picked(int)));
        std::ignore = openTabMenu(*host, controller, 1);
        REQUIRE(QMetaObject::invokeMethod(menu, "dismiss"));
        REQUIRE(QTest::qWaitFor([&] { return !menu->property("visible").toBool(); }, 2000));

        CHECK(pickedSpy.count() == 0);
        CHECK(controller.tabMenuPicks.size() == 1);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("The tab color flyout opens inside the window, wherever the tab strip sits (offscreen)",
          "[contour][gui][qml]")
{
    // The picker is MODAL: one that opens outside the window is not merely misplaced, it is invisible AND
    // still eats every click — the user presses SetTabColor, sees nothing, and cannot color the tab at all
    // until they guess at Escape. So the contract is not "below the tab", it is "on screen"; below the tab
    // is only how it is honored when there is room there.
    //
    // The tab is hosted at the window's edge here, which is exactly what `tab_bar_position: Bottom` does
    // (Main.qml anchors the TitleBar at `parent.height - titleBar.height`) — and what the tests above, all
    // of which host the tab at 0,0 in a big window, structurally cannot see.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const stripAt = GENERATE(TabStripAt::Top, TabStripAt::Bottom);
    // Narrow enough that a flyout hanging off the right-hand tab would overflow the right edge.
    auto const host = makeTabItemHost(engine, controller, stripAt, /* windowWidth: */ 420);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = qobject_cast<QQuickItem*>(host->property("tabItem").value<QObject*>());
    REQUIRE(tab != nullptr);

    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);

    controller.beginActiveTabColorPick(); // what the SetTabColor action does
    QCoreApplication::processEvents();
    REQUIRE(flyout->property("opened").toBool());

    auto const windowRect = QRectF(0, 0, window->width(), window->height());
    auto const flyoutRect = flyoutWindowRect(flyout);
    INFO("strip at " << (stripAt == TabStripAt::Bottom ? "bottom" : "top") << ", flyout at " << flyoutRect.x()
                     << "," << flyoutRect.y() << " " << flyoutRect.width() << "x" << flyoutRect.height()
                     << " in a " << window->width() << "x" << window->height() << " window");

    REQUIRE(flyoutRect.height() > 0); // i.e. the popup materialized at all
    CHECK(windowRect.contains(flyoutRect));

    // Inside the window is not enough: the flyout is an in-scene popup, so the window CLIPS it, and a
    // flyout flush against an edge would have its drop shadow cut away entirely. Assert the gutter the
    // style asks for, which is the only thing that can catch "the shadow is drawn but nobody sees it".
    //
    // On one axis it is unavoidable. The flyout hangs off a tab that can sit at the very top or the
    // very bottom of the window (tab_bar_position), so the side facing the strip is against the tab by
    // construction -- hence the containment below is the horizontal gutter plus the side away from it.
    auto const* provider = engine.rootContext()
                               ->contextProperty(QStringLiteral("chromeStyle"))
                               .value<contour::window::UiStyleProvider*>();
    REQUIRE(provider != nullptr);
    auto const margin = provider->shadowMargin();
    CHECK(flyoutRect.left() >= windowRect.left() + margin);
    CHECK(flyoutRect.right() <= windowRect.right() - margin);
    if (stripAt == TabStripAt::Bottom)
        CHECK(flyoutRect.top() >= windowRect.top() + margin);
    else
        CHECK(flyoutRect.bottom() <= windowRect.bottom() - margin);

    // And it still hugs the tab it belongs to: above it when it cannot go below, rather than being parked
    // in some arbitrary corner that happens to be on screen.
    auto const tabRect = QRectF(tab->mapToScene(QPointF(0, 0)), QSizeF(tab->width(), tab->height()));
    if (stripAt == TabStripAt::Bottom)
        CHECK(flyoutRect.bottom() <= tabRect.top());
    else
        CHECK(flyoutRect.top() >= tabRect.bottom());

    // Modal popup still up at window teardown => Qt's overlay/grab machinery leaks, and LeakSanitizer
    // fails the run on it (see the keyboard test below).
    QMetaObject::invokeMethod(flyout, "close");
    QCoreApplication::processEvents();

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Tab color flyout picks a swatch with the cursor keys and with h/j/k/l (offscreen)",
          "[contour][gui][qml][keyboard]")
{
    // The keyboard half of the picker: a SetTabColor bound to a key is useless if the flyout it opens
    // can only be finished with the mouse. Real key events go into the real window (QTest::keyClick ->
    // QWindowSystemInterface), so this exercises the same focus/grab path the user's keystrokes take —
    // an offscreen platform still delivers keys, it just draws nowhere.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QObject*>();
    REQUIRE(tab != nullptr);

    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);

    auto const palette = controller.tabColorPalette();
    REQUIRE(palette.size() == 16); // 8 columns x 2 rows: enough to move in both axes

    // The grid materializes with the flyout, so every section opens first and looks it up after. Each
    // opens exactly ONCE: a picker that is closed and re-opened within one window leaks Qt-internal
    // overlay state, which LeakSanitizer (rightly) reports and which is not this test's subject.
    QQuickItem* grid = nullptr;
    auto const openPicker = [&] {
        controller.beginActiveTabColorPick(); // what the SetTabColor action does
        QCoreApplication::processEvents();
        REQUIRE(flyout->property("opened").toBool());
        grid = flyout->findChild<QQuickItem*>(QStringLiteral("tabColorGrid"));
        REQUIRE(grid != nullptr);
    };

    auto const press = [&](Qt::Key key) {
        QTest::keyClick(window, key);
        QCoreApplication::processEvents();
        return grid->property("currentIndex").toInt();
    };

    SECTION("opening arms the keyboard: the grid holds the focus, on the first swatch")
    {
        openPicker();

        // The grid, not the hex field, holds the focus — so the first keystroke navigates rather than
        // being swallowed — and it starts on the first swatch, this tab wearing no color of its own.
        CHECK(grid->hasActiveFocus());
        CHECK(grid->property("currentIndex").toInt() == 0);
    }

    SECTION("cursor keys and vim motions move the same cursor, and stop at the edges")
    {
        openPicker();

        CHECK(press(Qt::Key_Right) == 1);
        CHECK(press(Qt::Key_L) == 2); // the vim motion is an alias, not a second cursor
        CHECK(press(Qt::Key_Down) == 10);
        CHECK(press(Qt::Key_J) == 10); // already on the last row: nothing below to move to
        CHECK(press(Qt::Key_K) == 2);
        CHECK(press(Qt::Key_H) == 1);
        CHECK(press(Qt::Key_Left) == 0);

        // Clamped, not wrapped: h at the left edge must not teleport the cursor to the far end of the
        // palette, which is where an off-by-one in either direction would land it.
        CHECK(press(Qt::Key_Left) == 0);
        CHECK(press(Qt::Key_H) == 0);
        CHECK(press(Qt::Key_Up) == 0);

        CHECK(controller.setTabColorCalls == 0); // navigating alone colors nothing
    }

    SECTION("horizontal motion stays in its row rather than wrapping into the neighbouring one")
    {
        openPicker();

        // The edge above (index 0) is the ONE place a flat ±1 step over the model happens to clamp on its
        // own, so it proves nothing about the rows in between. GridView's own moveCurrentIndexLeft/Right
        // step ±1 through the FLAT model index: `h` on the first swatch of the second row lands on the LAST
        // swatch of the first — a color from a row the user never navigated to, one Enter from being
        // applied to their tab.
        REQUIRE(press(Qt::Key_Down) == 8); // first swatch of the second row
        CHECK(press(Qt::Key_H) == 8);      // nothing to its left: stay
        CHECK(press(Qt::Key_Left) == 8);

        // And the mirror image, at the right edge of the first row.
        REQUIRE(press(Qt::Key_Up) == 0);
        for ([[maybe_unused]] auto const step: std::views::iota(0, 7))
            press(Qt::Key_L);
        REQUIRE(grid->property("currentIndex").toInt() == 7); // last swatch of the first row
        CHECK(press(Qt::Key_L) == 7);                         // nothing to its right: stay
        CHECK(press(Qt::Key_Right) == 7);
    }

    SECTION("a modifier chord is not a motion")
    {
        openPicker();

        REQUIRE(press(Qt::Key_Right) == 1);

        // Qt reports the same event.key for Ctrl+L as for a bare `l`. Reading the key alone, the picker
        // would answer a user's reflexive Ctrl+L (clear screen) or Shift+J by moving the keyboard cursor —
        // and would swallow the chord — leaving the next Enter to apply a swatch they never aimed at.
        auto const chord = [&](Qt::Key key, Qt::KeyboardModifier modifier) {
            QTest::keyClick(window, key, modifier);
            QCoreApplication::processEvents();
            return grid->property("currentIndex").toInt();
        };

        CHECK(chord(Qt::Key_L, Qt::ControlModifier) == 1);
        CHECK(chord(Qt::Key_H, Qt::ControlModifier) == 1);
        CHECK(chord(Qt::Key_J, Qt::ShiftModifier) == 1);
        CHECK(chord(Qt::Key_Down, Qt::AltModifier) == 1);
        CHECK(chord(Qt::Key_Right, Qt::MetaModifier) == 1);

        // ... while the bare key still moves, i.e. the mask rejects the chord, not the key.
        CHECK(press(Qt::Key_L) == 2);
    }

    SECTION("Enter applies the swatch the cursor navigated to, and closes")
    {
        openPicker();

        REQUIRE(press(Qt::Key_L) == 1);
        REQUIRE(press(Qt::Key_J) == 9);

        QTest::keyClick(window, Qt::Key_Return);
        QCoreApplication::processEvents();

        CHECK(controller.setTabColorCalls == 1);
        CHECK(controller.lastTabColorIndex == 0); // this tab's row
        CHECK(controller.lastTabColor == palette[9].value<QColor>());
        CHECK_FALSE(flyout->property("opened").toBool());
    }

    SECTION("Space applies too — the other conventional 'activate what is focused' key")
    {
        openPicker();

        REQUIRE(press(Qt::Key_Right) == 1);

        QTest::keyClick(window, Qt::Key_Space);
        QCoreApplication::processEvents();

        CHECK(controller.setTabColorCalls == 1);
        CHECK(controller.lastTabColor == palette[1].value<QColor>());
        CHECK_FALSE(flyout->property("opened").toBool());
    }

    SECTION("Escape dismisses the flyout without coloring anything")
    {
        openPicker();

        REQUIRE(press(Qt::Key_Right) == 1);

        QTest::keyClick(window, Qt::Key_Escape);
        QCoreApplication::processEvents();

        CHECK_FALSE(flyout->property("opened").toBool());
        CHECK(controller.setTabColorCalls == 0);
    }

    SECTION("the cursor starts on the color the tab already wears")
    {
        // Otherwise re-coloring a tab always begins from the far corner of the palette, and the keyboard
        // user has to count their way back to the color they are changing.
        //
        // Set on the flyout, not on the tab: coloring a WINDOWED tab re-grabs its drag pixmap
        // (TabItem.onTabColorChanged -> grabToImage), an async grab that the offscreen platform never
        // renders and so never completes — its pending result then shows up as a leak. The binding that
        // feeds this property from the tab's color is pinned by the colored-TabItem test above.
        flyout->setProperty("currentColor", palette[11]);
        openPicker();

        CHECK(grid->property("currentIndex").toInt() == 11);
        CHECK(press(Qt::Key_K) == 3); // ... and it moves on from there
    }

    // Teardown, run after every section: destroying the window while a MODAL popup is still up leaks Qt's
    // overlay/grab machinery, and LeakSanitizer duly fails the run on it. The sections that pick a color
    // or press Escape already closed the flyout; the ones that only navigate have not.
    QMetaObject::invokeMethod(flyout, "close");
    QCoreApplication::processEvents();
    CHECK_FALSE(flyout->property("opened").toBool());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The tab color flyout's Reset Color button drops the tab's color (offscreen)",
          "[contour][gui][qml]")
{
    // The ONE path in the GUI that clears a user's tab color, so that whatever color the application gave
    // the tab (DECAC) resurfaces. Every sibling path through this flyout — a clicked swatch, Enter on the
    // keyboard cursor, the hex field — is pinned by a setTabColorCalls assertion; this one had a call
    // counter on the mock that nothing ever read, so a Reset that quietly stopped resetting (or stopped
    // closing, or reset the wrong tab) would have shipped with the suite green.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* tab = host->property("tabItem").value<QObject*>();
    REQUIRE(tab != nullptr);
    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);

    controller.beginActiveTabColorPick();
    QCoreApplication::processEvents();
    REQUIRE(flyout->property("opened").toBool());

    // Through the Button the user actually clicks, not the function behind it: the wiring between the two
    // is half of what can break here.
    auto* resetButton = flyout->findChild<QQuickItem*>(QStringLiteral("tabColorResetButton"));
    REQUIRE(resetButton != nullptr);
    QMetaObject::invokeMethod(resetButton, "clicked");
    QCoreApplication::processEvents();

    CHECK(controller.resetTabColorCalls == 1);
    CHECK(controller.lastTabColorIndex == 0); // this tab's row, not some other tab's
    CHECK(controller.setTabColorCalls == 0);  // a reset is not a set
    CHECK_FALSE(flyout->property("opened").toBool());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("The tab color flyout forgets an abandoned hex value between opens (offscreen)",
          "[contour][gui][qml]")
{
    // The picker is re-opened by a keystroke now, so what it carries over from the last visit matters.
    // A hex value typed and then abandoned with Escape must not still be sitting in the field on the next
    // open: its live preview swatch is enabled whenever the field parses, so one click on it — or one
    // Tab+Enter — would color the tab with the value from the session the user cancelled.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;

    auto const host = makeTabItemHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QObject*>();
    REQUIRE(tab != nullptr);
    auto* flyout = tab->findChild<QObject*>(QStringLiteral("tabColorFlyout"));
    REQUIRE(flyout != nullptr);

    controller.beginActiveTabColorPick();
    QCoreApplication::processEvents();
    REQUIRE(flyout->property("opened").toBool());

    auto* hexField = flyout->findChild<QQuickItem*>(QStringLiteral("customColorField"));
    REQUIRE(hexField != nullptr);
    hexField->setProperty("text", QStringLiteral("00FF00"));
    REQUIRE(hexField->property("acceptableInput").toBool()); // i.e. it WOULD be applicable

    // Abandon it, the way the flyout documents: Escape dismisses.
    QTest::keyClick(window, Qt::Key_Escape);
    QCoreApplication::processEvents();
    REQUIRE_FALSE(flyout->property("opened").toBool());
    REQUIRE(controller.setTabColorCalls == 0);

    controller.beginActiveTabColorPick(); // the user comes back
    QCoreApplication::processEvents();
    REQUIRE(flyout->property("opened").toBool());

    CHECK(hexField->property("text").toString().isEmpty());
    CHECK_FALSE(hexField->property("acceptableInput").toBool()); // so the preview cannot be applied

    QMetaObject::invokeMethod(flyout, "close"); // a modal popup left up at teardown leaks Qt's overlay
    QCoreApplication::processEvents();

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A zoomed TabItem shows the zoom badge and gives it back its width when unzoomed",
          "[contour][gui][qml][zoom]")
{
    // A zoomed tab renders only one of its panes, so it looks exactly like a genuinely single-pane
    // tab. The badge is the only thing telling the two apart, so assert it appears — and that it
    // costs the label nothing when absent (an invisible item still has geometry to anchors, so a
    // fixed-width badge would silently shrink every tab's title).
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 0);
    initial.insert("tabTitle", QStringLiteral("vim"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", QColor(Qt::transparent));
    initial.insert("tabActive", true);
    initial.insert("tabPaneCount", 2);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);

    auto* badge = tab->findChild<QQuickItem*>(QStringLiteral("zoomBadge"));
    REQUIRE(badge != nullptr);
    auto* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
    REQUIRE(label != nullptr);

    // Not zoomed: hidden, zero-width, and — the part a width check alone misses — costing the label
    // NOTHING. The label anchors to the badge's left edge, so a leftover margin would elide the title
    // early on every tab in the strip, badge or no badge. Pin the label's right edge as the oracle.
    CHECK_FALSE(badge->isVisible());
    CHECK(badge->width() == 0.0);
    auto const unzoomedLabelRight = label->x() + label->width();

    tab->setProperty("tabZoomed", true);
    CHECK(badge->isVisible());
    CHECK(badge->width() > 0.0);
    // Zoomed, the badge does take its room from the label — that is the point of it.
    CHECK(label->x() + label->width() < unzoomedLabelRight);

    tab->setProperty("tabZoomed", false);
    CHECK_FALSE(badge->isVisible());
    CHECK(badge->width() == 0.0);
    CHECK(label->x() + label->width() == unzoomedLabelRight);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A TabItem shows its badge and gives it back its width when there is none",
          "[contour][gui][qml][tabbadge]")
{
    // The badge sits between the label and the zoom badge, so like the zoom badge it must cost the label
    // nothing while nothing competes for the slot.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 0);
    initial.insert("tabTitle", QStringLiteral("cargo"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", QColor(Qt::transparent));
    initial.insert("tabActive", false);
    initial.insert("tabPaneCount", 1);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);

    auto* badge = tab->findChild<QQuickItem*>(QStringLiteral("tabBadgeSlot"));
    REQUIRE(badge != nullptr);
    auto* glyph = tab->findChild<QQuickItem*>(QStringLiteral("tabBadgeGlyph"));
    REQUIRE(glyph != nullptr);
    auto* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
    REQUIRE(label != nullptr);

    CHECK_FALSE(badge->isVisible());
    CHECK(badge->width() == 0.0);
    auto const bareLabelRight = label->x() + label->width();

    using contour::window::TabBadgeKind;
    tab->setProperty("tabBadge", static_cast<int>(TabBadgeKind::Blocked));
    tab->setProperty("tabBadgeText", QStringLiteral("Approve?"));
    CHECK(badge->isVisible());
    CHECK(badge->width() > 0.0);
    CHECK(glyph->property("text").toString() == QStringLiteral("!"));
    CHECK(label->x() + label->width() < bareLabelRight);

    // Working (3): spins without a progress, stands at the angle of one.
    tab->setProperty("tabBadge", static_cast<int>(TabBadgeKind::Working));
    CHECK(badge->property("spinning").toBool());
    tab->setProperty("tabBadgeProgress", 50);
    CHECK_FALSE(badge->property("spinning").toBool());
    CHECK(glyph->rotation() == 180.0);

    // A kind this build does not know draws nothing.
    tab->setProperty("tabBadge", static_cast<int>(contour::window::TabBadgeKindCount + 3));
    CHECK_FALSE(badge->isVisible());

    // The rest draw their glyph from the look table.
    tab->setProperty("tabBadge", static_cast<int>(TabBadgeKind::Output));
    CHECK(glyph->property("text").toString() == QStringLiteral("\u2022"));
    tab->setProperty("tabBadge", static_cast<int>(TabBadgeKind::Error));
    CHECK(glyph->property("text").toString() == QStringLiteral("\u2715"));

    tab->setProperty("tabBadge", static_cast<int>(TabBadgeKind::None));
    CHECK_FALSE(badge->isVisible());
    CHECK(badge->width() == 0.0);
    CHECK(label->x() + label->width() == bareLabelRight);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A working TabItem's spinner runs only while its window is on screen (offscreen)",
          "[contour][gui][qml][tabbadge]")
{
    // A program may work for hours in a minimized window; an infinite animation there is CPU for nothing.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTabItemHost(engine, controller);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto* spinner = tab->findChild<QObject*>(QStringLiteral("tabBadgeSpinner"));
    REQUIRE(spinner != nullptr);

    // Working, with no progress: it spins.
    tab->setProperty("tabBadge", static_cast<int>(contour::window::TabBadgeKind::Working));
    QCoreApplication::processEvents();
    CHECK(spinner->property("running").toBool());

    window->showMinimized();
    QCoreApplication::processEvents();
    CHECK_FALSE(spinner->property("running").toBool());

    window->showNormal();
    QCoreApplication::processEvents();
    CHECK(spinner->property("running").toBool());

    window->hide();
    QCoreApplication::processEvents();
    CHECK_FALSE(spinner->property("running").toBool());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A working TabItem's spinner stands still under Reduce Motion (offscreen)",
          "[contour][gui][qml][tabbadge]")
{
    // Reduce Motion turns every motion into a cross-fade; an endless spin has none to become.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto setup = contour::test::defaultChromeSetup();
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;
    auto const host = makeTabItemHost(engine, controller, TabStripAt::Top, 800, setup);
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto* spinner = tab->findChild<QObject*>(QStringLiteral("tabBadgeSpinner"));
    REQUIRE(spinner != nullptr);

    tab->setProperty("tabBadge", static_cast<int>(contour::window::TabBadgeKind::Working));
    QCoreApplication::processEvents();
    REQUIRE(window->isVisible());
    CHECK_FALSE(spinner->property("running").toBool());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A palette change recolours a TabItem's badge (offscreen)", "[contour][gui][qml][tabbadge]")
{
    // The ink is a status colour, which follows light and dark: the binding over the invokable must
    // re-evaluate when the palette does.
    contour::test::QmlMessageCapture const warnings;
    auto appearance =
        std::optional<contour::test::ScopedChromeAppearance> { std::in_place,
                                                               vtbackend::ColorPreference::Light };
    QQmlEngine engine;
    MockTabController controller;
    auto const host = makeTabItemHost(engine, controller);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto* glyph = tab->findChild<QQuickItem*>(QStringLiteral("tabBadgeGlyph"));
    REQUIRE(glyph != nullptr);

    tab->setProperty("tabBadge", static_cast<int>(contour::window::TabBadgeKind::Blocked));
    QCoreApplication::processEvents();
    auto const onLight = glyph->property("color").value<QColor>();

    appearance.reset();
    appearance.emplace(vtbackend::ColorPreference::Dark);
    QCoreApplication::processEvents();
    auto const onDark = glyph->property("color").value<QColor>();

    CHECK(onLight != onDark);
    CHECK(onDark != QColor(Qt::transparent));
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("the badge slot draws each kind from the look table", "[contour][gui][qml][tabbadge]")
{
    // The Pill shape draws the look's mark, the Flat shape its glyph; both in the status ink, both named.
    using contour::window::TabBadgeKind;
    using contour::window::TabBadgeKindCount;
    auto const rows =
        std::array { contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::ContourLinux };
    for (auto const row: rows)
    {
        INFO("row: " << contour::config::chromeStyleName(row));
        contour::test::QmlMessageCapture const warnings;
        QQmlEngine engine;
        auto setup = contour::test::defaultChromeSetup();
        setup.row = row;
        contour::test::installChromeStyle(engine, setup);
        auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        REQUIRE(style != nullptr);
        auto const pill = contour::config::ChromeTokenRows[static_cast<size_t>(row)].layout.tabShape
                          == contour::config::TabShape::Pill;

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabBadgeSlot.qml")));
        INFO("errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());

        for (auto const index: std::views::iota(std::size_t { 1 }, TabBadgeKindCount))
        {
            auto const kind = static_cast<TabBadgeKind>(index);
            auto const& look = contour::window::tabBadgeLook(kind);
            INFO("kind: " << index);
            auto slot = std::unique_ptr<QObject>(component.createWithInitialProperties(
                { { QStringLiteral("kind"), static_cast<int>(index) } }));
            REQUIRE(slot != nullptr);
            auto* item = qobject_cast<QQuickItem*>(slot.get());
            REQUIRE(item != nullptr);
            CHECK(item->isVisible());

            auto* mark = slot->findChild<QQuickItem*>(QStringLiteral("tabBadgeMark"));
            auto* glyph = slot->findChild<QQuickItem*>(QStringLiteral("tabBadgeGlyph"));
            REQUIRE(mark != nullptr);
            REQUIRE(glyph != nullptr);
            CHECK(mark->isVisible() == pill);
            CHECK(glyph->isVisible() == !pill);
            if (pill)
                CHECK(mark->property("mark").toString()
                      == QString::fromUtf8(contour::window::badgeMarkName(look.mark)));
            else
                CHECK(glyph->property("text").toString() == QString::fromUtf8(look.glyph));

            auto ink = QColor {};
            REQUIRE(QMetaObject::invokeMethod(
                style, "statusInk", Q_RETURN_ARG(QColor, ink), Q_ARG(int, static_cast<int>(look.ink))));
            CHECK(slot->property("ink").value<QColor>() == ink);
            if (!pill)
                CHECK(glyph->property("color").value<QColor>() == ink);

            auto name = QQmlExpression(qmlContext(item), item, QStringLiteral("Accessible.name"));
            CHECK(name.evaluate().toString() == QString::fromUtf8(look.accessibleName));
        }

        // Working at 40 % stands still as a dial of 144 degrees.
        auto working = std::unique_ptr<QObject>(component.createWithInitialProperties(
            { { QStringLiteral("kind"), static_cast<int>(TabBadgeKind::Working) },
              { QStringLiteral("progress"), 40 } }));
        REQUIRE(working != nullptr);
        auto const* dial = working->findChild<QObject*>(QStringLiteral("tabBadgeMark"));
        REQUIRE(dial != nullptr);
        CHECK(dial->property("sweep").toReal() == Catch::Approx(144.0));
        CHECK_FALSE(working->property("spinning").toBool());

        // None, and a kind the table does not know, draw nothing and take no room.
        for (auto const unknown: { 0, static_cast<int>(TabBadgeKindCount) + 3 })
        {
            auto none = std::unique_ptr<QObject>(
                component.createWithInitialProperties({ { QStringLiteral("kind"), unknown } }));
            REQUIRE(none != nullptr);
            auto const* noneItem = qobject_cast<QQuickItem*>(none.get());
            REQUIRE(noneItem != nullptr);
            CHECK_FALSE(noneItem->isVisible());
            CHECK(noneItem->implicitWidth() == 0.0);
        }

        CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    }
}

TEST_CASE("the progress pulse stops under Reduce Motion (offscreen)", "[contour][gui][qml][tabbadge]")
{
    // An indeterminate progress pulses; under Reduce Motion it is shown whole and still.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    auto setup = contour::test::defaultChromeSetup();
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;
    auto const host = makeTabItemHost(engine, controller, TabStripAt::Top, 800, setup);
    auto* tab = host->property("tabItem").value<QQuickItem*>();
    REQUIRE(tab != nullptr);
    auto const* fill = tab->findChild<QQuickItem*>(QStringLiteral("progressFill"));
    REQUIRE(fill != nullptr);
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const pulse = style->property("motion").value<QObject*>()->property("pulse").toInt();

    tab->setProperty("tabProgressState", 3); // vtbackend::ProgressState::Indeterminate
    QTest::qWait(pulse);
    CHECK(fill->isVisible());
    CHECK(fill->opacity() == 1.0);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A TabItem's tooltip shows a program's status text literally, never as markup",
          "[contour][gui][qml][tabbadge]")
{
    // The message is what a program sent. Rendered as AutoText, `<font>` would colour it and `<img>` would
    // have the engine load whatever it names.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 0);
    initial.insert("tabTitle", QStringLiteral("cargo"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", QColor(Qt::transparent));
    initial.insert("tabActive", false);
    initial.insert("tabPaneCount", 1);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    // The text reaches the tooltip only beside a badge.
    initial.insert("tabBadge", static_cast<int>(contour::window::TabBadgeKind::Blocked));
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);

    auto* toolTip = tab->findChild<QObject*>(QStringLiteral("tabHoverToolTip"));
    REQUIRE(toolTip != nullptr);
    auto* content = toolTip->findChild<QQuickItem*>(QStringLiteral("kitToolTipLabel"));
    REQUIRE(content != nullptr);

    auto const hostile = std::array {
        QStringLiteral("<font color=red>x</font>"),
        QStringLiteral("<img src=\"file:///etc/passwd\">"),
        QStringLiteral("<b>bold</b> &amp; plain"),
    };
    for (auto const& text: hostile)
    {
        tab->setProperty("tabBadgeText", text);
        // What HoverHandler does when the pointer arrives.
        QVariant composed;
        REQUIRE(QMetaObject::invokeMethod(tab.get(), "buildHoverTooltip", Q_RETURN_ARG(QVariant, composed)));
        tab->setProperty("hoverTooltip", composed);
        CHECK(toolTip->property("text").toString().startsWith(text));
        CHECK(content->property("text").toString() == text);
        // 0 = Text.PlainText: the string is drawn as the characters it holds.
        CHECK(content->property("textFormat").toInt() == 0);
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("A TabItem paints its OSC 9;4 progress bar only while an operation is in flight",
          "[contour][gui][qml][progress]")
{
    // The tab-strip half of OSC 9;4. The bar is anchored along the tab's bottom edge rather than
    // beside the label, so — unlike the zoom badge above — it must cost the title NO width in any
    // state; that is what lets it appear and vanish without reflowing the strip.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabItem.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&controller)));
    initial.insert("tabIndex", 0);
    initial.insert("tabTitle", QStringLiteral("build"));
    initial.insert("tabRawTitle", QString {});
    initial.insert("tabColor", QColor(Qt::transparent));
    initial.insert("tabActive", true);
    initial.insert("tabPaneCount", 1);
    initial.insert("tabZoomed", false);
    initial.insert("tabProgressState", 0);
    initial.insert("tabProgressPercentage", 0);
    std::unique_ptr<QObject> tab(component.createWithInitialProperties(initial));
    REQUIRE(tab != nullptr);

    auto* track = tab->findChild<QQuickItem*>(QStringLiteral("progressTrack"));
    REQUIRE(track != nullptr);
    auto* fill = tab->findChild<QQuickItem*>(QStringLiteral("progressFill"));
    REQUIRE(fill != nullptr);
    auto* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
    REQUIRE(label != nullptr);
    auto const labelRight = label->x() + label->width();

    // Inactive (state 0): nothing in flight, nothing painted.
    CHECK_FALSE(track->isVisible());

    // Normal (1) at 40%: shown, and the fill is 40% of the track.
    tab->setProperty("tabProgressState", 1);
    tab->setProperty("tabProgressPercentage", 40);
    CHECK(track->isVisible());
    CHECK(track->width() > 0.0);
    CHECK(fill->width() == Catch::Approx(track->width() * 0.4));

    // Indeterminate (3): no meaningful position, so the bar fills whole and pulses instead.
    tab->setProperty("tabProgressState", 3);
    CHECK(track->isVisible());
    CHECK(fill->width() == Catch::Approx(track->width()));

    // Error (2) keeps the percentage it was given, and recolors rather than resizing.
    tab->setProperty("tabProgressState", 2);
    tab->setProperty("tabProgressPercentage", 90);
    CHECK(fill->width() == Catch::Approx(track->width() * 0.9));

    // Back to inactive: withdrawn again.
    tab->setProperty("tabProgressState", 0);
    CHECK_FALSE(track->isVisible());

    // Through all of it the label never moved: the bar is under it, not beside it.
    CHECK(label->x() + label->width() == labelRight);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("window controls tolerate a controller that is absent, late or gone (offscreen)",
          "[contour][gui][qml]")
{
    // Regression: closing a window drops Main.qml's `win` to null while the title bar's WindowControls
    // are still alive, and the order in which QML resets the reference and destroys the children is not
    // guaranteed. Live that showed as `Cannot read property 'nativeMaximizeHovered' of null` and
    // `Cannot call method 'clearCaptionButtonRect' of null` (from Component.onDestruction).
    contour::test::QmlMessageCapture const capture;

    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQuickWindow window;

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/WindowControls.qml")));
    REQUIRE(component.isReady());

    auto const makeControls = [&](QObject* controller, QString const& side) {
        QVariantMap initial;
        initial.insert("controller", QVariant::fromValue(controller));
        initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&window)));
        initial.insert("side", side);
        return std::unique_ptr<QObject>(component.createWithInitialProperties(initial));
    };
    auto const isQmlDiagnostic = [](QString const& m) {
        return m.contains(QStringLiteral("TypeError")) || m.contains(QStringLiteral("null"));
    };

    SECTION("the controller goes null, then the controls are destroyed")
    {
        MockTabController controller;
        for (auto const& side: { QStringLiteral("leading"), QStringLiteral("trailing") })
        {
            auto controls = makeControls(&controller, side);
            REQUIRE(controls != nullptr);
            QCoreApplication::processEvents();
            controls->setProperty("controller", QVariant::fromValue<QObject*>(nullptr));
            controller.setNativeMaximizeVisual(contour::platform::CaptionButtonVisual::Pressed);
            QCoreApplication::processEvents();
            controls.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    SECTION("the controls are created before their controller exists")
    {
        MockTabController controller;
        auto controls = makeControls(nullptr, QStringLiteral("trailing"));
        REQUIRE(controls != nullptr);
        QCoreApplication::processEvents();
        controls->setProperty("controller", QVariant::fromValue<QObject*>(&controller));
        controller.setNativeMaximizeVisual(contour::platform::CaptionButtonVisual::Hover);
        QCoreApplication::processEvents();
        controls.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    SECTION("the controller is destroyed under live controls")
    {
        auto controller = std::make_unique<MockTabController>();
        auto controls = makeControls(controller.get(), QStringLiteral("leading"));
        REQUIRE(controls != nullptr);
        QCoreApplication::processEvents();
        controller.reset();
        QCoreApplication::processEvents();
        controls.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    QCoreApplication::processEvents();
    CHECK(capture.count(isQmlDiagnostic) == 0);
}

TEST_CASE("GUI tab strip survives controller destruction without QML errors (offscreen)",
          "[contour][gui][qml]")
{
    // Regression: closing a window destroys the C++ WindowController (deleteLater in
    // removeWindowController) while the QML tree is still alive. QML then resets every reference to
    // the dead controller to null and re-evaluates dependent bindings once more before teardown —
    // any unguarded `controller.` binding raises a TypeError in the shutdown log (seen live:
    // TabStrip.qml "Cannot read property 'activeTabIndex' of null"). The message capture (chained to
    // the run-wide gate) turns such a TypeError into a test failure.
    contour::test::QmlMessageCapture const capture;

    QQmlEngine engine;
    auto controller = std::make_unique<MockTabController>();
    engine.rootContext()->setContextProperty("terminalSessions", controller.get());

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabStrip.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(controller.get()));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&engine)));
    std::unique_ptr<QObject> strip(component.createWithInitialProperties(initial));
    REQUIRE(strip != nullptr);
    QCoreApplication::processEvents();

    // Push the null through the strip's `controller` property the way the app's teardown does:
    // Main.qml's `win` var property nulls with notification when the C++ controller dies, and QML
    // propagates that null down the binding chain (TitleBar.controller -> TabStrip.controller),
    // re-evaluating every dependent binding against null. The push happens here while the model
    // object is still alive, mirroring the propagation-before-model-teardown window (and keeping the
    // repro independent of the ListView's own model-destruction reset, which detaches bindings).
    strip->setProperty("controller", QVariant::fromValue<QObject*>(nullptr));
    QCoreApplication::processEvents();
    controller.reset();
    QCoreApplication::processEvents();

    auto const isTypeError = [](QString const& m) {
        return m.contains(QStringLiteral("TypeError"));
    };
    CHECK(capture.count(isTypeError) == 0);

    // Tear the strip down BEFORE the engine and drain deferred deletions: the null-model push above
    // makes the ListView release its delegates via deleteLater(), and those queued deletions would
    // otherwise still be pending at process exit and get reported by LeakSanitizer.
    strip.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

TEST_CASE("SessionChrome instantiates and wires a (null) session without errors (offscreen)",
          "[contour][gui][qml]")
{
    // SessionChrome is the per-session chrome (scrollbar, bell, permission dialogs, notification/alert
    // wiring) shared by every pane (TerminalPane.qml). Merely loading it catches syntax errors;
    // instantiating it against a null session exercises the null-guarded scrollbar/dialog bindings and
    // the null-target Connections, which is exactly the transient state a split pane hits on teardown.
    QQmlEngine engine;
    MockTabController controller; // provides the `terminalSessions.multimediaReady` the bell Loader gates on
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    {
        INFO("SessionChrome.qml status: " << static_cast<int>(component.status()));
        INFO("SessionChrome.qml errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
    }

    // Instantiate with an explicit null session (the required property) — the bindings must resolve
    // without dereferencing it.
    QVariantMap initial;
    initial.insert("session", QVariant::fromValue(static_cast<QObject*>(nullptr)));
    std::unique_ptr<QObject> chrome(component.createWithInitialProperties(initial));
    REQUIRE(chrome != nullptr);
    {
        INFO("SessionChrome creation errors: " << component.errorString().toStdString());
        REQUIRE_FALSE(component.isError());
    }
    QCoreApplication::processEvents();

    // Rebinding the session must be error-free in both directions: null -> object retargets the
    // declarative Connections (ignoreUnknownSignals tolerates a mock lacking some session signals),
    // object -> null is the pane-teardown path. The imperative wireSession() this replaced accumulated
    // stale cross-session connections on every rebind (wrong-session permission grants, N-fold bells).
    MockSession session;
    chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&session)));
    QCoreApplication::processEvents();
    chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(nullptr)));
    QCoreApplication::processEvents();
    CHECK(chrome != nullptr);
}

TEST_CASE("StatusLineItemDialog edits which extension an Extension item shows (offscreen)",
          "[contour][gui][qml][extensions]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import QtQuick.Controls
import Contour.Ui
ApplicationWindow {
    width: 480
    height: 480
    property var appliedItem: null
    property alias dialog: dialog
    StatusLineItemDialog {
        id: dialog
        onApplied: (segmentIndex, itemIndex, item) => appliedItem = item
    }
})",
                      QUrl());
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> window(component.create());
    REQUIRE(window != nullptr);
    auto* const dialog = window->property("dialog").value<QObject*>();
    REQUIRE(dialog != nullptr);

    auto const item = QVariantMap { { "type", "Extension" }, { "label", "Extension" }, { "name", "git" } };
    REQUIRE(QMetaObject::invokeMethod(
        dialog, "edit", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 1), Q_ARG(QVariant, item)));
    QCoreApplication::processEvents();
    auto* const field = dialog->findChild<QObject*>(QStringLiteral("indicatorItemTextField"));
    REQUIRE(field != nullptr);
    CHECK(field->property("text").toString() == "git");

    field->setProperty("text", QStringLiteral("clock"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    auto const applied = window->property("appliedItem").toMap();
    CHECK(applied.value("name").toString() == "clock");
    CHECK(applied.value("displayName").toString() == "clock");
}

TEST_CASE("StatusLineItemDialog keeps an item's roles and payload (offscreen)",
          "[contour][gui][qml][statusline]")
{
    // Apply used to rebuild the item from the handful of fields the dialog edits, so a payload it has no
    // control for -- Hide=, Chip, MaxWidth -- was deleted by any edit, and a role read as no colour at all.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import QtQuick.Controls
import Contour.Ui
ApplicationWindow {
    width: 480
    height: 640
    property var appliedItem: null
    property alias dialog: dialog
    StatusLineItemDialog {
        id: dialog
        colorRoles: ["accent", "danger"]
        onApplied: (segmentIndex, itemIndex, item) => appliedItem = item
    }
})",
                      QUrl());
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> window(component.create());
    REQUIRE(window != nullptr);
    auto* const dialog = window->property("dialog").value<QObject*>();
    REQUIRE(dialog != nullptr);

    auto const item = QVariantMap { { "type", "InputMode" }, { "label", "Input mode" }, { "hide", "Insert" },
                                    { "chip", true },        { "maxWidth", 7 },         { "hasColor", true },
                                    { "color", "accent" } };
    REQUIRE(QMetaObject::invokeMethod(
        dialog, "edit", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 1), Q_ARG(QVariant, item)));
    QCoreApplication::processEvents();

    auto* const field = dialog->findChild<QObject*>(QStringLiteral("foregroundColorField"));
    REQUIRE(field != nullptr);
    CHECK(field->property("text").toString() == "accent");

    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    auto applied = window->property("appliedItem").toMap();
    CHECK(applied.value("hide").toString() == "Insert");
    CHECK(applied.value("chip").toBool());
    CHECK(applied.value("maxWidth").toInt() == 7);
    CHECK(applied.value("hasColor").toBool());
    CHECK(applied.value("color").toString() == "accent");

    // A role typed into the field is taken like a hex colour.
    REQUIRE(QMetaObject::invokeMethod(
        dialog, "edit", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 1), Q_ARG(QVariant, item)));
    QCoreApplication::processEvents();
    field->setProperty("text", QStringLiteral("danger"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    applied = window->property("appliedItem").toMap();
    CHECK(applied.value("color").toString() == "danger");
    CHECK(applied.value("hide").toString() == "Insert");

    // In any case, as the template parser reads a role; kept as the role's own spelling.
    auto hexItem = item;
    hexItem["color"] = QStringLiteral("#102030");
    REQUIRE(QMetaObject::invokeMethod(dialog,
                                      "edit",
                                      Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, hexItem)));
    QCoreApplication::processEvents();
    field->setProperty("text", QStringLiteral("Accent"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    applied = window->property("appliedItem").toMap();
    CHECK(applied.value("hasColor").toBool());
    CHECK(applied.value("color").toString() == "accent");

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome scrollbar renders as a styled grabbable overlay (offscreen)", "[contour][gui][qml]")
{
    // Regression guard for the "barely visible / hard to grab" scrollbar. After the app-wide Fusion
    // style was pinned (for the tab strip/menus), the stock ScrollBar inherited Fusion's thin,
    // low-contrast, hover-only handle. SessionChrome now supplies an explicit contentItem/background and
    // a comfortable hit target, so the styling must survive: the bar is findable, wider than the thin
    // default, and owns its handle + track items rather than deferring to the style.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());

    MockSession session;
    QVariantMap initial;
    initial.insert("session", QVariant::fromValue(static_cast<QObject*>(&session)));
    std::unique_ptr<QObject> chrome(component.createWithInitialProperties(initial));
    REQUIRE(chrome != nullptr);
    QCoreApplication::processEvents();

    auto* bar = chrome->findChild<QQuickItem*>(QStringLiteral("verticalScrollBar"));
    REQUIRE(bar != nullptr);

    // A comfortable, easy-to-grab hit target rather than the thin Fusion default.
    CHECK(bar->property("implicitWidth").toReal() >= 12.0);

    // The bar is sized by its implicitWidth and nothing else. Asserting only implicitWidth (as this case
    // originally did) is blind to the real defect: an anchor pair can stretch the actual width while
    // implicitWidth stays a truthful 12. The rebind case below drives that transition.
    CHECK(bar->width() == Catch::Approx(bar->property("implicitWidth").toReal()));

    // Explicit, style-independent handle + track items (the fix), not the inherited defaults.
    CHECK(bar->property("contentItem").value<QQuickItem*>() != nullptr);
    CHECK(bar->property("background").value<QQuickItem*>() != nullptr);
}

TEST_CASE("SessionChrome scrollbar stays a thin edge overlay, whatever the session does (offscreen)",
          "[contour][gui][qml][scrollbar]")
{
    // THE regression guard: the bar used to span the WHOLE pane width.
    //
    // Placement was expressed as TWO conditional anchor bindings over one condition (anchors.right =
    // atRightEdge ? parent.right : undefined, plus the mirrored anchors.left). QML re-evaluates them one
    // at a time, and the null-session fallback anchored LEFT — so a null->Right rebind set `right` while
    // `left` was still bound. QQuickAnchors answers a left+right pair by stretching the item:
    // setItemWidth(parent.width) -> QQuickItem::setWidth() -> widthValid latches. Resetting `left` an
    // instant later does NOT restore the width, so implicitWidth never re-applied and the bar stayed
    // parent-wide at x == 0 forever. Placement is a single `x` binding now, so no anchor can size the bar
    // horizontally — and every section below asserts the width, which is what the styling case above
    // (implicitWidth only) was blind to: implicitWidth stayed a truthful 12 throughout the bug.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();

    SECTION("a null->session rebind (every split / tab switch) leaves it thin and right-aligned")
    {
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        QCoreApplication::processEvents();

        // Pre-fix this read width == 400 (the whole pane) and x == 0.
        CHECK(host.bar->width() == Catch::Approx(host.thin));
        CHECK(host.bar->x() == Catch::Approx(host.paneWidth - host.thin));
        CHECK(host.bar->height() == Catch::Approx(host.chrome->height())); // vertical anchors still span
        CHECK(QTest::qWaitFor([&] { return host.bar->isVisible(); }));

        // Rebinding back to null (pane teardown) must not stretch it either.
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(nullptr)));
        QCoreApplication::processEvents();
        CHECK(host.bar->width() == Catch::Approx(host.thin));
    }

    SECTION("the configured edge is honored, and a live profile flip only ever moves it")
    {
        // scrollbar.position is profile state, so a config reload can move the bar between edges at
        // runtime (TerminalSession::activateProfile re-announces it; the signal used to be declared and
        // never emitted, so the setting needed a restart). A flip is the second way into the old latch:
        // whichever direction set an anchor before the opposite one was reset stretched the bar. So both
        // directions are asserted, and each asserts the width, not just the position.
        session->setScrollbarRight(false); // position: Left
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        QCoreApplication::processEvents();
        CHECK(host.bar->x() == Catch::Approx(0.0));
        CHECK(host.bar->width() == Catch::Approx(host.thin));

        session->setScrollbarRight(true); // Left -> Right
        QCoreApplication::processEvents();
        CHECK(host.bar->x() == Catch::Approx(host.paneWidth - host.thin));
        CHECK(host.bar->width() == Catch::Approx(host.thin));

        session->setScrollbarRight(false); // Right -> Left
        QCoreApplication::processEvents();
        CHECK(host.bar->x() == Catch::Approx(0.0));
        CHECK(host.bar->width() == Catch::Approx(host.thin));
    }

    SECTION("hiding it live (position: Hidden, or hide_in_alt_screen on the alt screen) is honored")
    {
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
        QCoreApplication::processEvents();
        REQUIRE(QTest::qWaitFor([&] { return host.bar->isVisible(); }));

        session->setScrollbarVisible(false);
        QCoreApplication::processEvents();
        CHECK(QTest::qWaitFor([&] { return !host.bar->isVisible(); })); // once faded out
    }

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome scrollbar takes its extents and colours from the tokens (offscreen)",
          "[contour][gui][qml][scrollbar]")
{
    using contour::config::ChromeColorRole;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto const dark = QColor(0x1C, 0x1E, 0x24);
    auto session = std::make_unique<MockSession>(dark);
    session->setScrollbarVisible(true);
    session->setHistoryLineCount(1000);
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    // Faded in, over motion.standard, before a press can reach it.
    REQUIRE(QTest::qWaitFor([&] { return host.bar->opacity() == 1.0; }));

    // The track and the inset beside it, toward the pane's edge, so the hit area reaches the edge.
    CHECK(host.bar->width()
          == chromeToken(engine, "size", "scrollbar") + chromeToken(engine, "space", "scrollbarInset"));
    CHECK(host.bar->property("rightPadding").toReal() == chromeToken(engine, "space", "scrollbarInset"));
    CHECK(host.bar->property("leftPadding").toReal() == 0.0);
    auto* handle = host.bar->property("contentItem").value<QQuickItem*>();
    REQUIRE(handle != nullptr);
    CHECK(handle->width() == chromeToken(engine, "size", "scrollbar"));
    CHECK(handle->property("radius").toReal() == chromeToken(engine, "radii", "scrollbar"));
    auto const resting = colorOnPane(engine, ChromeColorRole::ScrollbarThumb, dark);
    CHECK(QTest::qWaitFor([&] { return handle->property("color").value<QColor>() == resting; }));

    // Engaged (pressed, as hovered), the thumb takes the active colour and the track shows.
    auto* window = qobject_cast<QQuickWindow*>(host.window.get());
    REQUIRE(window != nullptr);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, centreOf(*handle));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->property("pressed").toBool(); }));
    // The colours change over motion.fast.
    auto const active = colorOnPane(engine, ChromeColorRole::ScrollbarThumbActive, dark);
    CHECK(QTest::qWaitFor([&] { return handle->property("color").value<QColor>() == active; }));
    auto* track = host.bar->property("background").value<QQuickItem*>();
    REQUIRE(track != nullptr);
    auto const trackColor = colorOnPane(engine, ChromeColorRole::ScrollbarTrack, dark);
    CHECK(QTest::qWaitFor([&] { return track->property("color").value<QColor>() == trackColor; }));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, centreOf(*handle));
    REQUIRE(QTest::qWaitFor([&] { return !host.bar->property("pressed").toBool(); }));

    // Hovered, without a press, it engages too. The offscreen platform enables no hover effects of its own,
    // so the bar is asked to take hover as a desktop style would have it do.
    auto const away =
        QPoint(static_cast<int>(host.paneWidth / 2), static_cast<int>(host.chrome->height() / 2));
    QTest::mouseMove(window, away);
    REQUIRE(QTest::qWaitFor([&] { return handle->property("color").value<QColor>() == resting; }));
    host.bar->setProperty("hoverEnabled", true);
    QTest::mouseMove(window, centreOf(*handle));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->property("hovered").toBool(); }));
    CHECK(host.bar->property("engaged").toBool());
    CHECK(QTest::qWaitFor([&] { return handle->property("color").value<QColor>() == active; }));
    QTest::mouseMove(window, away);
    REQUIRE(QTest::qWaitFor([&] { return !host.bar->property("engaged").toBool(); }));

    // Over a light pane the thumb is dark.
    auto light = std::make_unique<MockSession>(QColor(0xFA, 0xFA, 0xF8));
    light->setScrollbarVisible(true);
    light->setHistoryLineCount(1000);
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(light.get())));
    QCoreApplication::processEvents();
    auto const lightThumb = colorOnPane(engine, ChromeColorRole::ScrollbarThumb, QColor(0xFA, 0xFA, 0xF8));
    CHECK(lightThumb.lightnessF() < 0.5);
    REQUIRE_FALSE(host.bar->property("engaged").toBool()); // the pointer rests away from the bar
    CHECK(QTest::qWaitFor([&] { return handle->property("color").value<QColor>() == lightThumb; }));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome: an overlay bar hidden at the bottom takes no presses (offscreen)",
          "[contour][gui][qml][scrollbar]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    session->setScrollbarVisible(false); // an overlay bar at the bottom of the scrollback
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    CHECK(QTest::qWaitFor([&] { return !host.bar->isVisible(); }));
    // What a press at the pane's right edge lands on is not the bar.
    auto const edge = QPointF(host.paneWidth - 1, host.chrome->height() / 2);
    CHECK(host.chrome->childAt(edge.x(), edge.y()) != host.bar);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome: the Terminal row draws the bar as a column of its thumb glyph (offscreen)",
          "[contour][gui][qml][scrollbar]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    session->setHistoryLineCount(30); // a tall handle: several cell rows
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const cellHeight = style->property("cellHeight").toReal();
    CHECK(host.bar->width() == Catch::Approx(style->property("cellWidth").toReal()));

    auto* handle = host.bar->property("contentItem").value<QQuickItem*>();
    REQUIRE(handle != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return handle->height() > cellHeight; }));
    auto glyphs = std::vector<QQuickItem*> {};
    auto const collect = [&](this auto const& self, QQuickItem const& item) -> void {
        for (auto* child: item.childItems())
        {
            if (child->objectName() == QStringLiteral("scrollbarThumbGlyph"))
                glyphs.push_back(child);
            self(*child);
        }
    };
    collect(*handle);
    auto const rows = static_cast<qsizetype>(std::floor(handle->height() / cellHeight));
    CHECK(std::cmp_equal(glyphs.size(), rows));
    for (auto const* glyph: glyphs)
        CHECK(glyph->property("text").toString() == QStringLiteral("┃"));
    CHECK(handle->property("color").value<QColor>().alpha() == 0);

    // The glyphs take the scheme's thumb ink, and its active ink while the thumb is pressed.
    auto* colors = style->property("colors").value<QObject*>();
    REQUIRE(colors != nullptr);
    auto const resting = colors->property("scrollbarThumb").value<QColor>();
    auto const pressed = colors->property("scrollbarThumbActive").value<QColor>();
    REQUIRE(resting != pressed);
    auto const inkOfAll = [&](QColor const& ink) {
        return std::ranges::all_of(
            glyphs, [&](QQuickItem const* glyph) { return glyph->property("color").value<QColor>() == ink; });
    };
    CHECK(QTest::qWaitFor([&] { return inkOfAll(resting); }));
    // Faded in, over motion.standard, before a press can reach it.
    REQUIRE(QTest::qWaitFor([&] { return host.bar->opacity() == 1.0; }));
    auto* window = qobject_cast<QQuickWindow*>(host.window.get());
    REQUIRE(window != nullptr);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, centreOf(*handle));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->property("pressed").toBool(); }));
    CHECK(QTest::qWaitFor([&] { return inkOfAll(pressed); }));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, centreOf(*handle));
    CHECK(QTest::qWaitFor([&] { return inkOfAll(resting); }));

    // Hovered alone, without a press, the glyphs take the active ink too. The offscreen platform enables no
    // hover effects of its own, so the bar is asked to take hover as a desktop style would have it do.
    auto const away =
        QPoint(static_cast<int>(host.paneWidth / 2), static_cast<int>(host.chrome->height() / 2));
    QTest::mouseMove(window, away);
    host.bar->setProperty("hoverEnabled", true);
    QTest::mouseMove(window, centreOf(*handle));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->property("hovered").toBool(); }));
    REQUIRE_FALSE(host.bar->property("pressed").toBool());
    CHECK(QTest::qWaitFor([&] { return inkOfAll(pressed); }));
    QTest::mouseMove(window, away);
    CHECK(QTest::qWaitFor([&] { return inkOfAll(resting); }));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome draws the scrollbar's marks, and a click on one jumps there (offscreen)",
          "[contour][gui][qml][scrollbar-marks]")
{
    // The tick layer over the scrollbar: one thin line per published mark, at its fraction of the track, in
    // its kind's colour -- and a click on one asks the session to bring that line to the top.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto* layer = host.bar->findChild<QQuickItem*>(QStringLiteral("scrollbarMarkLayer"));
    REQUIRE(layer != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(host.window.get());
    REQUIRE(window != nullptr);

    auto session = createScrollableSession();
    session->setScrollbarMarks(QVariantList { markEntry(0.1, vtbackend::ScrollbarMarkKind::Failure, 42),
                                              markEntry(0.5, vtbackend::ScrollbarMarkKind::Command, 4242) },
                               markColors());
    auto const bind = [&](MockSession& bound) {
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&bound)));
        QCoreApplication::processEvents();
        // A shown bar fades in over motion.standard; only then does every press reach it.
        REQUIRE(QTest::qWaitFor(
            [&] { return host.bar->opacity() == 1.0 || !host.bar->property("wanted").toBool(); }));
    };

    // At the bottom of 1000 rows of history under a 25-row page, the page is the track's last 2.4 %: far
    // below the handle's minimum size, so the handle is drawn over the track's last tenth. A tick at 0.95,
    // the head of a block some 25 rows above the page, lies under the drawn handle. Binds @p deep so, and
    // returns where that tick is, in the window's coordinates.
    auto const bindAtTheBottomOfADeepHistory = [&](ScrollingMockSession& deep) {
        deep.setScrollbarVisible(true);
        deep.setHistoryLineCount(1000);
        deep.setScrollbarMarks(QVariantList { markEntry(0.95, vtbackend::ScrollbarMarkKind::Failure, 950) },
                               markColors());
        bind(deep);
        auto const position = host.bar->property("position").toReal();
        REQUIRE(position == Catch::Approx(1.0 - host.bar->property("size").toReal()));
        REQUIRE(0.95 < position); // a row off the screen

        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 1);
        auto const onTheTick = centreOf(*ticks[0]);
        auto const* handle = host.bar->property("contentItem").value<QQuickItem*>();
        REQUIRE(handle != nullptr);
        REQUIRE(handle->contains(handle->mapFromScene(QPointF(onTheTick))));
        return onTheTick;
    };

    SECTION("one tick per mark, at its fraction of the track, in its kind's colour")
    {
        bind(*session);
        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        CHECK(ticks[0]->y() == Catch::Approx(std::round(0.1 * layer->height())));
        CHECK(ticks[1]->y() == Catch::Approx(std::round(0.5 * layer->height())));
        CHECK(ticks[0]->width() == Catch::Approx(layer->width()));
        CHECK(ticks[0]->property("color").value<QColor>() == QColor(Qt::red));  // Failure
        CHECK(ticks[1]->property("color").value<QColor>() == QColor(Qt::gray)); // Command

        // The session was told the track's height, so its pixel buckets are the pixels drawn here.
        CHECK(session->trackPixels == qRound(layer->height()));

        // Above the bar, which would otherwise take every press first, and below the handle, which is drawn
        // over the ticks it passes.
        auto const* handle = host.bar->property("contentItem").value<QQuickItem*>();
        REQUIRE(handle != nullptr);
        CHECK(layer->z() >= 0.0);
        CHECK(handle->z() > layer->z());
    }

    SECTION("a mark on the track's last pixel is drawn inside the track")
    {
        session->setScrollbarMarks(QVariantList { markEntry(1.0, vtbackend::ScrollbarMarkKind::Command, 7) },
                                   markColors());
        bind(*session);
        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 1);
        CHECK(ticks[0]->y() + ticks[0]->height() <= layer->height());
    }

    SECTION("a click on a tick asks the session to bring its line to the top")
    {
        bind(*session);
        host.bar->setProperty("position", 0.9); // the handle well clear of the tick at 0.1
        QCoreApplication::processEvents();

        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(*ticks[0]));
        QCoreApplication::processEvents();

        CHECK(session->jumpedTo == std::vector<qint64> { 42 });
        // The tick kept the press: the bar did not move to it.
        CHECK(host.bar->property("position").toReal() == Catch::Approx(0.9));
    }

    SECTION("a tick takes a press a few pixels off its line, a nine-pixel target")
    {
        bind(*session);
        host.bar->setProperty("position", 0.9);
        QCoreApplication::processEvents();

        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        CHECK(ticks[0]->height() == Catch::Approx(3));
        // Two pixels above its top edge: outside the line, inside the margin around it.
        auto const aboveTheLine = ticks[0]->mapToScene(QPointF(ticks[0]->width() / 2, -2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, aboveTheLine);
        QCoreApplication::processEvents();

        CHECK(session->jumpedTo == std::vector<qint64> { 42 });
    }

    SECTION("a click on a tick under the enlarged handle still jumps, and leaves the bar where it was")
    {
        auto deep = ScrollingMockSession {};
        auto const onTheTick = bindAtTheBottomOfADeepHistory(deep);

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, onTheTick);
        QCoreApplication::processEvents();

        CHECK(deep.jumpedTo == std::vector<qint64> { 950 });
        CHECK(deep.scrollOffset() == 0);
    }

    SECTION("a drag that starts on a tick under the enlarged handle moves the handle, and jumps nowhere")
    {
        auto deep = ScrollingMockSession {};
        auto const onTheTick = bindAtTheBottomOfADeepHistory(deep);

        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, onTheTick);
        for (auto const step: std::views::iota(1, 5))
            QTest::mouseMove(window, onTheTick - QPoint(0, 25 * step));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, onTheTick - QPoint(0, 100));
        QCoreApplication::processEvents();

        CHECK(deep.scrollOffset() > 0);
        CHECK(deep.jumpedTo.empty());
    }

    SECTION("no marks: no ticks, and the bar is exactly the bar it was")
    {
        auto plain = createScrollableSession();
        bind(*plain);
        CHECK(markTicksOf(*layer).empty());
        CHECK(QTest::qWaitFor([&] { return host.bar->isVisible(); }));
        CHECK(host.bar->width() == Catch::Approx(host.thin));
        CHECK(host.bar->x() == Catch::Approx(host.paneWidth - host.thin));
    }

    SECTION("an offset the session reports echoes back unchanged while the pointer rests on the bar")
    {
        // After a click on a tick the pointer still rests on the bar, so the offset the jump set comes back
        // through the bar's position: rows above over rows in all, times rows in all. That can land a hair
        // under the integer -- 16 rows of history under a 25-row page, two rows up, give 1.9999999999999982
        // -- and written back truncated it would name the row below. The bar is made active by holding its
        // handle, since an offscreen window gets no hover.
        auto scrolling = ScrollingMockSession {};
        scrolling.setScrollbarVisible(true);
        scrolling.setHistoryLineCount(16);
        bind(scrolling);
        auto const* handle = host.bar->property("contentItem").value<QQuickItem*>();
        REQUIRE(handle != nullptr);
        auto const onTheHandle = centreOf(*handle);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, onTheHandle);
        QCoreApplication::processEvents();
        REQUIRE(host.bar->property("active").toBool());

        scrolling.setScrollOffset(2);
        QCoreApplication::processEvents();
        CHECK(scrolling.scrollOffset() == 2);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, onTheHandle);
    }

    SECTION("hiding the bar -- position: Hidden, or hide_in_alt_screen -- hides its ticks")
    {
        bind(*session);
        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        REQUIRE(QTest::qWaitFor([&] { return ticks[0]->isVisible(); }));

        session->setScrollbarVisible(false);
        QCoreApplication::processEvents();
        CHECK(QTest::qWaitFor([&] { return !ticks[0]->isVisible(); })); // with the bar, once faded out
    }

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome lets a glide finish with the pointer on the scrollbar (offscreen)",
          "[contour][gui][qml][scrollbar]")
{
    // With the pointer on the bar, the bar is active, and a move of its own is the user's: the chrome writes
    // it back, and the session ends a glide to take it. A glide's frames move the bar too, to follow the
    // view; written back as well, they ended the glide at its first frame. The bar is made active by holding
    // its handle, since an offscreen window gets no hover.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto* window = qobject_cast<QQuickWindow*>(host.window.get());
    REQUIRE(window != nullptr);
    auto session = GlidingMockSession {};
    session.setScrollbarVisible(true);
    session.setHistoryLineCount(1000);
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&session)));
    REQUIRE(QTest::qWaitFor([&] { return host.bar->opacity() == 1.0; })); // faded in

    auto const* handle = host.bar->property("contentItem").value<QQuickItem*>();
    REQUIRE(handle != nullptr);
    auto const onTheHandle = centreOf(*handle);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, onTheHandle);
    QCoreApplication::processEvents();
    REQUIRE(host.bar->property("active").toBool());

    for (auto const frame: { 3, 7, 12 })
    {
        session.glideTo(frame);
        QCoreApplication::processEvents();
    }
    CHECK(session.written.empty());
    CHECK(session.scrollOffset() == 12);

    // A step of the bar's own is still the user's, and written back.
    REQUIRE(QMetaObject::invokeMethod(host.bar, "increase"));
    QCoreApplication::processEvents();
    CHECK(session.written == std::vector { 11 });

    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, onTheHandle);
    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome over a live session: the scrollbar's strip is the bar's, the cell beside it the "
          "terminal's (offscreen)",
          "[contour][gui][qml][scrollbar-marks]")
{
    // The scrollbar is shown on the right by default, as an overlay that takes no column from the page: its
    // strip covers the edge of the grid. A press there must go to the bar alone, while a press one cell to
    // its left must reach the terminal as it always did -- a selection, or a report to the application.
    contour::test::QmlMessageCapture const warnings;
    LiveChromeHarness const h;

    auto& terminal = h.live.session->terminal();
    terminal.writeToScreen(numberedLines(100));

    // The travel that shows the bar is published a frame after the output, by the refresh it asks for.
    REQUIRE(QTest::qWaitFor([&] { return h.bar->isVisible(); }, 1000));
    CHECK(h.bar->x() + h.bar->width() == Catch::Approx(h.live.display->width()));

    auto const cellWidth =
        static_cast<double>(unbox(h.live.display->cellSize().width)) / h.live.display->devicePixelRatio();

    // A strip, not a band: whatever it covers of the grid is the edge of the last column or two.
    CHECK(h.bar->width() < 2 * cellWidth);
    auto const middle = h.bar->height() / 2;
    auto const onTheBar = h.bar->mapToScene(QPointF(h.bar->width() / 2, middle)).toPoint();
    auto const besideTheBar = h.bar->mapToScene(QPointF(-cellWidth / 2, middle)).toPoint();

    SECTION("selection: the bar's strip starts none, the cell beside it does")
    {
        // Asked while the button is down: a press released where it went down is a click, which clears the
        // selection it started.
        h.press(onTheBar);
        CHECK(h.isBarPressed());
        CHECK_FALSE(h.hasSelection());
        h.release(onTheBar);

        h.press(besideTheBar);
        CHECK_FALSE(h.isBarPressed());
        CHECK(h.hasSelection());
        h.release(besideTheBar);
    }

    SECTION("mouse reporting: the application hears nothing of the bar's strip, and the cell beside it")
    {
        terminal.writeToScreen("\033[?1000h\033[?1006h");
        QCoreApplication::processEvents();
        auto const before = h.live.pty->stdinSnapshot();

        h.press(onTheBar);
        CHECK(h.isBarPressed());
        h.release(onTheBar);

        h.press(besideTheBar);
        CHECK_FALSE(h.isBarPressed());
        h.release(besideTheBar);
        CHECK_FALSE(h.hasSelection());

        // The reports are written by the PTY's reader thread, which the input only wakes, so they are waited
        // for rather than looked for at once. The writes keep their order: whatever the bar's strip had let
        // through would stand ahead of the click beside it, so that click must be all that follows `before`.
        auto const cell = terminal.currentMousePosition();
        auto const click =
            std::format("\033[<0;{0};{1}M\033[<0;{0};{1}m", unbox(cell.column) + 1, unbox(cell.line) + 1);
        REQUIRE(QTest::qWaitFor(
            [&] { return h.live.pty->stdinSnapshot().size() >= before.size() + click.size(); }, 5000));
        CHECK(h.live.pty->stdinSnapshot().substr(before.size()) == click);
    }

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome over a live session: a click on a tick puts its block's head on the top row "
          "(offscreen)",
          "[contour][gui][qml][scrollbar-marks]")
{
    // End to end: the session publishes the ticks on its frame timer for the track the chrome reported, the
    // chrome draws them, and a click on one scrolls the viewport -- which the bar, still under the pointer,
    // echoes back as an offset that must name the same row.
    contour::test::QmlMessageCapture const warnings;
    LiveChromeHarness const h;

    auto& terminal = h.live.session->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "out 1", "out 2", "out 3" }, 2));
    terminal.writeToScreen(vtbackend::test::commandRun("ls", { "out 1", "out 2", "out 3" }, 0));
    terminal.writeToScreen(numberedLines(300));

    auto* layer = h.bar->findChild<QQuickItem*>(QStringLiteral("scrollbarMarkLayer"));
    REQUIRE(layer != nullptr);
    auto ticks = markTicksOf(*layer);
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        if (ticks.size() == 2)
            break;
        QTest::qWait(5);
        ticks = markTicksOf(*layer);
    }
    REQUIRE(ticks.size() == 2);
    REQUIRE(QTest::qWaitFor([&] { return h.bar->opacity() == 1.0; })); // faded in, so the click reaches it
    auto const target =
        h.live.session->scrollbarMarks().at(1).toMap().value(QStringLiteral("targetLine")).toLongLong();

    QTest::mouseClick(h.live.window.get(), Qt::LeftButton, Qt::NoModifier, centreOf(*ticks[1]));
    QCoreApplication::processEvents();

    auto const lock = std::scoped_lock { terminal };
    CHECK(terminal.primaryScreen().grid().stableLineIdOf(terminal.viewport().topLine()) == target);

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("No QML detaches an anchor conditionally (the full-pane-scrollbar trap)", "[contour][gui][qml]")
{
    // A source tripwire for the bug class the scrollbar shipped, because no runtime assertion can catch its
    // reintroduction in a component no test happens to instantiate.
    //
    // `anchors.<edge>: cond ? parent.<edge> : undefined` is only ever written as one half of a pair that
    // switches an item between two opposite edges. The two bindings re-evaluate ONE AT A TIME, so a flip
    // transiently leaves both edges anchored; QQuickAnchors then stretches the item across its parent and
    // latches that as an explicit width/height (QQuickItem::setWidth marks widthValid), which the implicit
    // size can never undo. The item stays parent-sized for good. Bind `x`/`y` instead — see the vbar in
    // SessionChrome.qml and the TitleBar in Main.qml.
    //
    // Comment lines are skipped: the two fix sites document the trap using the very syntax it flags.
    //
    // The leading word boundary is spelled (?<!\w) rather than the usual backslash-b escape: identical
    // zero-width meaning, but check-spelling tokenizes this pattern as prose and would glue that escape
    // onto the word behind it, then demand the resulting nonsense token be added to the dictionary.
    static auto const conditionalDetach = QRegularExpression(QStringLiteral(
        R"(anchors\.(left|right|top|bottom|horizontalCenter|verticalCenter)\s*:.*(?<!\w)undefined\b)"));
    REQUIRE(conditionalDetach.isValid()); // an invalid pattern matches nothing and guards nothing

    auto offenders = QStringList {};
    // EVERY QML module this application ships, by its own resource root (see qt_add_qml_module in
    // CMakeLists.txt): the raw .qml files live there alongside the qmlcachegen output, which is what
    // makes this source scan possible at all. A module missing from this list is not scanned and
    // nothing says so -- the ContourTui style went unscanned exactly that way, because the other
    // root's files kept a single shared count non-zero. Hence the per-root floor below.
    auto const roots = std::array {
        QStringLiteral(":/qt/qml/Contour/Ui"),
        QStringLiteral(":/qt/qml/ContourTui"),
    };
    // Both modules hold well over this many components, so it is a floor on "did this root resolve to
    // the module at all", not a headcount to keep updated when one file is added or removed.
    constexpr auto LeastFilesPerRoot = 10;

    for (auto const& root: roots)
    {
        auto scanned = 0;
        auto it = QDirIterator(
            root, QStringList { QStringLiteral("*.qml") }, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            auto file = QFile(it.next());
            REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
            ++scanned;
            auto lineNumber = 0;
            auto stream = QTextStream(&file);
            while (!stream.atEnd())
            {
                auto const line = stream.readLine();
                ++lineNumber;
                if (line.trimmed().startsWith(QStringLiteral("//")))
                    continue;
                if (conditionalDetach.match(line).hasMatch())
                    offenders << QStringLiteral("%1:%2: %3")
                                     .arg(file.fileName())
                                     .arg(lineNumber)
                                     .arg(line.trimmed());
            }
        }

        // Per root, so a root that resolves to nothing fails instead of hiding behind the other one's
        // files -- which is exactly how the style module went unscanned when it was first added.
        INFO("root: " << root.toStdString() << " scanned " << scanned);
        REQUIRE(scanned >= LeastFilesPerRoot);
    }

    // ONE ScopedMessage covering the CHECK: an INFO raised inside a loop is destroyed on each iteration
    // and never survives to the assertion, so a failure would name no file at all.
    INFO("conditionally detached anchors (bind x/y instead):\n"
         << offenders.join(QStringLiteral("\n")).toStdString());
    CHECK(offenders.isEmpty());
}

TEST_CASE("Tab color flyout offers a dependency-free arbitrary-color entry (offscreen)",
          "[contour][gui][qml]")
{
    // Guards the arbitrary-RGB tab-color feature AND its robustness: the flyout must carry no hard
    // QtQuick.Dialogs dependency (a missing such module used to cascade up and break the whole Main.qml
    // where it is not installed), yet still expose a hex input field for entering any color. Loading the
    // component without error already proves the absent-module hazard is gone; finding the field proves
    // the entry point is there.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TabColorFlyout.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("tabIndex", 0);
    std::unique_ptr<QObject> flyout(component.createWithInitialProperties(initial));
    REQUIRE(flyout != nullptr);

    // A Popup defers building its contentItem until shown; reading the property forces that deferred
    // execution, instantiating the statically-declared hex field (unlike the swatch Repeater delegates,
    // a plain child materializes here without hosting/opening the popup).
    REQUIRE(flyout->property("contentItem").value<QQuickItem*>() != nullptr);
    QCoreApplication::processEvents();

    CHECK(flyout->findChild<QObject*>(QStringLiteral("customColorField")) != nullptr);
}

TEST_CASE("PaneNode renders a split tree without recursive-instantiation errors (offscreen)",
          "[contour][gui][qml][split]")
{
    // Regression test: a QML component cannot instantiate itself by name (Qt reports "instantiated
    // recursively" and the whole tree fails to load). PaneNode therefore loads its split children by
    // URL. Merely *loading* PaneNode.qml does not exercise this — the recursion only fires when it is
    // instantiated against a SPLIT node, whose children are themselves PaneNodes. So we instantiate
    // PaneNode against a split-of-two-leaves mock and assert no QML warnings are emitted.
    QQmlEngine engine;

    // Stub the C++ display type so PaneNode's leaf branch (TerminalPane → ContourTerminal) resolves
    // in the test engine, which does not link the real display.
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");

    // Capture any QML warning (a recursive-instantiation failure surfaces as a warning, not as a
    // component error, so we must watch the message handler).
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    // A split node (vertical) with two leaf children.
    auto* leftLeaf = new MockPaneProxy(/*leaf*/ true);
    auto* rightLeaf = new MockPaneProxy(/*leaf*/ true);
    MockPaneProxy splitNode(/*leaf*/ false, leftLeaf, rightLeaf);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    {
        INFO("PaneNode.qml errors:");
        for (auto const& e: component.errors())
            INFO("  " << e.toString().toStdString());
        REQUIRE(component.isReady());
    }

    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(&splitNode)));
    std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));

    // Let the URL-loaded child Loaders instantiate their PaneNodes.
    QCoreApplication::processEvents();

    REQUIRE(paneNode != nullptr);
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    // No "instantiated recursively" / "Type PaneNode unavailable" warnings.
    auto const recursionWarnings = warnings.count(
        [](QString const& w) { return w.contains("recursively") || w.contains("unavailable"); });
    CHECK(recursionWarnings == 0);

    // The recursive split children are URL-loaded and receive `node` only via the deferred onLoaded
    // assignment (after construction). If `node` were a `required property`, Qt would emit "Required property
    // node was not initialized" for every child and leave it null, so the pane never renders (the
    // transparent-split-pane bug). Assert that class of warning does not appear.
    auto const uninitializedRequiredWarnings = warnings.count([](QString const& w) {
        return w.contains("Required property") && w.contains("was not initialized");
    });
    CHECK(uninitializedRequiredWarnings == 0);
}

#include <QmlComponents_test.moc>
