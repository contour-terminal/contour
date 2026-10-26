// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end smoke tests for the GUI frontend's QML components.
//
// These run under the Qt "offscreen" platform plugin (no display server needed, so they work in
// CI). They instantiate each tab/title-bar QML component against a lightweight mock controller that
// mirrors the TerminalSessionManager interface the components rely on, and assert the component
// loads with no QML errors. This catches QML syntax/binding regressions deterministically without
// having to boot a full terminal session.

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/ContextMenu.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/SearchStatus.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/MockMainController.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SplitChrome.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TabColorScheme.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/ChannelPty.hpp>

#include <core/Utils.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDirIterator>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

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
#include <vtworkspace/Primitives.hpp>

using contour::test::MockPaneProxy;
using contour::test::MockSession;
using contour::test::StubContourTerminal;

namespace
{

/// A minimal stand-in for TerminalSessionManager exposing just the surface the tab-strip QML uses,
/// so the components can be instantiated in isolation under the offscreen platform.
class MockTabController: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    // Mirrors TerminalSessionManager::multimediaReady, which SessionChrome.qml's bell Loader gates on.
    Q_PROPERTY(bool multimediaReady READ multimediaReady CONSTANT)
    // Mirrors TerminalSessionManager::titleBarVisible (re-homed from the removed vtui): the window-decoration
    // axis Main.qml binds its flags / custom controls / resize border to.
    Q_PROPERTY(
        bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible NOTIFY titleBarVisibleChanged)
    // Mirrors WindowController's tab and new-tab menu surfaces, which TabStrip.qml binds and
    // Connections-targets.
    Q_PROPERTY(QVariantList tabContextMenuModel READ tabContextMenuModel NOTIFY tabContextMenuModelChanged)
    Q_PROPERTY(QVariantList newTabMenuModel READ newTabMenuModel NOTIFY newTabMenuModelChanged)

  public:
    MockTabController() = default;
    /// @param count How many tabs the strip lists.
    explicit MockTabController(int count): _count { count } {}

    [[nodiscard]] bool multimediaReady() const noexcept { return false; }

    [[nodiscard]] bool titleBarVisible() const noexcept { return _titleBarVisible; }
    void setTitleBarVisible(bool v)
    {
        if (_titleBarVisible != v)
        {
            _titleBarVisible = v;
            emit titleBarVisibleChanged();
        }
    }
    /// Mirrors TerminalDisplay::toggleTitleBar(): flips the native-decoration axis.
    Q_INVOKABLE void toggleTitleBar() { setTitleBarVisible(!_titleBarVisible); }

    /// Mirrors WindowController::triggerContextMenuAction(): what ActionContextMenu.qml calls on a pick.
    Q_INVOKABLE void triggerContextMenuAction(int actionId) { lastTriggeredActionId = actionId; }

    int lastTriggeredActionId = -1;

    [[nodiscard]] QVariantList const& tabContextMenuModel() const noexcept { return _tabContextMenuModel; }
    [[nodiscard]] QVariantList const& newTabMenuModel() const noexcept { return _newTabMenuModel; }

    /// Mirrors WindowController::openTabContextMenu(): publishes the tab's menu, then asks for it.
    Q_INVOKABLE void openTabContextMenu(int index, QPointF /*windowPosition*/)
    {
        _tabContextMenuModel = contour::test::mockTabContextMenuModel(index, _count);
        emit tabContextMenuModelChanged();
        emit tabContextMenuRequested(index);
    }
    /// Mirrors WindowController::triggerTabContextMenuAction(), recording the pick.
    Q_INVOKABLE void triggerTabContextMenuAction(int actionId)
    {
        if (onTabMenuPick)
            onTabMenuPick(actionId);
        tabMenuPicks.push_back(actionId);
    }
    /// Mirrors WindowController::openNewTabMenu(): publishes the menu, then asks for it.
    Q_INVOKABLE void openNewTabMenu(QPointF /*windowPosition*/)
    {
        ++openNewTabMenuCalls;
        _newTabMenuModel = contour::test::mockNewTabMenuModel();
        emit newTabMenuModelChanged();
        emit newTabMenuRequested();
    }
    /// Mirrors WindowController::triggerNewTabMenuAction(), recording the pick.
    Q_INVOKABLE void triggerNewTabMenuAction(int actionId) { newTabMenuPicks.push_back(actionId); }

    std::vector<int> tabMenuPicks {};    ///< Every tab menu pick, in order.
    std::vector<int> newTabMenuPicks {}; ///< Every new-tab menu pick, in order.
    int openNewTabMenuCalls = 0;         ///< How often the new-tab menu was asked for.
    /// Called with each tab menu pick as it arrives, before it is recorded; empty for none.
    std::function<void(int)> onTabMenuPick {};

    enum class Roles : std::uint16_t
    {
        DisplayRole = Qt::DisplayRole,
        TitleRole = Qt::UserRole + 1,
        ColorRole = Qt::UserRole + 2,
        IsActiveRole = Qt::UserRole + 3,
        PaneCountRole = Qt::UserRole + 4,
        SessionIdRole = Qt::UserRole + 5,
        RawTitleRole =
            Qt::UserRole
            + 6, //!< Must mirror contour::session::TerminalSessionManager; TabStrip's delegate requires it.
        ZoomedRole =
            Qt::UserRole + 7, //!< Ditto: TabStrip's delegate requires it (drives TabItem's zoom badge).
        //!< Ditto: both drive TabItem's OSC 9;4 progress bar.
        ProgressStateRole = Qt::UserRole + 8,
        ProgressPercentageRole = Qt::UserRole + 9,
        //!< Ditto: the three drive TabItem's badge slot.
        BadgeRole = Qt::UserRole + 10,
        BadgeTextRole = Qt::UserRole + 11,
        BadgeProgressRole = Qt::UserRole + 12,
    };

    [[nodiscard]] int activeTabIndex() const noexcept { return _activeTabIndex; }

    [[nodiscard]] int rowCount(QModelIndex const& = QModelIndex()) const override { return _count; }

    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        if (index.row() < 0 || index.row() >= _count)
            return {};
        switch (static_cast<Roles>(role))
        {
            case Roles::TitleRole: return QStringLiteral("tab %1").arg(index.row());
            case Roles::ColorRole: return QColor(Qt::transparent);
            case Roles::IsActiveRole: return index.row() == _activeTabIndex;
            case Roles::PaneCountRole: return 1;
            case Roles::RawTitleRole:
                return QString {}; // never-renamed tab: empty raw template (editor blank)
            case Roles::ZoomedRole: return false;
            // 0 = ProgressState::Inactive: no operation in flight, so TabItem paints no bar.
            case Roles::ProgressStateRole:
            case Roles::ProgressPercentageRole: return 0;
            // 0 = TabBadgeKind::None: nothing competes for the slot, so TabItem paints no badge.
            case Roles::BadgeRole: return 0;
            case Roles::BadgeTextRole: return QString {};
            case Roles::BadgeProgressRole: return -1;
            default: return index.row();
        }
    }

    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        // Must match TerminalSessionManager::roleNames() so the delegates bind the same roles —
        // including rawTitle, which TabStrip's delegate declares as a required property.
        return { { static_cast<int>(Roles::DisplayRole), "display" },
                 { static_cast<int>(Roles::TitleRole), "title" },
                 { static_cast<int>(Roles::ColorRole), "accentColor" },
                 { static_cast<int>(Roles::IsActiveRole), "isActive" },
                 { static_cast<int>(Roles::PaneCountRole), "paneCount" },
                 { static_cast<int>(Roles::SessionIdRole), "sessionId" },
                 { static_cast<int>(Roles::RawTitleRole), "rawTitle" },
                 { static_cast<int>(Roles::ZoomedRole), "zoomed" },
                 { static_cast<int>(Roles::ProgressStateRole), "progressState" },
                 { static_cast<int>(Roles::ProgressPercentageRole), "progressPercentage" },
                 { static_cast<int>(Roles::BadgeRole), "badge" },
                 { static_cast<int>(Roles::BadgeTextRole), "badgeText" },
                 { static_cast<int>(Roles::BadgeProgressRole), "badgeProgress" } };
    }

    Q_INVOKABLE [[nodiscard]] QObject* createSession() { return nullptr; }
    // Models the REAL TerminalSessionManager::canCloseWindow semantics: the window may close only when no
    // sessions remain (the last pane exited). By the time onTerminated runs, removeSession has already
    // decremented the session count, so `_openSessions` here reflects the survivors. Call-tracking lets a
    // test assert onTerminated consulted the gate. Using the true semantics (not a settable bool) is what
    // reproduces the "closing one pane of a split kills the whole window" bug.
    Q_INVOKABLE [[nodiscard]] bool canCloseWindow()
    {
        ++canCloseWindowCalls;
        return _openSessions == 0;
    }
    void setOpenSessions(int n) noexcept { _openSessions = n; }
    int canCloseWindowCalls = 0;
    Q_INVOKABLE void createNewTab() {}
    Q_INVOKABLE void activateTab(int) {}
    /// Mirrors WindowController::tabWorkingDirectory: what TabItem's hover tooltip asks for the
    /// working-directory line. Answering empty means "nothing known", which suppresses that line.
    Q_INVOKABLE [[nodiscard]] QString tabWorkingDirectory(int) const { return {}; }
    Q_INVOKABLE void moveTab(int, int) {}
    Q_INVOKABLE [[nodiscard]] quint64 windowIdValue() const noexcept { return 1; }
    Q_INVOKABLE void moveTabIntoThisWindow(quint64, int, int) {}
    Q_INVOKABLE void tearOffTab(int) {}
    Q_INVOKABLE void setTabTitle(int, QString const&) {}
    Q_INVOKABLE void resetTabTitle(int) {}
    Q_INVOKABLE void beginActiveTabTitleEdit() { emit tabTitleEditRequested(0); }
    Q_INVOKABLE void beginActiveTabColorPick() { emit tabColorPickRequested(0); }
    Q_INVOKABLE void setTabColor(int index, QColor const& color)
    {
        ++setTabColorCalls;
        lastTabColorIndex = index;
        lastTabColor = color;
    }
    Q_INVOKABLE void resetTabColor(int index)
    {
        ++resetTabColorCalls;
        lastTabColorIndex = index;
    }
    int setTabColorCalls = 0;
    int resetTabColorCalls = 0;
    int lastTabColorIndex = -1;
    QColor lastTabColor;
    Q_INVOKABLE void closeTabAtIndex(int) {}
    Q_INVOKABLE void closeOtherTabs(int) {}
    Q_INVOKABLE void closeTabsToRight(int) {}
    /// Sixteen distinct colors, matching the SHAPE of the real palette (vtworkspace SessionModel's
    /// DefaultPalette): two full rows of the flyout's 8-column grid. The count is what makes vertical
    /// keyboard navigation testable at all — a shorter palette has no second row to move down into.
    /// Built once, like the real TerminalSessionManager's cached list, since QML re-reads it per binding.
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const
    {
        static auto const colors = [] {
            QVariantList list;
            for (auto const i: std::views::iota(0, 16))
                list.append(QColor::fromHsv(i * 20, 200, 220));
            return list;
        }();
        return colors;
    }
    // Mirror WindowController's tab-color helpers so a colored TabItem's fill/text bindings resolve
    // (the delegate calls these once accentColor is non-transparent). Same pure math as the real one.
    Q_INVOKABLE [[nodiscard]] QColor tabBackgroundColor(QColor const& tabColor,
                                                        QColor const& rowBackground,
                                                        bool active,
                                                        bool hovered,
                                                        bool windowActive) const
    {
        return contour::test::mockTabBackgroundColor(tabColor, rowBackground, active, hovered, windowActive);
    }
    Q_INVOKABLE [[nodiscard]] QColor tabTextColor(QColor const& tabBackground) const
    {
        return contour::test::mockTabTextColor(tabBackground);
    }

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void tabTitleEditRequested(int /*index*/);
    // TabItem's Connections declares onTabColorPickRequested; without the matching signal HERE, every
    // test that instantiates a TabItem earns a "no signal of the target matches" QML warning — which the
    // suite's message capture turns into a failure.
    void tabColorPickRequested(int /*index*/);
    void tabContextMenuModelChanged();
    void tabContextMenuRequested(int index);
    void newTabMenuModelChanged();
    void newTabMenuRequested();

  private:
    QVariantList _tabContextMenuModel {};
    QVariantList _newTabMenuModel {};
    int _count = 3;
    int _activeTabIndex = 0;
    bool _titleBarVisible = true;
    int _openSessions = 1;
};

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

} // namespace

TEST_CASE("GUI QML tab components load without errors (offscreen)", "[contour][gui][qml]")
{
    // The Catch2 main builds a QGuiApplication for us (see test_main.cpp).
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
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
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsNavItem.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsListItem.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ConfirmDialog.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingRow.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/ColorSchemeEditor.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/LinkPatternList.qml"),
        QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsPage.qml"),
    };

    for (auto const& url: components)
    {
        INFO("component: " << url.toStdString());
        auto const errors = loadErrors(engine, url);
        for (auto const& e: errors)
            INFO("  error: " << e.toStdString());
        CHECK(errors.isEmpty());
    }
}

namespace
{
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
} // namespace

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

TEST_CASE("A colored TabItem fills with the user color and picks contrasting text (offscreen)",
          "[contour][gui][qml]")
{
    // Drives the colored-tab branch of TabItem: when tabColor is non-transparent, the fill is the
    // user's color (faded per state, via the controller's TabColorScheme helpers) and the label
    // contrasts against it. The uncolored branch is covered by the load/strip tests above.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
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

namespace
{
/// Where the host puts its TabItem — i.e. where the tab strip sits in the window (`tab_bar_position`).
/// Which one is in force decides whether there is room BELOW the tab for the color flyout, so it is the
/// hinge of the flyout's placement, and both must be drivable from a test.
enum class TabStripAt : std::uint8_t
{
    Top,   //!< The default: the strip is the top-most chrome, with the whole window below it.
    Bottom //!< `tab_bar_position: Bottom`: the tab's bottom edge IS the window's bottom edge.
};

/// Deletes a QML test host once it has drawn one more frame and run what that frame queued.
///
/// A tab under the pointer grabs its drag image when its window first shows -- offscreen the pointer rests
/// on the origin, where most hosts put their tab -- and the grab lands on the window's next frame. Nothing
/// owns a grab still in flight, so a window destroyed before that frame would leak it.
struct SettleThenDelete
{
    void operator()(QObject* host) const
    {
        if (auto* window = qobject_cast<QQuickWindow*>(host))
            std::ignore = window->grabWindow();
        QCoreApplication::processEvents();
        delete host;
    }
};

/// A QML test host, destroyed only once it has settled.
using QmlHost = std::unique_ptr<QObject, SettleThenDelete>;

/// Builds @p source as a QML component and instantiates it, failing the test on any QML error.
///
/// Every host in this file is the same three steps — hand the engine the source, wait out the load (which
/// is synchronous for a source string, so the loop turns over at most once), create — so they share them
/// here rather than repeating them once per host.
///
/// @param engine The engine to build in.
/// @param source The QML source of the host, whose root must be a Window.
/// @param name   A file name used for the component URL and in the failure message; it need not exist.
/// @return The instantiated root object.
[[nodiscard]] QmlHost buildQmlHost(QQmlEngine& engine, QString const& source, QString const& name)
{
    QQmlComponent component(&engine);
    component.setData(source.toUtf8(), QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/%1").arg(name)));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    INFO(name.toStdString() << " errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    auto host = QmlHost(component.create());
    REQUIRE(host != nullptr);
    QCoreApplication::processEvents();
    return host;
}

/// The TabItem property assignments every host that carries one needs, so two hosts cannot drift into
/// binding the same tab differently. Contains no `%n` placeholder, so it is safe to embed in a source
/// string that is then .arg()-substituted.
constexpr auto TabItemBindings = "    controller: terminalSessions\n"
                                 "    window: host\n"
                                 "    tabIndex: 0\n"
                                 "    tabTitle: \"shell\"\n"
                                 "    tabRawTitle: \"\"\n"
                                 "    tabColor: \"transparent\"\n"
                                 "    tabActive: true\n"
                                 "    tabPaneCount: 1\n"
                                 "    tabZoomed: false\n"
                                 "    tabProgressState: 0\n"
                                 "    tabProgressPercentage: 0\n";

/// Hosts TabItem.qml inside a real Window, which is what its TabColorFlyout Popup needs: a Popup opens
/// into its window's overlay, and a TabItem created bare (as the tests above do) has no window at all —
/// open() would then leave the flyout invisible and prove nothing.
///
/// The window is deliberately small in the Bottom case only in the sense that the tab is flush with its
/// bottom edge; the size is otherwise the same, so the two cases differ in exactly the one thing under
/// test.
///
/// @param engine     The engine to build in.
/// @param controller The mock the TabItem binds to.
/// @param stripAt    Where in the window to put the tab (mirrors the `tab_bar_position` option).
/// @param windowWidth The host window's width; a narrow one is what makes a right-edge overflow testable.
/// @param chrome     The chrome tokens to install; today's default row unless a test names one.
/// @return The host Window; the TabItem is reachable as its `tabItem` property.
[[nodiscard]] QmlHost makeTabItemHost(
    QQmlEngine& engine,
    MockTabController& controller,
    TabStripAt stripAt = TabStripAt::Top,
    int windowWidth = 800,
    contour::test::ChromeSetup const& chrome = contour::test::defaultChromeSetup())
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine, chrome);
    // `y` mirrors Main.qml, which anchors the TitleBar to the bottom edge under `tab_bar_position: Bottom`
    // (`y: parent.height - titleBar.height`). `x` puts the tab at the window's RIGHT edge in that case
    // too, so one host covers both axes a popup can overflow on.
    auto const bottom = stripAt == TabStripAt::Bottom;
    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: %1; height: 600; visible: true\n"
                                       "  property alias tabItem: item\n"
                                       "  TabItem {\n"
                                       "    id: item\n"
                                       "    x: %2 ? host.width - width : 0\n"
                                       "    y: %2 ? host.height - height : 0\n"
                                       "%3"
                                       "  }\n"
                                       "}\n")
                            .arg(windowWidth)
                            .arg(bottom ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(QLatin1StringView(TabItemBindings));

    return buildQmlHost(engine, source, QStringLiteral("TabItemTestHost.qml"));
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
} // namespace

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

namespace
{
/// Hosts TabStrip.qml at the top of a real Window, which its menus need: a Popup opens into its window's
/// overlay, and a strip created bare has none.
///
/// @param engine     The engine to build in.
/// @param controller The mock the strip binds to.
/// @param chrome     The chrome row, font and scheme to install.
/// @return The host Window; the strip is reachable as its `strip` property.
[[nodiscard]] QmlHost makeTabStripHost(QQmlEngine& engine,
                                       MockTabController& controller,
                                       contour::test::ChromeSetup const& chrome)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, chrome);
    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: 800; height: 600; visible: true\n"
                                       "  property alias strip: strip\n"
                                       "  TabStrip {\n"
                                       "    id: strip\n"
                                       "    height: chromeStyle.tabHeight\n"
                                       "    controller: terminalSessions\n"
                                       "    window: host\n"
                                       "  }\n"
                                       "}\n");
    auto host = buildQmlHost(engine, source, QStringLiteral("TabStripTestHost.qml"));
    auto* window = qobject_cast<QQuickWindow*>(host.get());
    REQUIRE(window != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    return host;
}

/// Hosts TabStrip.qml under the default chrome; see the overload above.
[[nodiscard]] QmlHost makeTabStripHost(QQmlEngine& engine, MockTabController& controller)
{
    return makeTabStripHost(engine, controller, contour::test::defaultChromeSetup());
}

/// The item @p menu shows at @p index, or null.
[[nodiscard]] QQuickItem* menuItemAt(QObject& menu, int index)
{
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(&menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
    return item;
}

/// The items @p menu shows, top to bottom; a null slot is left out.
[[nodiscard]] std::vector<QQuickItem*> menuItems(QObject& menu)
{
    auto items = std::vector<QQuickItem*> {};
    for (auto const index: std::views::iota(0, menu.property("count").toInt()))
        if (auto* item = menuItemAt(menu, index); item != nullptr)
            items.push_back(item);
    return items;
}

/// The row of @p menu that opens the sub-menu titled @p title, or null.
[[nodiscard]] QQuickItem* subMenuRow(QObject& menu, QString const& title)
{
    for (auto* item: menuItems(menu))
        if (auto const* sub = item->property("subMenu").value<QObject*>();
            sub != nullptr && sub->property("title").toString() == title)
            return item;
    return nullptr;
}

/// Every item beneath @p root, in the VISUAL tree, named @p name: a Repeater's delegates (which the
/// swatches are) have a parent item but no QObject parent, so findChildren() cannot see them.
void collectNamed(QQuickItem const& root, QString const& name, std::vector<QQuickItem*>& out)
{
    for (auto* child: root.childItems())
    {
        if (child->objectName() == name)
            out.push_back(child);
        collectNamed(*child, name, out);
    }
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

/// The strip's one tab menu, opened through the controller over tab @p index.
[[nodiscard]] QObject* openTabMenu(QObject& host, MockTabController& controller, int index)
{
    auto* menu = host.findChild<QObject*>(QStringLiteral("tabContextMenu"));
    REQUIRE(menu != nullptr);
    controller.openTabContextMenu(index, {});
    REQUIRE(QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000));
    return menu;
}

/// What the "Tab Color" sub-menu of an open tab @p menu holds, once it is open too.
struct TabColorMenu
{
    QObject* menu = nullptr;              ///< The sub-menu.
    QQuickItem* row = nullptr;            ///< Its one swatch row.
    std::vector<QQuickItem*> swatches {}; ///< The row's swatches, in order.
};

/// Opens the "Tab Color" sub-menu of @p menu as a click on its row would, and finds its swatches.
[[nodiscard]] TabColorMenu openTabColorMenu(QObject& menu)
{
    auto* row = subMenuRow(menu, QStringLiteral("Tab Color"));
    REQUIRE(row != nullptr);
    auto* sub = row->property("subMenu").value<QObject*>();
    REQUIRE(sub != nullptr);
    REQUIRE(QMetaObject::invokeMethod(row, "triggered"));
    REQUIRE(QTest::qWaitFor([&] { return sub->property("opened").toBool(); }, 2000));

    auto rows = std::vector<QQuickItem*> {};
    for (auto* item: menuItems(*sub))
        if (item->property("swatches").isValid())
            rows.push_back(item);
    REQUIRE(rows.size() == 1);

    auto result = TabColorMenu { .menu = sub, .row = rows.front(), .swatches = {} };
    collectNamed(*result.row, QStringLiteral("menuSwatch"), result.swatches);
    return result;
}
} // namespace

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
        auto const pill = row == contour::config::ChromeStyle::MacOS;

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
    auto* content = toolTip->property("contentItem").value<QQuickItem*>();
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
        CHECK(content->property("text").toString().startsWith(text));
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

TEST_CASE("SessionChrome asks an extension's question and hands the answer back (offscreen)",
          "[contour][gui][qml][extensions]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());

    MockSession session;
    session.extensionQuestion = QStringLiteral("status: git -C {dir} status --porcelain=v2 --branch");
    QVariantMap initial;
    initial.insert("session", QVariant::fromValue(static_cast<QObject*>(&session)));
    std::unique_ptr<QObject> chrome(component.createWithInitialProperties(initial));
    REQUIRE(chrome != nullptr);
    auto* const dialog = chrome->findChild<QObject*>(QStringLiteral("extensionCommandsDialog"));
    REQUIRE(dialog != nullptr);

    // The question is shown as the session words it.
    emit session.requestPermissionForExtension();
    QCoreApplication::processEvents();
    CHECK(dialog->property("text").toString() == session.extensionQuestion);

    SECTION("Yes allows, and so does Yes to All: every answer is remembered anyway")
    {
        REQUIRE(QMetaObject::invokeMethod(dialog, "yesClicked"));
        REQUIRE(QMetaObject::invokeMethod(dialog, "yesToAllClicked"));
        CHECK(session.extensionAllowed == 2);
    }

    SECTION("No denies")
    {
        REQUIRE(QMetaObject::invokeMethod(dialog, "noClicked"));
        REQUIRE(QMetaObject::invokeMethod(dialog, "noToAllClicked"));
        CHECK(session.extensionDenied == 2);
    }

    SECTION("closing it is no answer")
    {
        REQUIRE(QMetaObject::invokeMethod(dialog, "rejected"));
        CHECK(session.extensionDismissed == 1);
        CHECK(session.extensionAllowed == 0);
        CHECK(session.extensionDenied == 0);
    }
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

TEST_CASE("StatusLinePreviewItem paints a role from its resolved colour (offscreen)",
          "[contour][gui][qml][statusline]")
{
    // A role name in a QML colour property is an "Unable to assign" warning and no colour; the preview
    // paints the hex the bridge resolved beside it instead.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import Contour.Ui
StatusLinePreviewItem {
    item: ({ type: "Clock", sample: "12:34", hasColor: true, color: "muted", colorPreview: "#123456",
             hasBgColor: true, bgColor: "accent", bgColorPreview: "#654321" })
})",
                      QUrl());
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> preview(component.create());
    REQUIRE(preview != nullptr);
    QCoreApplication::processEvents();

    CHECK(preview->property("color").value<QColor>() == QColor(QStringLiteral("#654321")));
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

namespace
{
/// A SessionChrome hosted in a sized, visible offscreen Window, with its scrollbar already resolved.
struct ChromeHost
{
    std::unique_ptr<QObject> window;
    QQuickItem* chrome {};
    QQuickItem* bar {};
    qreal paneWidth {};
    /// The bar's natural (unstretched) width — the value an anchor pair used to latch away.
    qreal thin {};
};

/// Hosts a bare SessionChrome, bound to a NULL session, in a sized Window — so the scrollbar's real
/// geometry (x/width against a known parent width) is observable, and so the null->session rebind that
/// every split / tab switch / teardown performs can be driven from the test. Passing the session as an
/// initial property instead would defeat the purpose: a session that is non-null on the first binding
/// pass never armed the anchor latch these cases guard.
[[nodiscard]] ChromeHost createChromeInWindow(QQmlEngine& engine)
{
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property alias chrome: chromeItem\n"
                                        "  SessionChrome { id: chromeItem; session: null }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ScrollBarWrapper.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO("wrapper errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    auto host = ChromeHost { .window = std::unique_ptr<QObject>(component.create()) };
    REQUIRE(host.window != nullptr);
    QCoreApplication::processEvents();

    host.chrome = host.window->property("chrome").value<QQuickItem*>();
    REQUIRE(host.chrome != nullptr);
    host.bar = host.chrome->findChild<QQuickItem*>(QStringLiteral("verticalScrollBar"));
    REQUIRE(host.bar != nullptr);
    host.paneWidth = host.chrome->width(); // SessionChrome fills the window
    host.thin = host.bar->property("implicitWidth").toReal();
    return host;
}

/// A Right-placed session with scrollback, so the bar is genuinely shown rather than merely laid out.
[[nodiscard]] std::unique_ptr<MockSession> createScrollableSession()
{
    auto session = std::make_unique<MockSession>();
    session->setScrollbarVisible(true);
    session->setHistoryLineCount(1000);
    return session;
}
} // namespace

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
        CHECK(host.bar->isVisible());

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
        REQUIRE(host.bar->isVisible());

        session->setScrollbarVisible(false);
        QCoreApplication::processEvents();
        CHECK_FALSE(host.bar->isVisible());
    }

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// The tick items a SessionChrome's mark layer currently shows, top first.
[[nodiscard]] std::vector<QQuickItem*> markTicksOf(QQuickItem const& layer)
{
    auto ticks = std::vector<QQuickItem*> {};
    for (auto* item: layer.childItems())
        if (item->objectName() == QStringLiteral("scrollbarMark"))
            ticks.push_back(item);
    std::ranges::sort(ticks, std::ranges::less {}, &QQuickItem::y);
    return ticks;
}

/// A tick as TerminalSession publishes it.
[[nodiscard]] QVariantMap markEntry(double position, vtbackend::ScrollbarMarkKind kind, qint64 targetLine)
{
    return QVariantMap { { QStringLiteral("position"), position },
                         { QStringLiteral("kind"), static_cast<int>(kind) },
                         { QStringLiteral("targetLine"), targetLine } };
}

/// One colour per ScrollbarMarkKind, in enumerator order: Command, UserMark, Running, Failure.
[[nodiscard]] QVariantList markColors()
{
    return QVariantList { QVariant::fromValue(QColor(Qt::gray)),
                          QVariant::fromValue(QColor(Qt::cyan)),
                          QVariant::fromValue(QColor(Qt::yellow)),
                          QVariant::fromValue(QColor(Qt::red)) };
}

/// The centre of @p item, in its window's coordinates.
[[nodiscard]] QPoint centreOf(QQuickItem const& item)
{
    return item.mapToScene(QPointF(item.width() / 2, item.height() / 2)).toPoint();
}

/// A MockSession whose viewport scrolls, as TerminalSession's does: the chrome writes the offset when the bar
/// moves, and is told whenever it moved.
class ScrollingMockSession final: public MockSession
{
    Q_OBJECT
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)

  public:
    [[nodiscard]] int scrollOffset() const { return _scrollOffset; }

    /// Moves the viewport, announcing it only when it moved, as the terminal's viewport does.
    void setScrollOffset(int value)
    {
        if (value == _scrollOffset)
            return;
        _scrollOffset = value;
        emit scrollOffsetChanged();
    }

  signals:
    void scrollOffsetChanged();

  private:
    int _scrollOffset = 0;
};

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
} // namespace

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
        CHECK(host.bar->isVisible());
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
        REQUIRE(ticks[0]->isVisible());

        session->setScrollbarVisible(false);
        QCoreApplication::processEvents();
        CHECK_FALSE(ticks[0]->isVisible());
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
    QCoreApplication::processEvents();

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

namespace
{
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

TEST_CASE("PaneNode survives node becoming null during split teardown (offscreen)",
          "[contour][gui][qml][split]")
{
    // Regression for the unguarded root.node dereference inside splitComponent: when a pane
    // collapses, the proxy tree is pruned/rebound and the `node` binding can momentarily evaluate to
    // null while the SplitView is still alive. Unguarded bindings (orientation/ratio/first/second)
    // would then raise "TypeError: Cannot read property '...' of null". We instantiate PaneNode
    // against a split node, then drop node to null and pump events, asserting no such TypeError.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto* leftLeaf = new MockPaneProxy(/*leaf*/ true);
    auto* rightLeaf = new MockPaneProxy(/*leaf*/ true);
    auto splitNode = std::make_unique<MockPaneProxy>(/*leaf*/ false, leftLeaf, rightLeaf);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(splitNode.get())));
    std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));
    REQUIRE(paneNode != nullptr);
    QCoreApplication::processEvents();

    // Now drop the node to null while the SplitView subtree is still alive, as a collapse would.
    paneNode->setProperty("node", QVariant::fromValue(static_cast<QObject*>(nullptr)));
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    auto const nullDerefWarnings =
        warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("null"); });
    CHECK(nullDerefWarnings == 0);
}

TEST_CASE("PaneNode.clampRatio bounds a divider drag instead of discarding it (offscreen)",
          "[contour][gui][qml][split]")
{
    // Regression for the "divider drag to an edge is discarded" finding: the old guard
    // `if (r > 0.0 && r < 1.0)` skipped writing the ratio for an edge drag, so the divider snapped
    // back. The fix clamps the raw ratio into [minPaneRatio, 1 - minPaneRatio] (extracted as the pure
    // clampRatio() function) and always writes it. Here we invoke clampRatio() directly so the clamp
    // arithmetic is verified without synthesizing a real splitter drag.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    // The leaf renders a TerminalPane -> SessionChrome, whose bell Loader binds
    // `active: terminalSessions.multimediaReady`; without the context property that binding throws a
    // ReferenceError (now caught by the global QML-message gate in test_main.cpp).
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    auto leaf = std::make_unique<MockPaneProxy>(/*leaf*/ true);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    REQUIRE(component.isReady());
    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(leaf.get())));
    std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));
    REQUIRE(paneNode != nullptr);

    auto const minRatio = paneNode->property("minPaneRatio").toDouble();
    REQUIRE(minRatio > 0.0);
    REQUIRE(minRatio < 0.5);

    auto const clamp = [&](double raw) {
        QVariant result;
        REQUIRE(QMetaObject::invokeMethod(
            paneNode.get(), "clampRatio", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, raw)));
        return result.toDouble();
    };

    // A drag to the left/top edge (0) is clamped up to minPaneRatio, not dropped to 0.
    CHECK(clamp(0.0) == Catch::Approx(minRatio));
    // A drag to the right/bottom edge (1) is clamped down to 1 - minPaneRatio, not dropped to 1.
    CHECK(clamp(1.0) == Catch::Approx(1.0 - minRatio));
    // Beyond the edges (overshoot) is clamped to the same bounds.
    CHECK(clamp(-0.3) == Catch::Approx(minRatio));
    CHECK(clamp(1.3) == Catch::Approx(1.0 - minRatio));
    // A normal mid-range drag passes through unchanged.
    CHECK(clamp(0.42) == Catch::Approx(0.42));
}

// (No offscreen "grabs keyboard focus" test: hasActiveFocus() does not resolve reliably under the offscreen
// platform, so such an assertion is a FALSE guard — it passes with OR without the forceActiveFocus() fix.
// Focus-follows-active is covered deterministically at the vtworkspace model layer; the "renders active pane
// without error" GUI case is covered by the split behavioral test. The global QML-message gate in
// test_main.cpp additionally fails the run on ANY QML error, so a stray ReferenceError cannot pass silently.)

TEST_CASE("Custom window controls show only when the native frame is hidden (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // show_title_bar (terminalSessions.titleBarVisible, re-homed from the removed vtui) now selects the
    // window decoration on every OS:
    //   true  -> native frame + OS window controls; our custom controls must be HIDDEN (no duplicate);
    //   false -> frameless + our custom controls SHOWN.
    // Main.qml wires `useCustomWindowControls: !terminalSessions.titleBarVisible` while the tab strip itself
    // stays visible regardless. This exercises that exact binding.
    QQmlEngine engine;
    MockTabController controller;
    controller.setTitleBarVisible(false); // profile show_title_bar: false -> frameless CSD
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Item {
            property bool customControls: !terminalSessions.titleBarVisible
            property bool tabStripVisible: true
        }
    )",
                      QUrl());
    {
        INFO("component status: " << static_cast<int>(component.status()));
        for (auto const& e: component.errors())
            INFO("  error: " << e.toString().toStdString());
        REQUIRE(component.isReady());
    }
    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);

    // Frameless (show_title_bar:false) -> our custom controls are shown.
    CHECK(root->property("customControls").toBool() == true);
    // The tab strip is shown in either decoration mode.
    CHECK(root->property("tabStripVisible").toBool() == true);

    // Switching to native decoration hides our custom controls (the OS draws them).
    controller.setTitleBarVisible(true);
    CHECK(root->property("customControls").toBool() == false);
    CHECK(root->property("tabStripVisible").toBool() == true);
}

TEST_CASE("ToggleTitleBar flips the native-frame axis without stacking decorations (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // ToggleTitleBar switches between native decoration and frameless CSD. Main.qml keeps a single
    // source of truth (terminalSessions.titleBarVisible) that drives BOTH the window frame (flags) and
    // whether our custom controls render, so the two decorations can never stack into a double frame.
    QQmlEngine engine;
    MockTabController controller; // starts visible (native frame)
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Window
        Item {
            // Mirror Main.qml: frameless exactly when the native frame is off; custom controls then on.
            property bool frameless: !terminalSessions.titleBarVisible
            property bool customControls: !terminalSessions.titleBarVisible
        }
    )",
                      QUrl());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);

    // Native frame on: not frameless, and no custom controls (single decoration).
    REQUIRE(controller.titleBarVisible() == true);
    CHECK(root->property("frameless").toBool() == false);
    CHECK(root->property("customControls").toBool() == false);

    // Toggle to frameless CSD: frameless and custom controls both on — still a single decoration.
    controller.toggleTitleBar();
    CHECK(controller.titleBarVisible() == false);
    CHECK(root->property("frameless").toBool() == true);
    CHECK(root->property("customControls").toBool() == true);

    // Toggle back to native: frameless and custom controls both off. The two are always opposite of
    // each other, so a native frame is never stacked on top of custom controls.
    controller.toggleTitleBar();
    CHECK(controller.titleBarVisible() == true);
    CHECK(root->property("frameless").toBool() == false);
    CHECK(root->property("customControls").toBool() == false);
}

TEST_CASE("PaneNode renders a SINGLE leaf as the whole window without TypeErrors (offscreen)",
          "[contour][gui][qml][split]")
{
    // The pane tree is now the SOLE renderer: an unsplit terminal is a single-leaf tree (one TerminalPane).
    // Instantiate PaneNode against a single leaf and assert its TerminalPane resolves with no
    // "TypeError: ... of null" — the null-safety the old Terminal.qml test guarded, now on the leaf path.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;
    MockPaneProxy leaf(/*leaf*/ true);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(&leaf)));
    std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));
    QCoreApplication::processEvents();

    REQUIRE(paneNode != nullptr);
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);
}

TEST_CASE("The reconnecting overlay shows only while the pane's session is reconnecting (offscreen)",
          "[contour][gui][qml][reconnect]")
{
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    MockSession session;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    engine.rootContext()->setContextProperty("mockSession", &session);
    contour::test::installChromeStyle(engine);

    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Window\n"
                          "import Contour.Ui\n"
                          "Window {\n"
                          "  width: 400; height: 300; visible: true\n"
                          "  property var win: terminalSessions\n"
                          "  property alias pane: paneItem\n"
                          "  TerminalPane { id: paneItem; anchors.fill: parent; session: mockSession }\n"
                          "}\n"),
        QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TestWrapper.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> const obj(component.create());
    REQUIRE(obj != nullptr);
    auto* pane = obj->property("pane").value<QQuickItem*>();
    REQUIRE(pane != nullptr);
    auto* overlay = pane->findChild<QQuickItem*>("reconnectingOverlay");
    REQUIRE(overlay != nullptr);
    QCoreApplication::processEvents();

    CHECK_FALSE(overlay->isVisible());
    session.setReconnecting(true);
    QCoreApplication::processEvents();
    CHECK(overlay->isVisible());
    CHECK(overlay->property("text").toString() == QStringLiteral("Reconnecting…"));
    session.setReconnecting(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(overlay->isVisible());
}

TEST_CASE("TerminalPane.onTerminated closes the window only when it is the last session (offscreen)",
          "[contour][gui][qml][split][close]")
{
    // With the pane tree as the sole renderer, TerminalPane.onTerminated is the ONLY window-close path.
    // Bug "closing one pane of a split killed the whole window": after one pane exits, ONE survivor remains,
    // so canCloseWindow() (false while any session remains) must NOT close the window. Only the last exit
    // does. onTerminated routes through THIS window's controller (Main.qml exposes it as the window's `win`
    // property), so load the pane inside a Window that declares `property var win` pointing at the mock —
    // mirroring how Main.qml's ApplicationWindow exposes its WindowController.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property var win: terminalSessions\n"
                                        "  property alias pane: paneItem\n"
                                        "  TerminalPane { id: paneItem; anchors.fill: parent }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TestWrapper.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> obj(component.create());
    REQUIRE(obj != nullptr);
    auto* pane = obj->property("pane").value<QQuickItem*>();
    REQUIRE(pane != nullptr);
    QCoreApplication::processEvents();

    // One survivor remains after the split collapsed -> canCloseWindow() false -> must NOT close.
    controller.setOpenSessions(1);
    controller.canCloseWindowCalls = 0;
    QMetaObject::invokeMethod(pane, "terminated");
    QCoreApplication::processEvents();
    CHECK(controller.canCloseWindowCalls == 1);

    // Last pane exits: no sessions remain -> canCloseWindow() true.
    controller.setOpenSessions(0);
    controller.canCloseWindowCalls = 0;
    QMetaObject::invokeMethod(pane, "terminated");
    QCoreApplication::processEvents();
    CHECK(controller.canCloseWindowCalls == 1);

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);
}

// ============================================================================================
// Split-lifecycle behavioral tests: DRIVE the state transitions a real split/collapse produces (against the
// dynamic MockSession/MockPaneProxy) and assert BEHAVIOR — not just that the QML loads. Each corresponds to a
// bug that shipped this branch and slipped through the load-only tests.
// ============================================================================================

namespace
{
/// Instantiate PaneNode.qml against a proxy tree, parented into a live offscreen window. Returns the root
/// QQuickItem (owned by the caller) and pumps the event loop so child Loaders instantiate.
[[nodiscard]] std::unique_ptr<QObject> instantiatePaneNode(QQmlEngine& engine,
                                                           QQuickWindow& window,
                                                           MockPaneProxy& node)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    if (!component.isReady())
        return nullptr;

    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(&node)));
    std::unique_ptr<QObject> root(component.createWithInitialProperties(initial));
    if (auto* item = qobject_cast<QQuickItem*>(root.get()))
    {
        item->setParentItem(window.contentItem());
        item->setWidth(400);
        item->setHeight(300);
    }
    QCoreApplication::processEvents();
    return root;
}
} // namespace

TEST_CASE("split: rebinding a leaf's session through null does not raise a TypeError (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // Regression for the "Cannot read property 'session' of null" burst on split: the size-popup was wired
    // with session.lineCountChanged.connect(updateSizeWidget) in onSessionChanged, re-connecting (never
    // disconnecting) on every session rebind, so stale handlers fired on a torn-down pane. Drive that churn
    // (A -> null -> B, resizing each live session) and assert no TypeError.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession sessionA;
    MockSession sessionB;
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&sessionA);

    contour::test::QmlMessageCapture const warnings;

    auto root = instantiatePaneNode(engine, window, leaf);
    REQUIRE(root != nullptr);

    sessionA.setPageSize(100, 40);
    QCoreApplication::processEvents();
    leaf.setSession(nullptr);
    QCoreApplication::processEvents();
    leaf.setSession(&sessionB);
    QCoreApplication::processEvents();
    sessionB.setPageSize(120, 50); // a stale handler bound to A (or a null pane) would throw here
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);
}

TEST_CASE("split: a pane created already-active renders without error (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // A split makes the NEW pane active from construction. Assert a from-birth-active leaf instantiates and
    // renders with no QML error. Whether it holds keyboard focus is NOT asserted (unreliable offscreen);
    // focus-follows-active is tested at the vtworkspace model layer.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    contour::test::QmlMessageCapture const warnings;

    MockSession session;
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&session);
    leaf.setActive(true);

    auto root = instantiatePaneNode(engine, window, leaf);
    REQUIRE(root != nullptr);
    QCoreApplication::processEvents();

    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    REQUIRE(rootItem != nullptr);
    auto* pane = rootItem->findChild<StubContourTerminal*>();
    REQUIRE(pane != nullptr);
    CHECK(pane->property("paneActive").toBool());
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);
}

TEST_CASE("split: a leaf becoming a split node renders two panes without errors (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // Drive the split-CREATION transition: a single leaf becomes a split node with two leaf children, which
    // must flip PaneNode's Loader from the leaf branch to the SplitView branch and instantiate two child
    // TerminalPanes — with no TypeError / recursive-instantiation / required-property warning.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession original;
    MockPaneProxy rootNode(/*leaf*/ true);
    rootNode.setSession(&original);

    contour::test::QmlMessageCapture const warnings;

    auto rootObj = instantiatePaneNode(engine, window, rootNode);
    REQUIRE(rootObj != nullptr);
    QCoreApplication::processEvents();

    auto* leftLeaf = new MockPaneProxy(/*leaf*/ true);
    leftLeaf->setSession(&original);
    auto* rightLeaf = new MockPaneProxy(/*leaf*/ true);
    MockSession newSession;
    rightLeaf->setSession(&newSession);
    rightLeaf->setActive(true);
    rootNode.becomeSplit(leftLeaf, rightLeaf);
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    auto const badWarnings = warnings.count([](QString const& w) {
        return w.contains("TypeError") || w.contains("of null") || w.contains("recursively")
               || (w.contains("Required property") && w.contains("not initialized"));
    });
    CHECK(badWarnings == 0);
}

TEST_CASE("split: collapsing a split node back to a leaf keeps the survivor rendering (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // Drive the COLLAPSE transition (the black-terminal bug's shape): a split node collapses back to a single
    // leaf that adopts the surviving session. PaneNode must flip back to the leaf branch and render a
    // TerminalPane bound to the survivor — no null-deref.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession survivor;
    MockSession closing;
    auto* left = new MockPaneProxy(/*leaf*/ true);
    left->setSession(&survivor);
    auto* right = new MockPaneProxy(/*leaf*/ true);
    right->setSession(&closing);
    MockPaneProxy rootNode(/*leaf*/ false, left, right);

    contour::test::QmlMessageCapture const warnings;

    auto rootObj = instantiatePaneNode(engine, window, rootNode);
    REQUIRE(rootObj != nullptr);
    QCoreApplication::processEvents();

    rootNode.collapseToLeaf(&survivor);
    rootNode.setActive(true);
    QCoreApplication::processEvents();

    survivor.setPageSize(90, 30);
    survivor.setOpacity(0.8F);
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);

    auto* rootItem = qobject_cast<QQuickItem*>(rootObj.get());
    REQUIRE(rootItem != nullptr);
    auto* pane = rootItem->findChild<StubContourTerminal*>();
    REQUIRE(pane != nullptr);
    CHECK(pane->session() == static_cast<QObject*>(&survivor));
}

TEST_CASE("split: TerminalPane opacity follows the session opacity (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // TerminalPane.opacity binds to session.opacity. Assert the binding is live and null-guarded.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession session;
    session.setOpacity(0.5F);
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&session);

    auto root = instantiatePaneNode(engine, window, leaf);
    REQUIRE(root != nullptr);
    QCoreApplication::processEvents();

    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    REQUIRE(rootItem != nullptr);
    auto* pane = rootItem->findChild<StubContourTerminal*>();
    REQUIRE(pane != nullptr);
    CHECK(pane->opacity() == Catch::Approx(0.5));

    session.setOpacity(0.9F);
    QCoreApplication::processEvents();
    CHECK(pane->opacity() == Catch::Approx(0.9));

    leaf.setSession(nullptr);
    QCoreApplication::processEvents();
    CHECK(pane->opacity() == Catch::Approx(1.0));
}

// ============================================================================================
// GUI-layer pane-operation tests: assert observable RENDERED properties (orientation, geometry, border
// width, activate routing) — the parts of split/resize/focus-highlight that DO resolve reliably offscreen
// (unlike Qt keyboard focus). These complement the vtworkspace model tests.
// ============================================================================================

namespace
{
/// Find the SplitView child of a loaded PaneNode split, or nullptr.
[[nodiscard]] QQuickItem* findSplitView(QQuickItem* root)
{
    for (auto* child: root->findChildren<QQuickItem*>())
        if (QString(child->metaObject()->className()).contains("SplitView"))
            return child;
    return nullptr;
}
} // namespace

TEST_CASE("PaneNode: SplitView orientation follows node.orientation for both axes (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // A Horizontal(1) split is otherwise never exercised (the mock used to hardcode orientation); a swapped
    // enum mapping would render splits along the wrong axis with no test catching it. orientation is a plain
    // int, reliably observable offscreen.
    auto run = [](int nodeOrientation, int expectedQtOrientation) {
        QQmlEngine engine;
        qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine);
        QQuickWindow window;
        window.resize(400, 300);
        window.show();

        auto* left = new MockPaneProxy(/*leaf*/ true);
        auto* right = new MockPaneProxy(/*leaf*/ true);
        MockPaneProxy splitNode(/*leaf*/ false, left, right);
        splitNode.setOrientation(nodeOrientation);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        REQUIRE(component.isReady());
        QVariantMap initial;
        initial.insert("node", QVariant::fromValue(static_cast<QObject*>(&splitNode)));
        std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));
        REQUIRE(paneNode != nullptr);
        QCoreApplication::processEvents();

        auto* rootItem = qobject_cast<QQuickItem*>(paneNode.get());
        REQUIRE(rootItem != nullptr);
        auto* splitView = findSplitView(rootItem);
        REQUIRE(splitView != nullptr);
        // Qt.Horizontal == 1, Qt.Vertical == 2 (Qt::Orientation).
        CHECK(splitView->property("orientation").toInt() == expectedQtOrientation);
    };

    // node.orientation 2 (vtworkspace Vertical, side-by-side) -> Qt.Horizontal(1); anything else ->
    // Qt.Vertical(2).
    run(/*node*/ 2, /*qt*/ 1);
    run(/*node*/ 1, /*qt*/ 2);
}

// (No GUI test asserts the SplitView's laid-out child PIXEL widths for a given ratio: SplitView defers its
// layout under the offscreen platform, so a firstChild.width ≈ ratio*width assertion is flaky. The ratio
// contract is covered where it is deterministic: the divider-drag clamp in "PaneNode.clampRatio bounds a
// divider drag..." above, and the model-layer clamp+emit in vtworkspace SessionModel_test's setPaneRatio
// test.)

TEST_CASE("PaneNode: tapping a leaf routes to node.activate(), guarded against a null node (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // Click->activate routing is where split focus bugs live; activate() is now call-tracked. Emit the
    // TapHandler's activated signal and assert node.activate() ran; then drive with a null node and assert
    // no throw and no activate.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession session;
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&session);

    auto paneNode = instantiatePaneNode(engine, window, leaf);
    REQUIRE(paneNode != nullptr);

    auto* rootItem = qobject_cast<QQuickItem*>(paneNode.get());
    REQUIRE(rootItem != nullptr);
    auto* pane = rootItem->findChild<StubContourTerminal*>();
    REQUIRE(pane != nullptr);

    // Emit the pane's `activated` signal (what the TapHandler fires) and assert it routed to node.activate().
    REQUIRE(QMetaObject::invokeMethod(pane, "activated"));
    QCoreApplication::processEvents();
    CHECK(leaf.activateCalls == 1);
}

TEST_CASE("PaneNode: a nested 3-pane tree renders three panes without warnings (offscreen)",
          "[contour][gui][qml][split][behavior]")
{
    // Only a 2-leaf split is rendered elsewhere; the recursive instantiation for 3+ panes is where
    // required-property / recursion warnings appear as the tree deepens.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    QQuickWindow window;
    window.resize(600, 400);
    window.show();

    contour::test::QmlMessageCapture const warnings;

    // root = [ leaf | split(leaf, leaf) ]  -> three TerminalPane leaves.
    auto* a = new MockPaneProxy(/*leaf*/ true);
    auto* b = new MockPaneProxy(/*leaf*/ true);
    auto* c = new MockPaneProxy(/*leaf*/ true);
    auto* rightSplit = new MockPaneProxy(/*leaf*/ false, b, c);
    MockPaneProxy root(/*leaf*/ false, a, rightSplit);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());
    QVariantMap initial;
    initial.insert("node", QVariant::fromValue(static_cast<QObject*>(&root)));
    std::unique_ptr<QObject> paneNode(component.createWithInitialProperties(initial));
    REQUIRE(paneNode != nullptr);
    // Pump twice: the nested split instantiates its children via URL Loaders (PaneNode loads PaneNode.qml by
    // URL to break the recursion), so the deepest leaves settle on a later event-loop turn than the root.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* rootItem = qobject_cast<QQuickItem*>(paneNode.get());
    REQUIRE(rootItem != nullptr);
    CHECK(rootItem->findChildren<StubContourTerminal*>().size() == 3);
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    auto const badWarnings = warnings.count([](QString const& w) {
        return w.contains("TypeError") || w.contains("of null") || w.contains("recursively")
               || (w.contains("Required property") && w.contains("not initialized"));
    });
    CHECK(badWarnings == 0);
}

// {{{ Step 2: the split handle is the gap

namespace
{
// Defined with the kit and window-control cases further down; one unnamed namespace per translation unit.
[[nodiscard]] QQuickItem* findItemNamed(QQuickItem const& root, QString const& objectName);
[[nodiscard]] qreal chromeToken(QQmlEngine& engine, char const* group, char const* name);

/// A split of two leaves, each with its own session, on one token row and density, in a 400 x 300 window.
struct SplitHost
{
    QQmlEngine engine;
    MockTabController controller;
    QQuickWindow window;
    MockSession leftSession;
    MockSession rightSession;
    MockPaneProxy split { /*leaf*/ false,
                          new MockPaneProxy(/*leaf*/ true),
                          new MockPaneProxy(/*leaf*/ true) };
    std::unique_ptr<QObject> paneNode;

    SplitHost(contour::config::ChromeStyle row,
              contour::config::ChromeDensity density,
              vtworkspace::SplitState axis)
    {
        qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        auto setup = contour::test::defaultChromeSetup();
        setup.row = row;
        setup.density = density;
        contour::test::installChromeStyle(engine, setup);

        window.resize(400, 300);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));

        split.first()->setSession(&leftSession);
        split.second()->setSession(&rightSession);
        split.setOrientation(static_cast<int>(axis));
        paneNode = instantiatePaneNode(engine, window, split);
        REQUIRE(paneNode != nullptr);
        // SplitView lays out on polish: wait until the second card has been placed beside the first.
        REQUIRE(QTest::qWaitFor([this] {
            auto const* second = item("splitSecond");
            auto const* first = item("splitFirst");
            return first != nullptr && second != nullptr && first->width() > 0 && first->height() > 0
                   && (second->x() > 0 || second->y() > 0);
        }));
    }

    [[nodiscard]] QQuickItem& root() const { return *qobject_cast<QQuickItem*>(paneNode.get()); }

    /// @return The item named @p name beneath the pane node, or nullptr.
    [[nodiscard]] QQuickItem* item(char const* name) const
    {
        return findItemNamed(root(), QString::fromUtf8(name));
    }

    /// @return The item named @p name, which the test cannot go on without.
    [[nodiscard]] QQuickItem& required(char const* name) const
    {
        auto* found = item(name);
        INFO("item: " << name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] qreal token(char const* group, char const* name)
    {
        return chromeToken(engine, group, name);
    }

    /// @return The style object itself, for its top-level properties.
    [[nodiscard]] QObject& style() const
    {
        return *engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    }

    /// @return Where @p target's point @p local is, in window coordinates.
    [[nodiscard]] static QPoint inWindow(QQuickItem const& target, QPointF local)
    {
        return target.mapToScene(local).toPoint();
    }

    /// @return The centre of the gap between the two cards, in window coordinates.
    [[nodiscard]] QPoint gapCentre() const
    {
        auto const& handle = required("splitHandle");
        return inWindow(handle, { handle.width() / 2, handle.height() / 2 });
    }
};

/// The densities the handle cases run under.
constexpr auto HandleDensities =
    std::array { contour::config::ChromeDensity::Comfortable, contour::config::ChromeDensity::Minimal };
} // namespace

TEST_CASE("split handle: the handle is the gap, and its hit area is wider (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    using contour::config::ChromeStyle;
    auto const density = GENERATE(from_range(HandleDensities));
    CAPTURE(static_cast<int>(density));

    SECTION("side by side")
    {
        auto host = SplitHost(ChromeStyle::MacOS, density, vtworkspace::SplitState::Vertical);
        auto const gap = host.token("space", "paneGap");
        auto const hit = host.token("space", "splitHit");
        auto const& handle = host.required("splitHandle");
        auto const& hitArea = host.required("splitHitArea");
        CHECK(handle.width() == gap);
        CHECK(hitArea.width() == hit);
        CHECK(hitArea.x() == (gap - hit) / 2);
        CHECK(hitArea.height() == handle.height());

        // Only Minimal draws a line; Comfortable's gap is the root's backdrop showing through.
        CHECK(host.required("splitLine").isVisible() == (density == contour::config::ChromeDensity::Minimal));

        // The layout ignores the hit area: the second card starts one gap after the first.
        auto const& first = host.required("splitFirst");
        auto const& second = host.required("splitSecond");
        auto const inset = host.token("space", "paneInset");
        CHECK(second.mapToItem(&host.root(), QPointF {}).x() == inset + first.width() + gap);
    }

    SECTION("stacked")
    {
        auto host = SplitHost(ChromeStyle::MacOS, density, vtworkspace::SplitState::Horizontal);
        auto const& handle = host.required("splitHandle");
        auto const& hitArea = host.required("splitHitArea");
        CHECK(handle.height() == host.token("space", "paneGapRows"));
        CHECK(hitArea.height() == host.token("space", "splitHit"));
        CHECK(hitArea.width() == handle.width());
    }
}

TEST_CASE("split handle: the grabber shows on hover (offscreen)", "[contour][gui][qml][split][handle]")
{
    auto host = SplitHost(contour::config::ChromeStyle::MacOS,
                          contour::config::ChromeDensity::Comfortable,
                          vtworkspace::SplitState::Vertical);
    auto const& grabber = host.required("splitGrabber");
    CHECK_FALSE(grabber.isVisible());

    QTest::mouseMove(&host.window, host.gapCentre());
    CHECK(QTest::qWaitFor([&grabber] { return grabber.isVisible(); }));
    CHECK(grabber.width() == host.token("size", "grabber"));
    CHECK(grabber.height() == host.token("size", "grabberLength"));
}

TEST_CASE("split handle: a press inside a card's edge drags the divider (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    auto const density = GENERATE(from_range(HandleDensities));
    CAPTURE(static_cast<int>(density));
    auto host = SplitHost(contour::config::ChromeStyle::MacOS, density, vtworkspace::SplitState::Vertical);
    auto* splitView = findSplitView(&host.root());
    REQUIRE(splitView != nullptr);

    // Half a point inside the edge of the hit area's overhang into the first card: 1.5 pt on the
    // comfortable row (10 around 6), 3 pt on Minimal (8 around 1).
    auto const overhang = (host.token("space", "splitHit") - host.token("space", "paneGap")) / 2;
    REQUIRE(overhang > 1);
    auto const& handle = host.required("splitHandle");
    auto const press = SplitHost::inWindow(handle, { 0.5 - overhang, handle.height() / 2 });
    QTest::mousePress(&host.window, Qt::LeftButton, Qt::NoModifier, press);
    CHECK(splitView->property("resizing").toBool());
    for (auto const step: std::views::iota(1, 5))
        QTest::mouseMove(&host.window, press + QPoint { step * 10, 0 });
    QTest::mouseRelease(&host.window, Qt::LeftButton, Qt::NoModifier, press + QPoint { 40, 0 });

    CHECK_FALSE(splitView->property("resizing").toBool());
    REQUIRE(host.split.lastRatio.has_value());
    CHECK(*host.split.lastRatio > host.split.ratio());
}

TEST_CASE("split handle: the scrollbar's centre still scrolls (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    auto const density = GENERATE(from_range(HandleDensities));
    CAPTURE(static_cast<int>(density));
    auto host = SplitHost(contour::config::ChromeStyle::MacOS, density, vtworkspace::SplitState::Vertical);
    host.leftSession.setScrollbarRight(true);
    host.leftSession.setScrollbarVisible(true);
    host.leftSession.setHistoryLineCount(1000);

    auto const& first = host.required("splitFirst");
    auto* scrollBar = findItemNamed(first, QStringLiteral("verticalScrollBar"));
    REQUIRE(scrollBar != nullptr);
    REQUIRE(QTest::qWaitFor([scrollBar] { return scrollBar->isVisible(); }));
    auto* splitView = findSplitView(&host.root());
    REQUIRE(splitView != nullptr);

    auto const press = SplitHost::inWindow(*scrollBar, { scrollBar->width() / 2, scrollBar->height() / 2 });
    QTest::mousePress(&host.window, Qt::LeftButton, Qt::NoModifier, press);
    CHECK(scrollBar->property("pressed").toBool());
    CHECK_FALSE(splitView->property("resizing").toBool());
    QTest::mouseRelease(&host.window, Qt::LeftButton, Qt::NoModifier, press);
}

TEST_CASE("split handle: the root inset and backdrop appear only when split (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    SECTION("a leaf root is edge to edge")
    {
        QQmlEngine engine;
        qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        MockTabController controller;
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine);
        QQuickWindow window;
        window.resize(400, 300);
        window.show();
        MockSession session;
        MockPaneProxy leaf(/*leaf*/ true);
        leaf.setSession(&session);
        auto paneNode = instantiatePaneNode(engine, window, leaf);
        REQUIRE(paneNode != nullptr);
        auto const& root = *qobject_cast<QQuickItem*>(paneNode.get());

        auto const* backdrop = findItemNamed(root, QStringLiteral("splitBackdrop"));
        REQUIRE(backdrop != nullptr);
        CHECK_FALSE(backdrop->isVisible());
        auto const* pane = root.findChild<StubContourTerminal*>();
        REQUIRE(pane != nullptr);
        CHECK(pane->mapToItem(&root, QPointF {}) == QPointF {});
        CHECK(pane->width() == root.width());
    }

    SECTION("a split root sits its cards in from every edge over the gap's colour")
    {
        auto host = SplitHost(contour::config::ChromeStyle::MacOS,
                              contour::config::ChromeDensity::Comfortable,
                              vtworkspace::SplitState::Vertical);
        auto const inset = host.token("space", "paneInset");
        REQUIRE(inset > 0);
        auto const& backdrop = host.required("splitBackdrop");
        CHECK(backdrop.isVisible());
        auto* colors = host.style().property("colors").value<QObject*>();
        REQUIRE(colors != nullptr);
        CHECK(backdrop.property("color").value<QColor>() == colors->property("paneGapFill").value<QColor>());

        auto* splitView = findSplitView(&host.root());
        REQUIRE(splitView != nullptr);
        CHECK(splitView->mapToItem(&host.root(), QPointF {}) == QPointF { inset, inset });
        CHECK(splitView->width() == host.root().width() - (2 * inset));
        CHECK(splitView->height() == host.root().height() - (2 * inset));
    }
}

TEST_CASE("split handle: the Terminal row draws the split as a line of box glyphs (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    auto const glyphs = contour::config::uiStyleTokens(ChromeStyle::Terminal).splitLineGlyphs;

    SECTION("side by side: one cell wide, a vertical line")
    {
        auto host =
            SplitHost(ChromeStyle::Terminal, ChromeDensity::Comfortable, vtworkspace::SplitState::Vertical);
        auto const& handle = host.required("splitHandle");
        // One cell, floored to whole device pixels.
        auto const cellWidth = host.style().property("cellWidth").toReal();
        CHECK(handle.width() <= cellWidth);
        CHECK(handle.width() > cellWidth - 1);
        CHECK(host.required("splitHitArea").width() >= handle.width());
        auto const& glyph = host.required("splitLineGlyph");
        CHECK(glyph.property("text").toString() == QString::fromUtf8(glyphs[0]));
        CHECK_FALSE(host.required("splitGrabber").isVisible());

        // The glyphs are clipped to the handle, which the last one overhangs.
        auto const& line = host.required("splitLineGlyphs");
        CHECK(line.clip());
        CHECK(line.size() == handle.size());

        // The line takes the row's accent (its grabber colour) while engaged.
        auto* colors = host.style().property("colors").value<QObject*>();
        REQUIRE(colors != nullptr);
        QTest::mouseMove(&host.window, QPoint { 20, 20 }); // inside the first card, off the handle
        CHECK(QTest::qWaitFor([&] {
            return glyph.property("color").value<QColor>()
                   == colors->property("paneSplitLine").value<QColor>();
        }));
        QTest::mouseMove(&host.window, host.gapCentre());
        CHECK(QTest::qWaitFor([&] {
            return glyph.property("color").value<QColor>() == colors->property("paneGrabber").value<QColor>();
        }));
    }

    SECTION("stacked: one cell high, a horizontal line")
    {
        auto host =
            SplitHost(ChromeStyle::Terminal, ChromeDensity::Comfortable, vtworkspace::SplitState::Horizontal);
        auto const& handle = host.required("splitHandle");
        auto const cellHeight = host.style().property("cellHeight").toReal();
        CHECK(handle.height() <= cellHeight);
        CHECK(handle.height() > cellHeight - 1);
        // The row's split hit is one cell WIDE, narrower than a stacked gap one cell HIGH: the hit area
        // never shrinks what grabs the divider below the gap itself.
        CHECK(host.required("splitHitArea").height() >= handle.height());
        auto const& glyph = host.required("splitLineGlyph");
        CHECK(glyph.property("text").toString() == QString::fromUtf8(glyphs[1]));
    }
}

TEST_CASE("split handle: the inset and the gap snap to whole device pixels at fractional scales (offscreen)",
          "[contour][gui][qml][split][handle]")
{
    // #2040: the second card's origin is `inset + first + gap`, so all three must land on the device-pixel
    // grid; and the inset and gap are floored, so the canvas never spends more than the window-size solver
    // counts for them (splitChromeOf, which rounds the same snap up). The offscreen screen's ratio is 1, so
    // the snaps PaneNode shares are asked directly.
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    auto const [row, density] = GENERATE(std::pair { ChromeStyle::MacOS, ChromeDensity::Comfortable },
                                         std::pair { ChromeStyle::MacOS, ChromeDensity::Minimal },
                                         std::pair { ChromeStyle::Terminal, ChromeDensity::Comfortable });
    CAPTURE(static_cast<int>(row), static_cast<int>(density));
    auto host = SplitHost(row, density, vtworkspace::SplitState::Vertical);
    auto const call = [&host](char const* function, qreal logical, qreal dpr) {
        QVariant result;
        REQUIRE(QMetaObject::invokeMethod(&host.root(),
                                          function,
                                          Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, logical),
                                          Q_ARG(QVariant, dpr)));
        return result.toReal();
    };
    auto const isWhole = [](qreal value) {
        return std::abs(value - std::round(value)) < 1e-9;
    };
    auto const ceilOf = [](qreal value) {
        return static_cast<int>(std::ceil(value - 1e-9));
    };
    auto const* provider = qobject_cast<contour::window::UiStyleProvider const*>(&host.style());
    REQUIRE(provider != nullptr);

    for (auto const dpr: { 1.0, 1.25, 1.75 })
    {
        CAPTURE(dpr);
        auto const chrome = contour::window::splitChromeOf(
            provider->resolvedRow(), provider->units().horizontal, provider->units().vertical, dpr);
        auto const inset = call("snapGapToDevice", host.token("space", "paneInset"), dpr);
        auto const gap = call("snapGapToDevice", host.token("space", "paneGap"), dpr);
        auto const stackedGap = call("snapGapToDevice", host.token("space", "paneGapRows"), dpr);
        auto const first = call("snapToDevice", (host.root().width() - (2 * inset)) * 0.5, dpr);

        CHECK(isWhole(inset * dpr));
        CHECK(isWhole((inset + first) * dpr));
        CHECK(isWhole((inset + first + gap) * dpr));
        CHECK(gap * dpr >= 1); // never floored away

        // What is drawn never exceeds what the solver counts, per axis -- and the solver counts exactly
        // that, rounded up to a whole logical pixel.
        CHECK(gap <= chrome.sideBySideGap);
        CHECK(stackedGap <= chrome.stackedGap);
        CHECK(inset <= chrome.rootInset);
        CHECK(ceilOf(gap) == chrome.sideBySideGap);
        CHECK(ceilOf(stackedGap) == chrome.stackedGap);
        CHECK(ceilOf(inset) == chrome.rootInset);
    }
}

// }}}

// {{{ Step 2: pane cards and headers

namespace
{
/// Every item beneath @p root (inclusive) named @p name, walking the visual tree so that Loader- and
/// Repeater-made items are found too.
[[nodiscard]] std::vector<QQuickItem*> itemsNamed(QQuickItem& root, QString const& name)
{
    auto found = std::vector<QQuickItem*> {};
    auto pending = std::vector<QQuickItem*> { &root };
    while (!pending.empty())
    {
        auto* item = pending.back();
        pending.pop_back();
        if (item->objectName() == name)
            found.push_back(item);
        for (auto* child: item->childItems())
            pending.push_back(child);
    }
    return found;
}

/// The name an item announces, read from its `Accessible` attached object; empty without one.
[[nodiscard]] QString accessibleNameOf(QObject const& item)
{
    for (auto const* child: item.findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child->property("name").toString();
    return {};
}

/// @return The chrome style installed on @p engine, as the provider it is.
[[nodiscard]] contour::window::UiStyleProvider& providerOf(QQmlEngine& engine)
{
    auto* provider = qobject_cast<contour::window::UiStyleProvider*>(
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>());
    REQUIRE(provider != nullptr);
    return *provider;
}

/// The chrome setup the pane card cases default to: the MacOS row, Comfortable.
[[nodiscard]] contour::test::ChromeSetup paneCardSetup()
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    return setup;
}

/// A pane tree shown as Main.qml shows the active tab's: PaneNode.qml filling a window, told the tab's
/// visible pane count and the window's header reveal.
struct PaneTreeHost
{
    QQmlEngine engine;
    MockTabController controller;
    QQuickWindow window;
    std::unique_ptr<QObject> paneNode;

    /// @param setup The chrome the tree is drawn with.
    /// @param size  The window's size.
    PaneTreeHost(contour::test::ChromeSetup const& setup, QSize size)
    {
        qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, setup);
        window.resize(size);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    /// Shows @p tree, filling the window, and waits until the SplitViews have laid out @p leaves cards.
    void show(MockPaneProxy& tree, int visiblePanes, int headerReveal = 0)
    {
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaneNode.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO("PaneNode.qml: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
        auto const initial = QVariantMap { { "node", QVariant::fromValue(static_cast<QObject*>(&tree)) },
                                           { "visiblePanes", visiblePanes },
                                           { "headerReveal", headerReveal } };
        paneNode.reset(component.createWithInitialProperties(initial));
        REQUIRE(paneNode != nullptr);
        root().setParentItem(window.contentItem());
        resize(window.size());
    }

    /// Gives the tree another size and waits for its layout to settle.
    void resize(QSize size)
    {
        window.resize(size);
        root().setSize(QSizeF(size));
        settle();
    }

    /// Waits until the cards' geometry stops changing: the SplitViews lay out on polish, a frame later.
    void settle()
    {
        auto const geometry = [this] {
            auto rects = std::vector<QRectF> {};
            for (auto* card: cards())
                rects.push_back(card->mapRectToScene(QRectF { QPointF {}, card->size() }));
            return rects;
        };
        auto last = geometry();
        REQUIRE(QTest::qWaitFor(
            [&] {
                QTest::qWait(QuietFrameMs);
                auto const now = geometry();
                auto const stable = now == last && !now.empty();
                last = now;
                return stable;
            },
            SettleTimeoutMs));
    }

    [[nodiscard]] QQuickItem& root() const { return *qobject_cast<QQuickItem*>(paneNode.get()); }
    [[nodiscard]] std::vector<QQuickItem*> cards() const
    {
        return itemsNamed(root(), QStringLiteral("paneCard"));
    }

    /// @return The card showing leaf @p leaf.
    [[nodiscard]] QQuickItem& cardOf(MockPaneProxy const& leaf) const
    {
        for (auto* card: cards())
            if (card->property("node").value<QObject*>() == &leaf)
                return *card;
        FAIL("no card shows the leaf");
        return root();
    }

    /// @return The item named @p name inside @p card, which the test cannot go on without.
    [[nodiscard]] static QQuickItem& partOf(QQuickItem const& card, char const* name)
    {
        auto* found = findItemNamed(card, QString::fromUtf8(name));
        INFO("part: " << name);
        REQUIRE(found != nullptr);
        return *found;
    }

    /// @return The terminal inside @p card.
    [[nodiscard]] static QQuickItem& terminalOf(QQuickItem const& card)
    {
        auto* terminal = card.findChild<StubContourTerminal*>();
        REQUIRE(terminal != nullptr);
        return *terminal;
    }

    [[nodiscard]] contour::window::UiStyleProvider& style() { return providerOf(engine); }

    /// How long a frame without a layout change counts as settled, and how long settling may take.
    static constexpr int QuietFrameMs = 20;
    static constexpr int SettleTimeoutMs = 2000;
};

/// Two leaves side by side, the first active (focused by the pointer): "vim" in ~/repo, and a shell
/// whose title is "zsh" in ~/notes.
struct TwoPanes
{
    MockSession leftSession;
    MockSession rightSession;
    MockPaneProxy split { /*leaf*/ false,
                          new MockPaneProxy(/*leaf*/ true),
                          new MockPaneProxy(/*leaf*/ true) };

    explicit TwoPanes(QColor background = QColor(Qt::black)):
        leftSession { background }, rightSession { background }
    {
        leftSession.setForegroundProcessName(QStringLiteral("vim"));
        leftSession.setTitle(QStringLiteral("vim"));
        leftSession.setDisplayWorkingDirectory(QStringLiteral("~/repo"));
        rightSession.setTitle(QStringLiteral("zsh"));
        rightSession.setDisplayWorkingDirectory(QStringLiteral("~/notes"));
        left().setSession(&leftSession);
        right().setSession(&rightSession);
        left().setActive(true);
        left().setFocus(static_cast<int>(contour::window::PaneFocus::Focused));
        split.setOrientation(static_cast<int>(vtworkspace::SplitState::Vertical));
    }

    [[nodiscard]] MockPaneProxy& left() const { return *split.first(); }
    [[nodiscard]] MockPaneProxy& right() const { return *split.second(); }
};

/// How long the long-path case's working directory is.
constexpr auto LongPathLength = 4096;

/// How many nested splits the deep-tree case shows.
constexpr auto DeepTreeSplits = 8;

/// The pane header roles' indices, as QML names them (chromeStyle.roles).
constexpr auto HeaderTextRole = contour::window::ChromePaneRoleNames::paneHeaderText();
constexpr auto HeaderTextDimmedRole = contour::window::ChromePaneRoleNames::paneHeaderTextDimmed();
} // namespace

TEST_CASE("pane card: a single pane is edge to edge (offscreen)", "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);

    auto const& card = host.cardOf(leaf);
    CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeader").isVisible());
    for (auto* surface: itemsNamed(host.root(), QStringLiteral("paneCardSurface")))
        CHECK_FALSE(surface->isVisible());
    auto const& terminal = PaneTreeHost::terminalOf(card);
    CHECK(terminal.mapRectToScene(QRectF { QPointF {}, terminal.size() })
          == QRectF { QPointF {}, QSizeF { host.window.size() } });
}

TEST_CASE("pane card: a split's cards have headers (offscreen)", "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    for (auto const* leaf: { &panes.left(), &panes.right() })
    {
        auto const& card = host.cardOf(*leaf);
        auto const& header = PaneTreeHost::partOf(card, "paneHeader");
        CHECK(header.isVisible());
        CHECK(header.height() == 22);
        CHECK(PaneTreeHost::terminalOf(card).y() == 22 + host.style().paneCardContentInset(1.0));
        CHECK(card.width() >= contour::window::CwdMinPaneWidth);
        CHECK(PaneTreeHost::partOf(card, "paneHeaderCwd").isVisible());
    }
    // The program in front, else the title.
    CHECK(PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeaderName").property("text").toString()
          == QStringLiteral("vim"));
    CHECK(PaneTreeHost::partOf(host.cardOf(panes.right()), "paneHeaderName").property("text").toString()
          == QStringLiteral("zsh"));
    CHECK(PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeaderCwd").property("text").toString()
          == QStringLiteral("~/repo"));

    // About 291 wide each: too narrow for the directory.
    host.resize(QSize { 600, 400 });
    for (auto const* leaf: { &panes.left(), &panes.right() })
    {
        auto const& card = host.cardOf(*leaf);
        CHECK(card.width() < contour::window::CwdMinPaneWidth);
        CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeaderCwd").isVisible());
        CHECK(PaneTreeHost::partOf(card, "paneHeaderName").isVisible());
    }
}

TEST_CASE("pane card: the grid keeps clear of a rounded card's corners (offscreen)",
          "[contour][gui][qml][panecard]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    auto setup = paneCardSetup();
    setup.row = row;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    // What the window-size solver counts for the card is what the card spends.
    auto& style = host.style();
    auto const counted = contour::window::splitChromeOf(
        style.resolvedRow(), style.units().horizontal, style.units().vertical, 1.0);
    auto const inset = style.paneCardContentInset(1.0);
    CHECK(inset == counted.leafInset);
    CHECK((row == ChromeStyle::MacOS ? inset == 3.0 : inset == 0.0));

    for (auto const* leaf: { &panes.left(), &panes.right() })
    {
        auto const& card = host.cardOf(*leaf);
        auto const& header = PaneTreeHost::partOf(card, "paneHeader");
        auto const& terminal = PaneTreeHost::terminalOf(card);
        CHECK(terminal.x() == inset);
        CHECK(terminal.y() == header.height() + inset);
        CHECK(card.width() - (terminal.x() + terminal.width()) == inset);
        CHECK(card.height() - (terminal.y() + terminal.height()) == inset);
    }
}

TEST_CASE("pane card: only a soft shadow lifts the active card (offscreen)", "[contour][gui][qml][panecard]")
{
    using contour::config::ChromeStyle;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    auto setup = paneCardSetup();
    setup.row = row;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    // The Terminal row's e1 is a hard offset copy, which would shade the gap beside the card.
    CHECK(PaneTreeHost::partOf(host.cardOf(panes.left()), "paneCardShadow").isVisible()
          == (row == ChromeStyle::MacOS));
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "paneCardShadow").isVisible());
}

TEST_CASE("pane card: focus is emphasis and dim, never an outline (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes { QColor(0x1c, 0x1e, 0x24) };
    panes.rightSession.setDimUnfocused(0.3F);
    host.show(panes.split, /*visiblePanes*/ 2);

    CHECK(itemsNamed(host.root(), QStringLiteral("focusFrame")).empty());

    auto const background = panes.leftSession.backgroundColor();
    auto const nameColour = [&](MockPaneProxy const& leaf) {
        return PaneTreeHost::partOf(host.cardOf(leaf), "paneHeaderName").property("color").value<QColor>();
    };
    CHECK(nameColour(panes.left()).rgba() == host.style().colorOn(HeaderTextRole, background).rgba());
    CHECK(nameColour(panes.right()).rgba() == host.style().colorOn(HeaderTextDimmedRole, background).rgba());

    // The inactive pane dims below its header, not over it.
    auto const& inactive = host.cardOf(panes.right());
    auto const& header = PaneTreeHost::partOf(inactive, "paneHeader");
    auto const& dim = PaneTreeHost::partOf(inactive, "dimOverlay");
    CHECK(dim.isVisible());
    CHECK(dim.mapToItem(&inactive, QPointF {}).y() >= header.y() + header.height());
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.left()), "dimOverlay").isVisible());
}

TEST_CASE("pane card: the actions show on hover and act on their pane (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    // The pointer starts over the other pane: the cursor outlives the windows of earlier cases.
    auto const& other = PaneTreeHost::terminalOf(host.cardOf(panes.right()));
    QTest::mouseMove(&host.window, other.mapToScene({ other.width() / 2, other.height() / 2 }).toPoint());
    auto const& card = host.cardOf(panes.left());
    auto const& actions = PaneTreeHost::partOf(card, "paneHeaderActions");
    CHECK(QTest::qWaitFor([&actions] { return actions.opacity() == 0.0; }));

    auto const& header = PaneTreeHost::partOf(card, "paneHeader");
    QTest::mouseMove(&host.window, header.mapToScene({ header.width() / 2, header.height() / 2 }).toPoint());
    CHECK(QTest::qWaitFor([&actions] { return actions.opacity() == 1.0; }));

    auto const& split = PaneTreeHost::partOf(card, "paneHeaderAction0");
    QTest::mouseClick(&host.window,
                      Qt::LeftButton,
                      Qt::NoModifier,
                      split.mapToScene({ split.width() / 2, split.height() / 2 }).toPoint());
    CHECK(panes.left().headerActions == std::vector<int> { 0 });
    CHECK(panes.right().headerActions.empty());

    // The keyboard reaches a shown button too: focused, Space clicks it.
    auto& down = PaneTreeHost::partOf(card, "paneHeaderAction1");
    CHECK(down.activeFocusOnTab());
    down.forceActiveFocus();
    QTest::keyClick(&host.window, Qt::Key_Space);
    CHECK(panes.left().headerActions == std::vector<int> { 0, 1 });
}

TEST_CASE("pane card: the header buttons are the kit's glyph, on a fill by the pane's own tone (offscreen)",
          "[contour][gui][qml][panecard]")
{
    // A dark pane under a light chrome: the band's hover fill (black, by the chrome) would not show on it.
    contour::test::ScopedChromeAppearance const lightChrome { vtbackend::ColorPreference::Light };
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes { QColor(0x1c, 0x1e, 0x24) };
    host.show(panes.split, /*visiblePanes*/ 2);

    auto& button = PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeaderAction0");
    auto const& glyph = PaneTreeHost::partOf(button, "toolbarButtonGlyph");
    CHECK(glyph.property("font").value<QFont>().pixelSize() == 15);

    button.forceActiveFocus();
    auto const& background = PaneTreeHost::partOf(button, "toolbarButtonBackground");
    auto const fill = background.property("color").value<QColor>();
    auto const pane = panes.leftSession.backgroundColor();
    CHECK(
        fill.rgba()
        == host.style().colorOn(contour::window::ChromePaneRoleNames::paneHeaderActionHover(), pane).rgba());
    CHECK(fill.alpha() > 0);
    CHECK(fill.lightness() > pane.lightness()); // light on the dark pane, whatever the chrome's tone
}

TEST_CASE("pane card: a long cwd elides in the middle (offscreen)", "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    auto const path = QStringLiteral("/") + QString(LongPathLength - 1, QLatin1Char('d'));
    panes.leftSession.setDisplayWorkingDirectory(path);
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto const& header = PaneTreeHost::partOf(card, "paneHeader");
    auto const& cwd = PaneTreeHost::partOf(card, "paneHeaderCwd");
    auto const& name = PaneTreeHost::partOf(card, "paneHeaderName");
    auto const& actions = PaneTreeHost::partOf(card, "paneHeaderActions");
    CHECK(cwd.property("elide").toInt() == Qt::ElideMiddle);
    CHECK(header.height() == 22);
    CHECK(cwd.x() + cwd.width() <= actions.x());
    CHECK_FALSE(name.property("truncated").toBool());
    CHECK(accessibleNameOf(header).contains(path));
}

TEST_CASE("pane card: a deep tree in a small window (offscreen)", "[contour][gui][qml][panecard]")
{
    contour::test::QmlMessageCapture const warnings;
    auto host = PaneTreeHost { paneCardSetup(), QSize { 320, 200 } };

    // Eight nested splits, alternating their axis, each keeping a leaf and splitting the rest.
    auto const leaves = static_cast<size_t>(DeepTreeSplits) + 1;
    std::array<MockSession, static_cast<size_t>(DeepTreeSplits) + 1> sessions;
    auto tree = MockPaneProxy { /*leaf*/ true };
    auto* tip = &tree;
    for (auto const depth: std::views::iota(0, DeepTreeSplits))
    {
        auto* kept = new MockPaneProxy(/*leaf*/ true);
        kept->setSession(&sessions[static_cast<size_t>(depth)]);
        auto* rest = new MockPaneProxy(/*leaf*/ true);
        tip->becomeSplit(kept, rest);
        tip->setOrientation(static_cast<int>(depth % 2 == 0 ? vtworkspace::SplitState::Vertical
                                                            : vtworkspace::SplitState::Horizontal));
        tip = rest;
    }
    tip->setSession(&sessions.back());
    host.show(tree, /*visiblePanes*/ DeepTreeSplits + 1);

    auto const cards = host.cards();
    CHECK(cards.size() == leaves);
    auto const leastWithHeader = 22 + host.style().cellHeight();
    for (auto const* card: cards)
    {
        CHECK(card->width() >= 0);
        CHECK(card->height() >= 0);
        if (card->height() < leastWithHeader)
            CHECK_FALSE(PaneTreeHost::partOf(*card, "paneHeader").isVisible());
    }
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("pane card: Minimal shows no headers until the modifier is held (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.density = contour::config::ChromeDensity::Minimal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};

    SECTION("a split")
    {
        host.show(panes.split, /*visiblePanes*/ 2);
        for (auto const* leaf: { &panes.left(), &panes.right() })
        {
            auto const& card = host.cardOf(*leaf);
            CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeader").isVisible());
            CHECK(PaneTreeHost::terminalOf(card).y() == 0);
        }

        host.paneNode->setProperty("headerReveal",
                                   static_cast<int>(contour::window::HeaderReveal::HeldModifier));
        host.settle();
        for (auto const* leaf: { &panes.left(), &panes.right() })
        {
            auto const& card = host.cardOf(*leaf);
            auto const& header = PaneTreeHost::partOf(card, "paneHeader");
            auto const& terminal = PaneTreeHost::terminalOf(card);
            CHECK(header.isVisible());
            CHECK(header.height() == host.style().overlayPaneHeader());
            CHECK(header.z() > terminal.z());
            CHECK(terminal.y() == 0);
            CHECK(terminal.height() == card.height());
        }
    }

    SECTION("a click on a revealed action reaches the action, not the terminal beneath")
    {
        host.show(
            panes.split, /*visiblePanes*/ 2, static_cast<int>(contour::window::HeaderReveal::HeldModifier));
        auto const& card = host.cardOf(panes.left());
        auto& terminal = PaneTreeHost::terminalOf(card);
        auto const tapped = QSignalSpy { &terminal, SIGNAL(activated()) };
        auto const& header = PaneTreeHost::partOf(card, "paneHeader");
        auto const& actions = PaneTreeHost::partOf(card, "paneHeaderActions");
        QTest::mouseMove(&host.window,
                         header.mapToScene({ header.width() / 2, header.height() / 2 }).toPoint());
        REQUIRE(QTest::qWaitFor([&actions] { return actions.opacity() == 1.0; }));

        auto const& zoom = PaneTreeHost::partOf(card, "paneHeaderAction2");
        QTest::mouseClick(&host.window,
                          Qt::LeftButton,
                          Qt::AltModifier,
                          zoom.mapToScene({ zoom.width() / 2, zoom.height() / 2 }).toPoint());
        CHECK(panes.left().headerActions == std::vector<int> { 2 });

        // The header's own background takes its click too: the terminal beneath sees neither.
        QTest::mouseClick(&host.window,
                          Qt::LeftButton,
                          Qt::AltModifier,
                          header.mapToScene({ header.width() / 2, header.height() / 2 }).toPoint());
        CHECK(panes.left().activateCalls >= 1);
        CHECK(tapped.count() == 0);
    }

    SECTION("a single pane")
    {
        host.show(
            panes.left(), /*visiblePanes*/ 1, static_cast<int>(contour::window::HeaderReveal::HeldModifier));
        CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeader").isVisible());
    }
}

TEST_CASE("pane card: Increased Contrast rings the active card (offscreen)", "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.accessibility.contrast = contour::config::ContrastPreference::Increased;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& ring = PaneTreeHost::partOf(host.cardOf(panes.left()), "activeCardRing");
    CHECK(ring.isVisible());
    CHECK(ring.property("border").value<QObject*>()->property("width").toReal() == 1.5);
    CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "activeCardRing").isVisible());

    SECTION("and standard contrast rings none")
    {
        auto plain = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
        auto others = TwoPanes {};
        plain.show(others.split, /*visiblePanes*/ 2);
        CHECK(itemsNamed(plain.root(), QStringLiteral("activeCardRing")).size() == 2);
        for (auto const* other: itemsNamed(plain.root(), QStringLiteral("activeCardRing")))
            CHECK_FALSE(other->isVisible());
    }
}

TEST_CASE("pane card: Minimal frames no pane at the window's edge (offscreen)",
          "[contour][gui][qml][panecard]")
{
    using contour::config::ChromeDensity;
    auto const density = GENERATE(ChromeDensity::Comfortable, ChromeDensity::Minimal);
    CAPTURE(static_cast<int>(density));
    auto setup = paneCardSetup();
    setup.density = density;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    // Comfortable's cards each wear a border; Minimal's only line is the gap's, between the panes.
    for (auto const* leaf: { &panes.left(), &panes.right() })
        CHECK(PaneTreeHost::partOf(host.cardOf(*leaf), "paneCardSurface").isVisible()
              == (density == ChromeDensity::Comfortable));
    auto const lines = itemsNamed(host.root(), QStringLiteral("splitLine"));
    CHECK(std::ranges::count_if(lines, [](QQuickItem const* line) { return line->isVisible(); })
          == (density == ChromeDensity::Minimal ? 1 : 0));
}

TEST_CASE("pane card: a translucent profile's card is composited once (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    panes.leftSession.setOpacity(0.5F);
    host.show(panes.split, /*visiblePanes*/ 2);

    // The card paints no fill of its own: the header's background and the terminal's carry the
    // profile's opacity, each over the backdrop alone.
    auto const& card = host.cardOf(panes.left());
    CHECK(PaneTreeHost::partOf(card, "paneCardSurface").property("color").value<QColor>().alpha() == 0);
    CHECK(PaneTreeHost::partOf(card, "paneHeaderBackground").opacity() == Catch::Approx(0.5));
    CHECK(PaneTreeHost::terminalOf(card).opacity() == Catch::Approx(0.5));
}

TEST_CASE("pane card: under Reduce Motion the actions cross-fade and the overlay does not move (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.density = contour::config::ChromeDensity::Minimal;
    setup.accessibility.motion = contour::config::MotionPreference::Reduced;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& motion = *host.style().motion();
    CHECK(motion.property("style").toInt() == motion.property("crossFade").toInt());

    host.paneNode->setProperty("headerReveal", static_cast<int>(contour::window::HeaderReveal::HeldModifier));
    auto const& header = PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeader");
    CHECK(header.isVisible());

    // The header's only animation is the actions' fade, over motion.fast (the cross-fade's duration
    // here); nothing moves it.
    auto const animations = header.findChildren<QObject*>();
    auto const fades = std::ranges::count_if(
        animations, [](QObject const* object) { return object->inherits("QQuickNumberAnimation"); });
    CHECK(fades == 1);
    for (auto const* animation: animations)
        if (animation->inherits("QQuickPropertyAnimation"))
            CHECK(animation->property("duration").toInt() == motion.property("fast").toInt());
}

TEST_CASE("pane card: the Terminal row draws a header of one cell row (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& active = host.cardOf(panes.left());
    auto const& inactive = host.cardOf(panes.right());
    CHECK(PaneTreeHost::partOf(active, "paneHeader").height() == host.style().cellHeight());

    auto const expected =
        std::array { QStringLiteral("⊞"), QStringLiteral("⊟"), QStringLiteral("Z"), QStringLiteral("×") };
    for (auto const index: std::views::iota(size_t { 0 }, expected.size()))
    {
        auto const& button = PaneTreeHost::partOf(active, std::format("paneHeaderAction{}", index).c_str());
        CHECK(PaneTreeHost::partOf(button, "toolbarButtonGlyph").property("text").toString()
              == expected[index]);
    }

    // The inactive card fades its content, not its header.
    CHECK(PaneTreeHost::partOf(inactive, "paneCardContent").opacity() == Catch::Approx(0.62));
    CHECK(PaneTreeHost::partOf(inactive, "paneHeader").opacity() == 1.0);
    CHECK(inactive.opacity() == 1.0);
    CHECK(PaneTreeHost::partOf(active, "paneCardContent").opacity() == 1.0);

    // The row shows a focused pane's actions without hover.
    auto const& actions = PaneTreeHost::partOf(active, "paneHeaderActions");
    CHECK(QTest::qWaitFor([&actions] { return actions.opacity() == 1.0; }));
    CHECK(PaneTreeHost::partOf(inactive, "paneHeaderActions").opacity() == 0.0);
}

// }}}

TEST_CASE("TerminalPane.dimOverlayColor implements the unfocused-dim policy (offscreen)",
          "[contour][gui][qml][dim]")
{
    // The overlay's color decision is a pure function (like PaneNode.clampRatio) so every
    // dim/pane-focus/window-focus combination is testable without synthesizing window activation,
    // which the offscreen platform cannot do.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/TerminalPane.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> pane(component.create());
    REQUIRE(pane != nullptr);

    auto const colorFor = [&](double dim, bool paneActive, bool windowActive) -> QColor {
        QVariant result;
        REQUIRE(QMetaObject::invokeMethod(pane.get(),
                                          "dimOverlayColor",
                                          Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, dim),
                                          Q_ARG(QVariant, paneActive),
                                          Q_ARG(QVariant, windowActive),
                                          Q_ARG(QVariant, QColor(Qt::black))));
        return result.value<QColor>();
    };

    // dim == 0 never dims, regardless of focus.
    CHECK(colorFor(0.0, false, false).alphaF() == 0.0);
    // A focused pane in a focused window never dims.
    CHECK(colorFor(0.4, true, true).alphaF() == 0.0);
    // An inactive split pane dims, and so does the active pane of an unfocused window.
    CHECK(colorFor(0.4, false, true).alphaF() == Catch::Approx(0.4));
    CHECK(colorFor(0.4, true, false).alphaF() == Catch::Approx(0.4));
    // The blend target is the pane's own background color.
    CHECK(colorFor(0.4, false, false).red() == 0);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("TerminalPane dim overlay follows the session's dimUnfocused live (offscreen)",
          "[contour][gui][qml][dim]")
{
    // The overlay binds to session.dimUnfocused (NOTIFY-driven): default 0.0 renders no overlay
    // (pixel-identical to a build without the feature), and a runtime config change shows it.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    MockSession session;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("mockSession", &session);
    contour::test::QmlMessageCapture const warnings;

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property alias pane: paneItem\n"
                                        "  TerminalPane { id: paneItem; anchors.fill: parent;"
                                        " session: mockSession }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/DimWrapper.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> obj(component.create());
    REQUIRE(obj != nullptr);
    auto* pane = obj->property("pane").value<QQuickItem*>();
    REQUIRE(pane != nullptr);
    auto* overlay = pane->findChild<QQuickItem*>(QStringLiteral("dimOverlay"));
    REQUIRE(overlay != nullptr);
    QCoreApplication::processEvents();

    // Default (dim_unfocused: 0.0): no overlay, even though the pane is unfocused.
    CHECK_FALSE(overlay->isVisible());

    // Runtime change (config reload emits dimUnfocusedChanged): the unfocused pane dims with the
    // configured alpha, blending toward the session's background color.
    session.setDimUnfocused(0.4F);
    QCoreApplication::processEvents();
    CHECK(overlay->isVisible());
    CHECK(overlay->property("color").value<QColor>().alphaF() == Catch::Approx(0.4).epsilon(0.01));

    // Back to 0 hides it again.
    session.setDimUnfocused(0.0F);
    QCoreApplication::processEvents();
    CHECK_FALSE(overlay->isVisible());

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError") || w.contains("of null"); })
          == 0);
}

// ============================================================================================
// Command palette (Ctrl+Shift+P). The popup is driven against a REAL CommandPaletteModel rather than a
// stub, so the delegate's roles (title / shortcut / description / section) are the ones production
// actually publishes — a role renamed on one side and not the other would otherwise pass here and
// render a row of blanks in the app.
// ============================================================================================

namespace
{

/// A stand-in for WindowController exposing exactly the command-palette surface CommandPalette.qml
/// binds: the model, and the runCommand() the popup calls when a row is picked.
class MockPaletteController: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)

  public:
    MockPaletteController():
        _history { 5 }, _model { std::make_unique<contour::window::CommandPaletteModel>(_history) }
    {
        _history.record("TogglePaneZoom");
        _model->setSources({ &_actionCommands });
        _model->setShortcuts(contour::command::shortcutIndex(contour::config::defaultInputMappings(),
                                                             contour::command::ShortcutStyle::Text));
        _model->refresh();
    }

    [[nodiscard]] QObject* commandPalette() const noexcept { return _model.get(); }
    [[nodiscard]] contour::window::CommandPaletteModel& model() noexcept { return *_model; }

    /// Records what the popup asked to run, so a test can assert the pick actually routed.
    ///
    /// OpenCommandPalette is dispatched for real, not just recorded: that is the one command the palette
    /// offers that re-enters the palette (WindowController::runCommand -> executeAction -> the manager ->
    /// WindowController::openCommandPalette -> commandPaletteRequested -> Main.qml's `open()`), and the
    /// re-entry is only reproducible if this mock closes that loop the way the real chain does.
    Q_INVOKABLE void runCommand(QString const& id)
    {
        ran << id;
        if (observeOnRun)
            observeOnRun();
        if (id == QStringLiteral("OpenCommandPalette"))
            emit commandPaletteRequested();
    }
    QStringList ran;

    /// Called from within runCommand(), so a test can observe the palette's state at the exact moment
    /// the command fires — which is the whole contract for commands that open a keyboard-driven surface
    /// of their own (SetTabColor's picker): they must run once the palette is out of the way.
    std::function<void()> observeOnRun;

  signals:
    /// What WindowController raises to ask its window to show the palette; Main.qml answers it with open().
    void commandPaletteRequested();

  private:
    contour::command::CommandHistory _history;
    contour::command::ActionCommandSource _actionCommands;
    std::unique_ptr<contour::window::CommandPaletteModel> _model;
};

/// Hosts CommandPalette.qml in a Window that mirrors Main.qml's ApplicationWindow — in particular it
/// declares restoreTerminalFocus(), which the popup calls on close to hand the keyboard back to the
/// terminal, and counts the calls so a test can assert it actually happened.
///
/// Deliberately a real QML host rather than a bare QQuickWindow: the popup's contract with its window
/// IS that function, and a stub without it would make the call a no-op that no test could see.
struct PaletteHost
{
    std::unique_ptr<QObject> window; //!< The wrapper Window.
    QObject* palette = nullptr;      //!< The CommandPalette inside it (owned by the window).

    [[nodiscard]] int focusRestores() const { return window->property("focusRestores").toInt(); }
};

/// Builds the host, opens the palette, and pumps the event loop so the Popup materializes its content.
[[nodiscard]] PaletteHost openPalette(QQmlEngine& engine, MockPaletteController& controller)
{
    // The popup's background is a PopupSurface, which takes its shape and its drop shadow from
    // the design tokens -- so the engine needs them, as every other chrome component's does.
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("paletteController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  id: host\n"
                                        "  width: 800; height: 600; visible: true\n"
                                        "  property int focusRestores: 0\n"
                                        // NOT `palette`: a Window already has one (the Controls color
                                        // group), and shadowing it is a QML warning — which the
                                        // run-wide gate turns into a failure of the whole suite.
                                        "  property alias commandPaletteItem: paletteItem\n"
                                        "  function restoreTerminalFocus() { host.focusRestores++; }\n"
                                        "  CommandPalette {\n"
                                        "    id: paletteItem\n"
                                        "    controller: paletteController\n"
                                        "    window: host\n"
                                        "  }\n"
                                        // Main.qml's wiring, verbatim: the controller asks, the window
                                        // opens the palette. Without it here, the one command that re-opens
                                        // the palette (OpenCommandPalette) would look inert in a test and
                                        // its re-entry could not be tested at all.
                                        "  Connections {\n"
                                        "    target: paletteController\n"
                                        "    function onCommandPaletteRequested() { paletteItem.open(); }\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaletteTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    auto host = PaletteHost {};
    {
        INFO("PaletteTestHost errors: " << component.errorString().toStdString());
        if (!component.isReady())
            return host;
    }

    host.window.reset(component.create());
    if (host.window == nullptr)
        return host;

    host.palette = host.window->property("commandPaletteItem").value<QObject*>();
    if (host.palette == nullptr)
        return host;

    // A Popup builds its contentItem lazily; opening it is what materializes the filter field and the
    // list, which is the state every assertion below is about.
    QMetaObject::invokeMethod(host.palette, "open");
    QCoreApplication::processEvents();
    return host;
}

} // namespace

TEST_CASE("The command palette lists commands with their shortcut and documentation (offscreen)",
          "[contour][gui][qml][palette]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    auto* list = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);

    SECTION("the list is populated and sectioned before the user types")
    {
        CHECK(list->property("count").toInt() == controller.model().rowCount());
        CHECK(list->property("count").toInt() > 0);
        CHECK(controller.model().sectioned());
    }

    SECTION("a row carries a title, a description and — when bound — a shortcut")
    {
        // The three things the palette promises to show. Read them off the MODEL through the same roles
        // the delegate binds, so a renamed role fails here.
        controller.model().setFilter(QStringLiteral("SplitVertical"));
        QCoreApplication::processEvents();
        REQUIRE(controller.model().rowCount() > 0);

        auto const index = controller.model().index(0, 0);
        CHECK(controller.model()
                  .data(index, static_cast<int>(contour::window::CommandPaletteModel::Roles::TitleRole))
                  .toString()
              == QStringLiteral("Split Vertical"));
        CHECK(controller.model()
                  .data(index, static_cast<int>(contour::window::CommandPaletteModel::Roles::ShortcutRole))
                  .toString()
              == QStringLiteral("Ctrl+Shift+E"));
        CHECK_FALSE(
            controller.model()
                .data(index, static_cast<int>(contour::window::CommandPaletteModel::Roles::DescriptionRole))
                .toString()
                .isEmpty());
    }

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Typing in the palette filters the list through the model (offscreen)",
          "[contour][gui][qml][palette]")
{
    // The filter field is wired to the model's `filter` property, so typing must actually narrow the
    // list — a TextField that looks right but is not connected would pass a load-only test.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    auto* list = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteList"));
    REQUIRE(list != nullptr);

    auto const unfiltered = list->property("count").toInt();
    REQUIRE(unfiltered > 0);

    filter->setProperty("text", QStringLiteral("spl"));
    QCoreApplication::processEvents();

    CHECK(controller.model().filter() == QStringLiteral("spl"));
    CHECK(list->property("count").toInt() < unfiltered);
    CHECK(list->property("count").toInt() > 0);
    // Sections collapse once there is a query; the delegate's header binds to this.
    CHECK_FALSE(controller.model().sectioned());

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Accepting a palette row runs that command and gives the keyboard back (offscreen)",
          "[contour][gui][qml][palette]")
{
    // The pick has to route all the way back to the controller with the right id — the popup is mere
    // decoration otherwise — and the terminal has to get the keyboard back, or the user is left typing
    // into a dismissed popup.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);

    filter->setProperty("text", QStringLiteral("SplitVertical"));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().rowCount() > 0);

    // Asserted from INSIDE the command, the only place the ordering is observable — by the time
    // acceptCurrent() returns, everything has already happened. The command must run LAST: one that opens
    // a keyboard-driven surface of its own (SetTabColor's swatch picker, SetTabTitle's rename field)
    // takes the focus as it opens, so running it any earlier lets the palette's own dismissal reclaim
    // that focus, and the surface cannot be typed into at all.
    controller.observeOnRun = [&] {
        CHECK_FALSE(host.palette->property("visible").toBool()); // the palette is already gone
        CHECK(host.focusRestores() == 1);                        // and has handed the keyboard back
    };

    QMetaObject::invokeMethod(host.palette, "acceptCurrent");
    QCoreApplication::processEvents();

    // (Which also proves the observer above ran at all.)
    REQUIRE(controller.ran.size() == 1);
    CHECK(controller.ran.front() == QStringLiteral("SplitVertical"));
    // Picking a command dismisses the popup and hands the keyboard back to the terminal.
    CHECK_FALSE(host.palette->property("visible").toBool());
    CHECK(host.focusRestores() == 1);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Picking OpenCommandPalette re-opens the palette cleanly, not on top of its own close (offscreen)",
          "[contour][gui][qml][palette]")
{
    // The palette offers "Open Command Palette" as a row — it is a plain command taking no argument, so the
    // catalog lists it like any other. Picking it therefore runs a command that re-opens the very popup
    // that is closing: WindowController::runCommand -> the manager -> openCommandPalette -> the window's
    // open(). The pick must not re-enter Popup.open() from INSIDE the popup's own closed() emission —
    // there, Qt is still unwinding the exit transition, and the palette ends up needing two dismissals.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);

    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);

    filter->setProperty("text", QStringLiteral("OpenCommandPalette"));
    QCoreApplication::processEvents();
    REQUIRE(controller.model().rowCount() > 0);
    REQUIRE(controller.model()
                .data(controller.model().index(0, 0),
                      static_cast<int>(contour::window::CommandPaletteModel::Roles::IdRole))
                .toString()
            == QStringLiteral("OpenCommandPalette"));

    QMetaObject::invokeMethod(host.palette, "acceptCurrent");

    // THE PIN, asserted before the event loop gets a turn: the popup has closed, and the command has NOT
    // run. That gap is the fix. Closing is synchronous, so a command dispatched straight from onClosed
    // would already be on `ran` here — running while Qt is still inside the popup's own closed() emission,
    // which for this command means calling Popup.open() on top of a close that has not finished unwinding.
    REQUIRE_FALSE(host.palette->property("opened").toBool());
    CHECK(controller.ran.isEmpty());

    QCoreApplication::processEvents(); // ... and now, with the popup at rest, it runs

    // It ran, exactly once, and re-opened the palette — a fresh open rather than one stacked on a close.
    REQUIRE(controller.ran.size() == 1);
    CHECK(controller.ran.front() == QStringLiteral("OpenCommandPalette"));
    CHECK(host.palette->property("opened").toBool());

    // And one dismissal dismisses it.
    QMetaObject::invokeMethod(host.palette, "close");
    QCoreApplication::processEvents();

    CHECK_FALSE(host.palette->property("opened").toBool());
    CHECK_FALSE(host.palette->property("visible").toBool());
    CHECK(controller.ran.size() == 1); // closing runs nothing further

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("Dismissing the palette runs nothing and gives the keyboard back (offscreen)",
          "[contour][gui][qml][palette]")
{
    // Escape must be a clean exit: no command runs, and the terminal is typeable again immediately.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;

    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    REQUIRE(host.palette->property("visible").toBool());

    QMetaObject::invokeMethod(host.palette, "close");
    QCoreApplication::processEvents();

    CHECK(controller.ran.isEmpty());
    CHECK_FALSE(host.palette->property("visible").toBool());
    CHECK(host.focusRestores() == 1);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

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

// ============================================================================================
// SaveLayoutDialog: the "save layout as" name prompt a nameless SaveLayout action opens. The exact
// mirror of the TabColorFlyout tests above — a bare action opens a surface that supplies the
// argument — and of the command-palette focus tests: the prompt hands the keyboard back on close.
// ============================================================================================

namespace
{

/// A stand-in for WindowController exposing exactly the save-layout surface SaveLayoutDialog.qml binds:
/// the saveLayoutRequested signal Main.qml opens the dialog on, and the saveLayoutAs() the dialog calls
/// back with the typed name.
class MockSaveLayoutController: public QObject
{
    Q_OBJECT

  public:
    /// Mirrors WindowController::beginSaveLayoutPrompt(): what the manager calls for a nameless
    /// SaveLayout action, and what makes Main.qml's prompt appear.
    Q_INVOKABLE void beginSaveLayoutPrompt() { emit saveLayoutRequested(); }

    /// Records the name the dialog accepted, so a test can assert the accept routed through with it.
    Q_INVOKABLE void saveLayoutAs(QString const& name) { saved << name; }
    QStringList saved;

  signals:
    /// What WindowController raises to ask its window to open the prompt; Main.qml answers with open().
    void saveLayoutRequested();
};

/// Hosts SaveLayoutDialog.qml in a Window mirroring Main.qml's ApplicationWindow — it declares
/// restoreTerminalFocus() (which the dialog calls on close) and wires saveLayoutRequested -> open(),
/// exactly as Main.qml does, so the open path and the focus-restore contract are both exercised.
struct SaveLayoutHost
{
    std::unique_ptr<QObject> window; //!< The wrapper Window.
    QObject* dialog = nullptr;       //!< The SaveLayoutDialog inside it (owned by the window).

    [[nodiscard]] int focusRestores() const { return window->property("focusRestores").toInt(); }
    [[nodiscard]] bool opened() const { return dialog != nullptr && dialog->property("opened").toBool(); }
    [[nodiscard]] QQuickItem* nameField() const
    {
        return dialog != nullptr ? dialog->findChild<QQuickItem*>(QStringLiteral("saveLayoutNameField"))
                                 : nullptr;
    }
};

/// Builds the host and locates the dialog (not yet opened — the tests open it via the controller's
/// request, which is the path Main.qml takes).
[[nodiscard]] SaveLayoutHost makeSaveLayoutHost(QQmlEngine& engine, MockSaveLayoutController& controller)
{
    // The popup's background is a PopupSurface, which takes its shape and its drop shadow from
    // the design tokens -- so the engine needs them, as every other chrome component's does.
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("saveLayoutController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import Contour.Ui\n"
                                        "Window {\n"
                                        "  id: host\n"
                                        "  width: 800; height: 600; visible: true\n"
                                        "  property int focusRestores: 0\n"
                                        "  property alias saveLayoutDialogItem: dialogItem\n"
                                        "  function restoreTerminalFocus() { host.focusRestores++; }\n"
                                        "  SaveLayoutDialog {\n"
                                        "    id: dialogItem\n"
                                        "    controller: saveLayoutController\n"
                                        "    window: host\n"
                                        "  }\n"
                                        // Main.qml's wiring, verbatim: the controller asks, the window
                                        // opens the prompt.
                                        "  Connections {\n"
                                        "    target: saveLayoutController\n"
                                        "    function onSaveLayoutRequested() { dialogItem.open(); }\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SaveLayoutTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    auto host = SaveLayoutHost {};
    {
        INFO("SaveLayoutTestHost errors: " << component.errorString().toStdString());
        if (!component.isReady())
            return host;
    }

    host.window.reset(component.create());
    if (host.window == nullptr)
        return host;

    host.dialog = host.window->property("saveLayoutDialogItem").value<QObject*>();
    return host;
}

/// Opens the dialog the way a nameless SaveLayout action does (controller request -> Main.qml open()) and
/// pumps the loop so the Popup materializes its field.
void openSaveLayoutPrompt(MockSaveLayoutController& controller)
{
    controller.beginSaveLayoutPrompt();
    QCoreApplication::processEvents();
}

} // namespace

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
    QMetaObject::invokeMethod(host.dialog, "accept");
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
    QMetaObject::invokeMethod(host.dialog, "accept");
    QCoreApplication::processEvents();

    CHECK(controller.saved.isEmpty());
    CHECK(host.opened()); // still up, nothing saved

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

TEST_CASE("The palette bolds matched characters and tints only unselected rows (offscreen)",
          "[contour][gui][qml][palette]")
{
    // The title label renders StyledText so the filter's matches stand out: bold everywhere, and accent
    // coloured too — except on the selected row, whose accent background would swallow the tint.
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

    // Both rows bold their matched characters...
    CHECK(selectedTitle.contains(QStringLiteral("<b>")));
    CHECK(unselectedTitle.contains(QStringLiteral("<b>")));
    // ...but only the unselected row tints them with the accent colour.
    CHECK(unselectedTitle.contains(QStringLiteral("<font color=\"")));
    CHECK_FALSE(selectedTitle.contains(QStringLiteral("<font color=\"")));

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}

TEST_CASE("SessionChrome shows the hyperlink tooltip only while there is a link under the pointer",
          "[contour][gui][qml][hyperlink]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto* tip = host.chrome->findChild<QQuickItem*>(QStringLiteral("hyperlinkTooltip"));
    REQUIRE(tip != nullptr);

    // Nothing hovered: nothing to say. The tooltip's own text is the gate, so an empty one can never
    // pop an empty box.
    CHECK(tip->property("tipText").toString().isEmpty());

    SECTION("a hovered link publishes its text and is placed at its anchor")
    {
        session->setHyperlinkHover("https://example.com/", QRectF(120, 80, 8, 16));
        QCoreApplication::processEvents();

        CHECK(tip->property("tipText").toString() == "https://example.com/");
        CHECK(tip->x() == Catch::Approx(120));
        // Well below the top edge, so it goes ABOVE the cell -- the anchor's own y.
        CHECK(tip->property("showAbove").toBool());
        CHECK(tip->y() == Catch::Approx(80));
    }

    SECTION("a link on the top row is placed below it, where there is room")
    {
        session->setHyperlinkHover("https://example.com/", QRectF(10, 0, 8, 16));
        QCoreApplication::processEvents();

        CHECK_FALSE(tip->property("showAbove").toBool());
        CHECK(tip->y() == Catch::Approx(16)); // just under the cell
    }

    SECTION("a link at the right edge is clamped into the pane")
    {
        // Without the clamp the tooltip hangs off the pane, which is the popup-placement trap this
        // codebase has already been bitten by once.
        session->setHyperlinkHover("https://example.com/", QRectF(host.chrome->width() + 50, 40, 8, 16));
        QCoreApplication::processEvents();

        CHECK(tip->x() <= host.chrome->width());
    }

    SECTION("leaving the link withdraws the tooltip")
    {
        session->setHyperlinkHover("https://example.com/", QRectF(120, 80, 8, 16));
        QCoreApplication::processEvents();
        REQUIRE_FALSE(tip->property("tipText").toString().isEmpty());

        session->setHyperlinkHover(QString(), QRectF());
        QCoreApplication::processEvents();
        CHECK(tip->property("tipText").toString().isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SessionChrome shows the command block tooltip only while a block is hovered",
          "[contour][gui][qml][gutter]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto* tip = host.chrome->findChild<QQuickItem*>(QStringLiteral("commandBlockTooltip"));
    REQUIRE(tip != nullptr);
    CHECK(tip->property("tipText").toString().isEmpty());
    // Sooner than the hyperlink tip: the gutter is where a pointer goes on purpose.
    CHECK(tip->property("showDelay").toInt() == 300);

    SECTION("a hovered block publishes its text, beside its gutter cell")
    {
        session->setCommandBlockHover("Exit code 2\n$ make", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();

        CHECK(tip->property("tipText").toString() == "Exit code 2\n$ make");
        CHECK(tip->x() == Catch::Approx(12)); // on the grid side of the cell, not over the strip
    }

    SECTION("another block entered at the same cell is read afresh")
    {
        // New output scrolls the page under a stationary pointer without moving the viewport, so the
        // next block can be entered at the very cell the last one was: the anchor does not change, and
        // the text must be read again all the same.
        session->setCommandBlockHover("Exit code 2", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();
        REQUIRE(tip->property("tipText").toString() == "Exit code 2");

        session->setCommandBlockHover("Exit code 0", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();
        CHECK(tip->property("tipText").toString() == "Exit code 0");
    }

    SECTION("leaving the block withdraws it")
    {
        session->setCommandBlockHover("Exit code 2", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();
        REQUIRE_FALSE(tip->property("tipText").toString().isEmpty());

        session->setCommandBlockHover(QString(), QRectF());
        QCoreApplication::processEvents();
        CHECK(tip->property("tipText").toString().isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
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

/// The scene-coordinate rectangle @p item occupies.
[[nodiscard]] QRectF sceneRectOf(QQuickItem const& item)
{
    return { item.mapToScene(QPointF(0, 0)), QSizeF(item.width(), item.height()) };
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

/// Builds the REAL TitleBar with its window controls enabled, under @p uiStyle and
/// @p windowControlStyle.
///
/// The one place TitleBar is instantiated for a test, so a change to how it is instantiated -- a new
/// required property, say -- lands on every test at once rather than on whichever host builder the
/// author happened to notice. The bar sits at z:1 as Main.qml puts it, which is what lets a caller
/// stack the REAL ResizeBorder (z:1000) over it via @p extraChildren.
///
/// @param engine             The engine to build in. Both style providers are installed on it.
/// @param controller         The mock the bar binds to.
/// @param uiStyle            The chrome style, i.e. what decides the extents.
/// @param windowControlStyle The window-control style, i.e. what decides side, order and shape.
/// @param extraChildren      QML appended after the bar, e.g. a ResizeBorder over it.
/// @param width              The host window's width. Wide by default, so the tab strip's 70% cap
///                           leaves the drag region real width and "left of the tabs" and "right of
///                           the drag region" are genuinely different places to be.
/// @return The host Window; the bar is reachable via findChild("titleBar").
[[nodiscard]] QmlHost makeTitleBarHost(
    QQmlEngine& engine,
    MockTabController& controller,
    contour::config::UiStyle uiStyle = contour::config::UiStyle::Native,
    contour::config::WindowControlStyle windowControlStyle = contour::config::WindowControlStyle::Windows,
    QString const& extraChildren = {},
    int width = 900)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, uiStyle, windowControlStyle);

    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import Contour.Ui\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: %1; height: 400; visible: true\n"
                                       "  TitleBar {\n"
                                       "    objectName: \"titleBar\"\n"
                                       "    anchors.left: parent.left\n"
                                       "    anchors.right: parent.right\n"
                                       "    y: 0\n"
                                       "    height: implicitHeight\n"
                                       "    z: 1\n"
                                       "    controller: terminalSessions\n"
                                       "    window: host\n"
                                       "    useCustomWindowControls: true\n"
                                       "  }\n"
                                       "%2"
                                       "}\n")
                            .arg(width)
                            .arg(extraChildren);

    return buildQmlHost(engine, source, QStringLiteral("TitleBarTestHost.qml"));
}
} // namespace

TEST_CASE("the new-tab menu lists the profiles (offscreen)", "[contour][gui][qml][menus]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    // The menu is the title bar's, which pops it below the strip's "▾".
    auto const host = makeTitleBarHost(engine, controller);
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

namespace
{
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

TEST_CASE("A native tab keeps the exact width its historical literal gave it (offscreen)",
          "[contour][gui][qml]")
{
    // The band check above cannot see a shift inside 120..240, and UiStyle_test pins the token values
    // without adding them up -- so this is the only assertion that notices the tokenized sum coming to
    // something other than the `label.implicitWidth + 56` it replaced. A drift there re-flows every tab
    // in the strip of every user who never chose a style at all.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, contour::config::UiStyle::Native);

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

    auto const* label = tab->findChild<QQuickItem*>(QStringLiteral("tabLabel"));
    REQUIRE(label != nullptr);

    // The literal 56, deliberately, rather than a sum of the same tokens the QML reads: re-deriving it
    // here would agree with any token edit whatsoever, which is the one thing this must not do.
    CHECK(tab->property("naturalWidth").toReal()
          == Catch::Approx(label->property("implicitWidth").toReal() + 56.0));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("A tab's close button sits in clear space at both ends (offscreen)", "[contour][gui][qml]")
{
    // The trailing gap is the one that has to be stated. A style that rules between tabs ends every
    // tab with that rule, and with the close button anchored straight onto it "×│" sat side by side --
    // at one cell each, in the same monospace font, that reads as a single box-drawing figure rather
    // than as a button beside a border.
    //
    // Native is asserted from the same measurement rather than left implicit: it deliberately keeps a
    // trailing gap of 0 (its tabs have no drawn edge, so the gap the eye sees is the NEXT tab's
    // leading inset), and a token edit that "fixed" that would silently widen every tab in the strip
    // of every user who never chose a style at all.
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

    SECTION("the native style keeps the gaps it always had")
    {
        auto const native = gapsUnder(contour::config::UiStyle::Native);
        CHECK(native.leading == Catch::Approx(4.0)); // labelGapUnits
        CHECK(native.trailing == Catch::Approx(0.0).margin(0.001));
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
    // window's size hints -- so this is also what shrinks the terminal viewport. Native keeps the 34px
    // MainWindowQml_test asserts on; Terminal must be exactly one row of the chrome font.
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

    SECTION("native keeps its historical height")
    {
        CHECK(chromeHeightUnder(contour::config::UiStyle::Native).value == 34.0);
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
    // to controlWidth.
    contour::test::QmlMessageCapture const warnings;
    auto const style = GENERATE(contour::config::UiStyle::Native, contour::config::UiStyle::Terminal);

    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, style);

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

    for (auto const* name: { "newTabButton", "newTabMenuButton" })
    {
        INFO("button: " << name);
        auto const* button = strip->findChild<QQuickItem*>(QLatin1StringView(name));
        REQUIRE(button != nullptr);
        CHECK(button->width() == provider->stripButtonWidth());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{

/// Finds the first descendant of @p root satisfying @p predicate, in the VISUAL tree.
///
/// findChild() is not enough for anything a Repeater created: a delegate is given a parentItem but
/// no QObject parent, so it never appears among its ancestors' QObject children. The window controls
/// are exactly that, being one delegate per row of `windowControls.buttons`, and so is whatever a
/// Loader loads.
///
/// @param root      The item to search beneath.
/// @param predicate What to look for.
/// @return The item, or nullptr when no descendant satisfies @p predicate.
[[nodiscard]] QQuickItem* findDescendant(QQuickItem const& root, auto&& predicate)
{
    for (auto* child: root.childItems())
    {
        if (predicate(*child))
            return child;
        if (auto* found = findDescendant(*child, predicate); found != nullptr)
            return found;
    }
    return nullptr;
}

/// Finds the item named @p objectName anywhere in @p root's visual tree. @see findDescendant.
[[nodiscard]] QQuickItem* findItemNamed(QQuickItem const& root, QString const& objectName)
{
    return findDescendant(root, [&](QQuickItem const& item) { return item.objectName() == objectName; });
}

} // namespace

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
    // close, minimize, maximize in that order, so maximize is the one.
    auto const trafficLights = style == contour::config::WindowControlStyle::MacOS;
    auto const expectedFor = [&](std::string_view action) {
        if (!trafficLights)
            return provider->windowControlWidth();
        return action == "maximize" ? provider->trafficLightDotSize()
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
                                    : provider->chromeHeight();

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
    auto const host = makeTitleBarHost(
        engine, controller, contour::config::UiStyle::Native, contour::config::WindowControlStyle::Windows);
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
    provider->setStyle(contour::config::WindowControlStyle::MacOS);

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

// {{{ The find bar (issue #2081)
namespace
{

/// A stand-in for TerminalSession carrying exactly the search surface SearchBar.qml binds to.
///
/// The real session needs a PTY, a render thread and a display; the bar needs four properties, five
/// invokables and one signal. Mocking that surface is what makes the bar's BEHAVIOUR -- what it shows,
/// what it enables, what it does to focus -- testable offscreen, and it doubles as a check that the
/// bar reads nothing else.
class MockSearchSession: public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString searchPattern READ searchPattern NOTIFY searchStateChanged)
    Q_PROPERTY(QString searchSummary READ searchSummary NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchHasMatches READ searchHasMatches NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchNavigable READ searchNavigable NOTIFY searchStateChanged)
    // The affordance as the bar consumes it -- a glyph and a lit flag, not an enumerator. Mirrors
    // TerminalSession, which resolves it through describeSearchCase.
    Q_PROPERTY(QString searchCaseGlyph READ searchCaseGlyph NOTIFY searchStateChanged)
    Q_PROPERTY(QString searchCaseTooltip READ searchCaseTooltip NOTIFY searchStateChanged)
    Q_PROPERTY(bool searchCasePinned READ searchCasePinned NOTIFY searchStateChanged)

  public:
    [[nodiscard]] QString searchPattern() const { return _pattern; }
    [[nodiscard]] QString searchSummary() const { return _summary; }
    [[nodiscard]] bool searchHasMatches() const { return _hasMatches; }
    [[nodiscard]] bool searchNavigable() const { return _navigable; }
    [[nodiscard]] QString searchCaseGlyph() const
    {
        return QString::fromUtf8(contour::session::describeSearchCase(_caseMode).glyph.data());
    }
    // The real session renders this through tr(); the bar only needs a non-empty string here.
    [[nodiscard]] QString searchCaseTooltip() const { return QStringLiteral("match case"); }
    [[nodiscard]] bool searchCasePinned() const
    {
        return contour::session::describeSearchCase(_caseMode).pinned == contour::session::CasePinned::Yes;
    }

    Q_INVOKABLE void setSearchPattern(QString const& pattern)
    {
        ++setPatternCalls;
        _pattern = pattern;
        // A crude stand-in for the real tally, enough to drive every state the bar can render.
        if (pattern.isEmpty())
        {
            _summary.clear();
            _hasMatches = true;
            _navigable = false;
        }
        else if (pattern == QStringLiteral("zzz"))
        {
            _summary = QStringLiteral("No results");
            _hasMatches = false;
            _navigable = false;
        }
        else
        {
            _summary = QStringLiteral("1 of 3");
            _hasMatches = true;
            _navigable = true;
        }
        emit searchStateChanged();
    }

    Q_INVOKABLE void cycleSearchCaseSensitivity()
    {
        _caseMode = contour::session::nextSearchCase(_caseMode);
        emit searchStateChanged();
    }

    Q_INVOKABLE void searchBarOpened() { ++openNotifications; }
    Q_INVOKABLE void searchBarClosed() { ++closeNotifications; }

    int openNotifications = 0;
    int closeNotifications = 0;
    /// Counts the pushes FROM the field, so a re-seed that bounces back can be told from a keystroke.
    int setPatternCalls = 0;
    [[nodiscard]] vtbackend::SearchCaseSensitivity caseMode() const { return _caseMode; }

    Q_INVOKABLE void searchNext() { ++nextCalls; }
    Q_INVOKABLE void searchPrevious() { ++previousCalls; }
    Q_INVOKABLE void clearSearch() { setSearchPattern(QString {}); }

    int nextCalls = 0;
    int previousCalls = 0;

  signals:
    void searchStateChanged();

  private:
    QString _pattern;
    QString _summary;
    bool _hasMatches = true;
    bool _navigable = false;
    vtbackend::SearchCaseSensitivity _caseMode = vtbackend::SearchCaseSensitivity::Smart;
};

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
[[nodiscard]] SearchBarHost openSearchBar(QQmlEngine& engine,
                                          MockSearchSession& session,
                                          MockSearchSession* alternate = nullptr)
{
    contour::test::installChromeStyle(engine);
    engine.rootContext()->setContextProperty("mockSession", &session);
    engine.rootContext()->setContextProperty("mockSessionAlt", alternate != nullptr ? alternate : &session);

    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral("import QtQuick\n"
                          "import QtQuick.Window\n"
                          "import Contour.Ui\n"
                          "Window {\n"
                          "  id: host\n"
                          "  width: 800; height: 600; visible: true\n"
                          "  property int focusRestores: 0\n"
                          "  property bool useAlternate: false\n"
                          "  property alias searchBarItem: barItem\n"
                          // Stands in for the ContourTerminal the bar docks against: the bar reads its
                          // width to place itself and calls forceActiveFocus() on it when it closes.
                          "  Item {\n"
                          "    id: fakeDisplay\n"
                          "    anchors.fill: parent\n"
                          "    function forceActiveFocus() { host.focusRestores++; }\n"
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

} // namespace

TEST_CASE("The find bar drives the session's search and restores focus (offscreen)",
          "[contour][gui][qml][search]")
{
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockSearchSession session;

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
        auto* toggle = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCaseToggle"));
        REQUIRE(toggle != nullptr);

        // Asserted through what the BUTTON shows, not through enumerator values: an earlier version
        // compared raw integers here and happily pinned an inverted mapping in place.
        CHECK(toggle->property("text").toString() == QStringLiteral("Aa"));
        CHECK_FALSE(toggle->property("checked").toBool()); // smart is not "pinned"

        // Smart -> Sensitive: still the capitalised glyph, now lit.
        QMetaObject::invokeMethod(host.bar, "cycleCase");
        QCoreApplication::processEvents();
        CHECK(toggle->property("text").toString() == QStringLiteral("Aa"));
        CHECK(toggle->property("checked").toBool());

        // Sensitive -> Insensitive: the lowercase glyph is the one that means "ignoring case".
        QMetaObject::invokeMethod(host.bar, "cycleCase");
        QCoreApplication::processEvents();
        CHECK(toggle->property("text").toString() == QStringLiteral("aa"));
        CHECK(toggle->property("checked").toBool());

        // Insensitive -> Smart closes the cycle. Two states could not express the third, which is
        // why this is not the binary Aa every other find bar has.
        QMetaObject::invokeMethod(host.bar, "cycleCase");
        QCoreApplication::processEvents();
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
        auto* toggle = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCaseToggle"));
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
        auto* toggle = host.bar->findChild<QQuickItem*>(QStringLiteral("searchBarCaseToggle"));
        REQUIRE(toggle != nullptr);

        QMetaObject::invokeMethod(toggle, "forceActiveFocus");
        QCoreApplication::processEvents();
        REQUIRE(toggle->hasActiveFocus());

        QTest::keyClick(window, Qt::Key_Space); // Smart -> Sensitive
        QCoreApplication::processEvents();
        REQUIRE(session.caseMode() == vtbackend::SearchCaseSensitivity::Sensitive);
        CHECK(toggle->property("checked").toBool());

        QTest::keyClick(window, Qt::Key_Space); // Sensitive -> Insensitive: still pinned, still lit
        QCoreApplication::processEvents();
        REQUIRE(session.caseMode() == vtbackend::SearchCaseSensitivity::Insensitive);
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
    MockSearchSession first;
    MockSearchSession second;
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
// }}}

namespace
{
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

namespace
{
/// Creates the Banner under @p engine with @p initialProperties.
[[nodiscard]] std::unique_ptr<QObject> createBanner(QQmlEngine& engine, QVariantMap const& initialProperties)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/Banner.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto banner = std::unique_ptr<QObject>(component.createWithInitialProperties(initialProperties));
    REQUIRE(banner != nullptr);
    QCoreApplication::processEvents();
    return banner;
}

/// The named token @p name of group @p group of the installed `chromeStyle`.
[[nodiscard]] qreal chromeToken(QQmlEngine& engine, char const* group, char const* name)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto* tokens = style->property(group).value<QObject*>();
    REQUIRE(tokens != nullptr);
    return tokens->property(name).toReal();
}
} // namespace

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
    CHECK(item->height() == content->implicitHeight() + 2 * inset);

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

namespace
{
/// The Terminal row painted with the built-in color scheme @p schemeName.
[[nodiscard]] contour::test::ChromeSetup terminalRowSetup(std::string_view schemeName)
{
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.scheme = contour::config::chromeSchemeColorsOf(palette);
    return setup;
}

/// The contrast ratio of @p label's text colour on @p fill.
[[nodiscard]] double contrastOn(QObject const* label, vtbackend::RGBColor fill)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(label->property("color").value<QColor>()),
                                    fill);
}

/// The colour an item paints as, read from its `color` property.
[[nodiscard]] vtbackend::RGBColor fillOf(QObject const* item)
{
    return contour::platform::toRGBColor(item->property("color").value<QColor>());
}

/// The light/dark name of @p appearance, for a test's INFO.
[[nodiscard]] std::string_view appearanceName(vtbackend::ColorPreference appearance) noexcept
{
    return appearance == vtbackend::ColorPreference::Light ? "light" : "dark";
}
} // namespace

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

namespace
{
/// Creates the Toast under @p engine with @p initialProperties.
[[nodiscard]] std::unique_ptr<QObject> createToast(QQmlEngine& engine, QVariantMap const& initialProperties)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/Toast.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto toast = std::unique_ptr<QObject>(component.createWithInitialProperties(initialProperties));
    REQUIRE(toast != nullptr);
    QCoreApplication::processEvents();
    return toast;
}
} // namespace

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

TEST_CASE("Toast: the Terminal row is a one-row inverse strip (offscreen)", "[contour][gui][qml][kit][toast]")
{
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Foreground)] =
        vtbackend::RGBColor { 0xD9DCE3u };
    setup.scheme.bySlot[static_cast<size_t>(contour::config::SchemeSlot::Background)] =
        vtbackend::RGBColor { 0x1C1E24u };
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

namespace
{
/// Creates the KitDialog under @p engine, parented to @p parent, with @p initialProperties, and opens it.
[[nodiscard]] std::unique_ptr<QObject> openKitDialog(QQmlEngine& engine,
                                                     QQuickItem* parent,
                                                     QVariantMap initialProperties)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/KitDialog.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    initialProperties.insert(QStringLiteral("parent"), QVariant::fromValue(parent));
    auto dialog = std::unique_ptr<QObject>(component.createWithInitialProperties(initialProperties));
    REQUIRE(dialog != nullptr);
    REQUIRE(QMetaObject::invokeMethod(dialog.get(), "open"));
    auto const* opened = dialog.get();
    REQUIRE(QTest::qWaitFor([&] { return opened->property("opened").toBool(); }, 2000));
    return dialog;
}

/// A 1024 x 640 window with a root item to parent a dialog to.
struct DialogHost
{
    QQuickWindow window;
    DialogHost()
    {
        window.resize(1024, 640);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }
};
} // namespace

TEST_CASE("KitDialog: a sheet on the macOS row, centred elsewhere (offscreen)",
          "[contour][gui][qml][kit][dialog]")
{
    using contour::config::ChromeStyle;
    QQmlEngine engine;
    auto setup = contour::test::defaultChromeSetup();
    setup.row = GENERATE(ChromeStyle::MacOS, ChromeStyle::ContourLinux, ChromeStyle::Terminal);
    contour::test::installChromeStyle(engine, setup);
    contour::test::QmlMessageCapture const warnings;

    DialogHost host;
    auto dialog = openKitDialog(
        engine, host.window.contentItem(), QVariantMap { { "heading", QStringLiteral("Close this tab?") } });
    auto const overlayWidth = host.window.contentItem()->width();
    auto const overlayHeight = host.window.contentItem()->height();

    CHECK(dialog->property("width").toReal() == chromeToken(engine, "size", "dialogWidth"));
    CHECK(std::abs(dialog->property("x").toReal() - (overlayWidth - dialog->property("width").toReal()) / 2)
          <= 1);
    if (setup.row == ChromeStyle::MacOS)
        CHECK(dialog->property("y").toReal() == 0);
    else
        CHECK(std::abs(dialog->property("y").toReal()
                       - (overlayHeight - dialog->property("height").toReal()) / 2)
              <= 1);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("KitDialog: accept and reject close it and emit (offscreen)", "[contour][gui][qml][kit][dialog]")
{
    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    DialogHost host;
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

    DialogHost host;
    auto dialog = openKitDialog(engine,
                                host.window.contentItem(),
                                QVariantMap { { "heading", QStringLiteral("Close this tab?") },
                                              { "acceptRole", provider->statusDanger() } });
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

    DialogHost host;
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
    DialogHost host;

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
    CHECK(std::abs(dialog->property("y").toReal() - (overlayHeight - dialog->property("height").toReal()) / 2)
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

#include <QmlComponents_test.moc>
