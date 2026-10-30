// SPDX-License-Identifier: Apache-2.0
//
// Fixtures shared by the Qml*_test.cpp files: the mocks the components are built over and the hosts
// that instantiate them.

#pragma once

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/platform/CaptionHitTest.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/PermissionPrompt.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SaveLayoutCheck.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowController.hpp>

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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
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
#include <vtworkspace/Primitives.hpp>

namespace contour::test::qmlSupport
{

using contour::test::itemsNamed;

using contour::test::MockPaneProxy;

using contour::test::MockSession;

using contour::test::StubContourTerminal;

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
    // Mirrors WindowController's maximise button as the OS drives it; a test sets it with
    // setNativeMaximizeVisual().
    Q_PROPERTY(bool nativeMaximizeHovered READ nativeMaximizeHovered NOTIFY nativeMaximizeVisualChanged)
    Q_PROPERTY(bool nativeMaximizePressed READ nativeMaximizePressed NOTIFY nativeMaximizeVisualChanged)

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

    [[nodiscard]] bool nativeMaximizeHovered() const noexcept
    {
        return _nativeMaximizeVisual != contour::platform::CaptionButtonVisual::Rest;
    }
    [[nodiscard]] bool nativeMaximizePressed() const noexcept
    {
        return _nativeMaximizeVisual == contour::platform::CaptionButtonVisual::Pressed;
    }
    /// What the caption adapter reports, as WindowController's binding would hear it.
    void setNativeMaximizeVisual(contour::platform::CaptionButtonVisual visual)
    {
        _nativeMaximizeVisual = visual;
        emit nativeMaximizeVisualChanged();
    }

    /// Mirrors WindowController::setCaptionButtonRect(): records the rectangle and its owner by kind.
    Q_INVOKABLE void setCaptionButtonRect(
        QObject* owner, int kind, qreal x, qreal y, qreal width, qreal height)
    {
        captionRects[kind] = QRectF { x, y, width, height };
        captionOwners[kind] = owner;
    }
    /// Mirrors WindowController::clearCaptionButtonRect(): records the call, and forgets the rectangle when
    /// @p owner reported it last.
    Q_INVOKABLE void clearCaptionButtonRect(QObject* owner, int kind)
    {
        clearedCaptionKinds.push_back(kind);
        if (captionOwners.value(kind) != owner)
            return;
        captionRects.remove(kind);
        captionOwners.remove(kind);
    }

    QHash<int, QRectF> captionRects {};      ///< The rectangles WindowControls.qml last reported, by kind.
    QHash<int, QObject*> captionOwners {};   ///< The control that reported each rectangle last, by kind.
    std::vector<int> clearedCaptionKinds {}; ///< Every clearCaptionButtonRect() kind, in order.

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
    Q_INVOKABLE [[nodiscard]] QString processToken() const { return contour::test::mockProcessToken(); }
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
    void tabContextMenuRequested(int /*index*/);
    void newTabMenuModelChanged();
    void newTabMenuRequested();
    void nativeMaximizeVisualChanged();

  private:
    QVariantList _tabContextMenuModel {};
    QVariantList _newTabMenuModel {};
    contour::platform::CaptionButtonVisual _nativeMaximizeVisual =
        contour::platform::CaptionButtonVisual::Rest;
    int _count = 3;
    int _activeTabIndex = 0;
    bool _titleBarVisible = true;
    int _openSessions = 1;
};

// Defined with the kit cases further down; the cases between here and there read it too.
[[nodiscard]] inline qreal chromeToken(QQmlEngine& engine, char const* group, char const* name);

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
[[nodiscard]] inline QmlHost buildQmlHost(QQmlEngine& engine, QString const& source, QString const& name)
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
[[nodiscard]] inline QmlHost makeTabItemHost(
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

/// Hosts TabStrip.qml at the top of a real Window, which its menus need: a Popup opens into its window's
/// overlay, and a strip created bare has none.
///
/// @param engine     The engine to build in.
/// @param controller The mock the strip binds to.
/// @param chrome     The chrome row, font and scheme to install.
/// @return The host Window; the strip is reachable as its `strip` property.
[[nodiscard]] inline QmlHost makeTabStripHost(QQmlEngine& engine,
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
[[nodiscard]] inline QmlHost makeTabStripHost(QQmlEngine& engine, MockTabController& controller)
{
    return makeTabStripHost(engine, controller, contour::test::defaultChromeSetup());
}

/// The item @p menu shows at @p index, or null.
[[nodiscard]] inline QQuickItem* menuItemAt(QObject& menu, int index)
{
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(&menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index));
    return item;
}

/// The items @p menu shows, top to bottom; a null slot is left out.
[[nodiscard]] inline std::vector<QQuickItem*> menuItems(QObject& menu)
{
    auto items = std::vector<QQuickItem*> {};
    for (auto const index: std::views::iota(0, menu.property("count").toInt()))
        if (auto* item = menuItemAt(menu, index); item != nullptr)
            items.push_back(item);
    return items;
}

/// The row of @p menu that opens the sub-menu titled @p title, or null.
[[nodiscard]] inline QQuickItem* subMenuRow(QObject& menu, QString const& title)
{
    for (auto* item: menuItems(menu))
        if (auto const* sub = item->property("subMenu").value<QObject*>();
            sub != nullptr && sub->property("title").toString() == title)
            return item;
    return nullptr;
}

/// Every item beneath @p root, in the VISUAL tree, named @p name: a Repeater's delegates (which the
/// swatches are) have a parent item but no QObject parent, so findChildren() cannot see them.
inline void collectNamed(QQuickItem const& root, QString const& name, std::vector<QQuickItem*>& out)
{
    for (auto* child: root.childItems())
    {
        if (child->objectName() == name)
            out.push_back(child);
        collectNamed(*child, name, out);
    }
}

/// The strip's one tab menu, opened through the controller over tab @p index.
[[nodiscard]] inline QObject* openTabMenu(QObject& host, MockTabController& controller, int index)
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
[[nodiscard]] inline TabColorMenu openTabColorMenu(QObject& menu)
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
[[nodiscard]] inline ChromeHost createChromeInWindow(QQmlEngine& engine, QSize size = QSize { 400, 300 })
{
    QQmlComponent component(&engine);
    component.setData(QStringLiteral("import QtQuick\n"
                                     "import QtQuick.Window\n"
                                     "import Contour.Ui\n"
                                     "Window {\n"
                                     "  width: %1; height: %2; visible: true\n"
                                     "  property alias chrome: chromeItem\n"
                                     "  SessionChrome { id: chromeItem; session: null }\n"
                                     "}\n")
                          .arg(size.width())
                          .arg(size.height())
                          .toUtf8(),
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
[[nodiscard]] inline std::unique_ptr<MockSession> createScrollableSession()
{
    auto session = std::make_unique<MockSession>();
    session->setScrollbarVisible(true);
    session->setHistoryLineCount(1000);
    return session;
}

[[nodiscard]] inline QPoint centreOf(QQuickItem const& item);

/// The tick items a SessionChrome's mark layer currently shows, top first.
[[nodiscard]] inline std::vector<QQuickItem*> markTicksOf(QQuickItem const& layer)
{
    auto ticks = std::vector<QQuickItem*> {};
    for (auto* item: layer.childItems())
        if (item->objectName() == QStringLiteral("scrollbarMark"))
            ticks.push_back(item);
    std::ranges::sort(ticks, std::ranges::less {}, &QQuickItem::y);
    return ticks;
}

/// A tick as TerminalSession publishes it.
[[nodiscard]] inline QVariantMap markEntry(double position,
                                           vtbackend::ScrollbarMarkKind kind,
                                           qint64 targetLine)
{
    return QVariantMap { { QStringLiteral("position"), position },
                         { QStringLiteral("kind"), static_cast<int>(kind) },
                         { QStringLiteral("style"),
                           static_cast<int>(vtbackend::scrollbarMarkKindRow(kind).style) },
                         { QStringLiteral("targetLine"), targetLine } };
}

/// One colour per ScrollbarMarkKind, in enumerator order: Command, UserMark, SearchMatch, Running, Failure.
[[nodiscard]] inline QVariantList markColors()
{
    return QVariantList { QVariant::fromValue(QColor(Qt::gray)),
                          QVariant::fromValue(QColor(Qt::cyan)),
                          QVariant::fromValue(QColor(Qt::magenta)),
                          QVariant::fromValue(QColor(Qt::yellow)),
                          QVariant::fromValue(QColor(Qt::red)) };
}

/// The centre of @p item, in its window's coordinates.
[[nodiscard]] inline QPoint centreOf(QQuickItem const& item)
{
    return item.mapToScene(QPointF(item.width() / 2, item.height() / 2)).toPoint();
}

// Defined with the kit and window-control cases further down; one unnamed namespace per translation unit.
[[nodiscard]] inline QQuickItem* findItemNamed(QQuickItem const& root, QString const& objectName);

/// The `Accessible` attached object of @p item, or null when the QML never attached one (or @p item is
/// null).
[[nodiscard]] inline QObject* accessibleOf(QObject const* item)
{
    if (item == nullptr)
        return nullptr;
    for (auto* child: item->findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child;
    return nullptr;
}

/// The name an item announces, read from its `Accessible` attached object; empty without one.
[[nodiscard]] inline QString accessibleNameOf(QObject const& item)
{
    auto const* attached = accessibleOf(&item);
    return attached != nullptr ? attached->property("name").toString() : QString {};
}

/// @return The chrome style installed on @p engine, as the provider it is.
[[nodiscard]] inline contour::window::UiStyleProvider& providerOf(QQmlEngine& engine)
{
    auto* provider = qobject_cast<contour::window::UiStyleProvider*>(
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>());
    REQUIRE(provider != nullptr);
    return *provider;
}

/// The chrome setup the pane card cases default to: the MacOS row, Comfortable.
[[nodiscard]] inline contour::test::ChromeSetup paneCardSetup()
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::MacOS;
    return setup;
}

/// What a pane reads of its window's controller (Main.qml's `win`): the attached host and the chords.
class MockPaneWindowController: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString hostLabel READ hostLabel CONSTANT)

  public:
    /// The host the window is attached to; empty for the local daemon.
    [[nodiscard]] QString hostLabel() const { return _hostLabel; }
    /// Names another host, or none; before the pane tree is shown, as the property is constant.
    void setHostLabel(QString label) { _hostLabel = std::move(label); }
    /// Mirrors WindowController::shortcutText(): ClosePane's default chord, nothing for any other; counted.
    [[nodiscard]] Q_INVOKABLE QString shortcutText(QString const& commandName) const
    {
        ++shortcutTextCalls;
        return commandName == QStringLiteral("ClosePane") ? QStringLiteral("Ctrl+Shift+X") : QString {};
    }
    mutable int shortcutTextCalls = 0; ///< How often a pane asked for a chord.
    /// Mirrors WindowController::canCloseWindow(): a pane's end never closes the test's window.
    [[nodiscard]] Q_INVOKABLE bool canCloseWindow() const { return _closable; }

    /// Mirrors WindowController::dropZoneAt() through the same pure decision (resolveDropZone), for a tab
    /// from another tab of a window that @ref split says can split. Records what the pane card asked.
    [[nodiscard]] Q_INVOKABLE QVariant
    dropZoneAt(QObject*, int payload, qreal x, qreal y, qreal width, qreal height, QString const& tabPayload)
    {
        lastPayload = payload;
        lastTabPayload = tabPayload;
        // What the QML engine hands back for a call it could not make (a argument it cannot convert).
        if (answersUndefined)
            return {};
        return static_cast<int>(contour::window::resolveDropZone(
            contour::window::DropZoneInputs { .payload = static_cast<contour::window::DropPayload>(payload),
                                              .pane = { .width = width, .height = height },
                                              .at = { .x = x, .y = y },
                                              .split = split,
                                              .origin = contour::window::DropOrigin::OtherTab }));
    }
    /// Mirrors WindowController::dropTabOnPane(): records the zone.
    Q_INVOKABLE void dropTabOnPane(QObject*, int zone, QString const& tabPayload)
    {
        droppedZones.push_back(zone);
        droppedTabPayloads.push_back(tabPayload);
    }
    /// Mirrors WindowController::dropGhostLabel().
    [[nodiscard]] Q_INVOKABLE QString dropGhostLabel(int zone) const
    {
        auto const label = contour::window::dropZoneRowOf(static_cast<contour::window::DropZone>(zone)).label;
        return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
    }
    /// Mirrors WindowController::dropGhostRect().
    [[nodiscard]] Q_INVOKABLE QRectF dropGhostRect(int zone, qreal width, qreal height, qreal inset) const
    {
        auto const ghost = contour::window::dropGhostOf(
            static_cast<contour::window::DropZone>(zone), { .width = width, .height = height }, inset);
        return ghost ? QRectF { ghost->x, ghost->y, ghost->width, ghost->height } : QRectF {};
    }
    contour::window::SplitSupport split =
        contour::window::SplitSupport::Supported; ///< Whether a tab may split.
    bool answersUndefined = false; ///< Whether dropZoneAt() answers `undefined`, as a failed call does.
    int lastPayload = 0;           ///< The payload the pane card last asked about.
    QString lastTabPayload;        ///< The tab JSON the pane card last asked about.
    std::vector<int> droppedZones; ///< Every zone dropTabOnPane() was told, in order.
    std::vector<QString> droppedTabPayloads; ///< The tab JSON of each.

  private:
    QString _hostLabel = QStringLiteral("home");
    bool _closable = false;
};

/// A window that carries its controller as `win`, as Main.qml's does.
class PaneTestWindow: public QQuickWindow
{
    Q_OBJECT
    Q_PROPERTY(QObject* win READ win CONSTANT)

  public:
    [[nodiscard]] QObject* win() noexcept { return &_controller; }
    /// @return The controller the window carries, for a test to configure and inspect.
    [[nodiscard]] MockPaneWindowController& controller() noexcept { return _controller; }

  private:
    MockPaneWindowController _controller;
};

/// A pane tree shown as Main.qml shows the active tab's: PaneNode.qml filling a window, told the tab's
/// visible pane count and the window's header reveal.
struct PaneTreeHost
{
    QQmlEngine engine;
    MockTabController controller;
    PaneTestWindow window;
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

/// Opens the find bar of @p card's pane, which the test cannot go on without.
[[nodiscard]] inline QObject& openFindBarOf(QQuickItem const& card)
{
    auto* bar = card.findChild<QObject*>(QStringLiteral("searchBar"));
    REQUIRE(bar != nullptr);
    QMetaObject::invokeMethod(bar, "open");
    QCoreApplication::processEvents();
    return *bar;
}

/// The item the find bar @p bar is placed in.
[[nodiscard]] inline QQuickItem* parentOf(QObject const& bar)
{
    return bar.property("parent").value<QQuickItem*>();
}

using contour::test::waitForPopupClosed;

using contour::test::waitForPopupOpened;

/// What a palette test's configuration and window hold: its profiles, its layouts and its tabs.
struct PaletteInventory
{
    std::vector<std::string> profiles { "dark", "light" }; ///< The configured profiles (the whole map).
    std::vector<std::string> layouts { "split-dev" };      ///< The saved layouts.
    std::vector<std::string> tabs {};                      ///< The open tabs' titles.
};

/// The configuration @p inventory describes: its profile and layout maps assigned whole.
[[nodiscard]] inline contour::config::Config paletteConfigOf(PaletteInventory const& inventory)
{
    auto config = contour::config::Config {};
    auto profiles = std::remove_cvref_t<decltype(config.profiles.value())> {};
    for (auto const& name: inventory.profiles)
        profiles.insert_or_assign(name, contour::config::TerminalProfile {});
    config.profiles.value() = std::move(profiles);
    auto layouts = std::remove_cvref_t<decltype(config.layouts.value())> {};
    for (auto const& name: inventory.layouts)
        layouts.insert_or_assign(name, contour::config::Layout {});
    config.layouts.value() = std::move(layouts);
    return config;
}

/// A stand-in for WindowController exposing exactly the command-palette surface CommandPalette.qml
/// binds: the model, and the runCommand() the popup calls when a row is picked.
class MockPaletteController: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)

  public:
    /// @param inventory The profiles, layouts and tabs the palette's live sources offer.
    explicit MockPaletteController(PaletteInventory const& inventory = {}):
        _history { 5 },
        _config { paletteConfigOf(inventory) },
        _tabs { inventory.tabs },
        _tabCommands { _tabs },
        _profileCommands { _config },
        _layoutCommands { _config },
        _model { std::make_unique<contour::window::CommandPaletteModel>(
            _history,
            std::vector<contour::command::CommandSource const*> {
                &_tabCommands, &_profileCommands, &_layoutCommands, &_actionCommands },
            contour::command::ShortcutStyle::Text) }
    {
        _history.record("TogglePaneZoom");
        _model->refresh(contour::command::shortcutIndex(contour::config::defaultInputMappings(),
                                                        contour::command::ShortcutStyle::Text));
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
    contour::config::Config _config;
    contour::test::StubTabs _tabs;
    contour::command::TabCommandSource _tabCommands;
    contour::command::ProfileCommandSource _profileCommands;
    contour::command::LayoutCommandSource _layoutCommands;
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

/// The host window's size when a test names none.
constexpr auto DefaultPaletteHostSize = QSize { 800, 600 };

/// Builds the host, opens the palette, and pumps the event loop so the Popup materializes its content.
/// @param setup The chrome row the palette is drawn with.
/// @param size  The host window's size.
[[nodiscard]] inline PaletteHost openPalette(QQmlEngine& engine,
                                             MockPaletteController& controller,
                                             contour::test::ChromeSetup const& setup,
                                             QSize size = DefaultPaletteHostSize)
{
    // The popup's background is a PopupSurface, which takes its shape and its drop shadow from
    // the design tokens -- so the engine needs them, as every other chrome component's does.
    contour::test::installChromeStyle(engine, setup);
    engine.rootContext()->setContextProperty("paletteController", &controller);

    QQmlComponent component(&engine);
    component.setData(QStringLiteral("import QtQuick\n"
                                     "import QtQuick.Window\n"
                                     "import Contour.Ui\n"
                                     "Window {\n"
                                     "  id: host\n"
                                     "  width: %1; height: %2; visible: true\n"
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
                                     "}\n")
                          .arg(size.width())
                          .arg(size.height())
                          .toUtf8(),
                      QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/PaletteTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    auto host = PaletteHost {};
    {
        if (!component.isReady())
        {
            WARN("PaletteTestHost errors: " << component.errorString().toStdString());
            return host;
        }
    }

    host.window.reset(component.create());
    if (host.window == nullptr)
    {
        WARN("PaletteTestHost create errors: " << component.errorString().toStdString());
        return host;
    }

    host.palette = host.window->property("commandPaletteItem").value<QObject*>();
    if (host.palette == nullptr)
        return host;

    // A Popup builds its contentItem lazily; opening it is what materializes the filter field and the
    // list, which is the state every assertion below is about. Opened once its enter transition is over.
    QMetaObject::invokeMethod(host.palette, "open");
    QCoreApplication::processEvents();
    waitForPopupOpened(*host.palette);
    return host;
}

/// Builds the host under the default chrome row (see the overload).
[[nodiscard]] inline PaletteHost openPalette(QQmlEngine& engine, MockPaletteController& controller)
{
    return openPalette(engine, controller, contour::test::defaultChromeSetup());
}

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

    /// Records the name the dialog replaced.
    Q_INVOKABLE void replaceLayout(QString const& name) { replaced << name; }
    QStringList replaced;

    /// Mirrors WindowController::checkLayoutName() against @c existing.
    [[nodiscard]] Q_INVOKABLE QVariantMap checkLayoutName(QString const& name, bool replace) const
    {
        auto const trimmed = name.trimmed();
        return contour::window::WindowController::layoutCheckMap(
            contour::window::checkLayoutName(trimmed.toStdString(),
                                             existing,
                                             replace ? contour::window::LayoutReplace::Replace
                                                     : contour::window::LayoutReplace::Keep),
            trimmed);
    }
    std::vector<std::string> existing; ///< The layouts already saved.

    /// Mirrors WindowController::layoutSummary(), over @c tabs and @c panes.
    [[nodiscard]] Q_INVOKABLE QString layoutSummary() const
    {
        return QString::fromStdString(contour::window::layoutSummaryText(tabs, panes));
    }
    int tabs = 3;  ///< The window's tabs, as layoutSummary() counts them.
    int panes = 5; ///< Their panes.

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
    [[nodiscard]] QQuickItem* nameField() const { return part(QStringLiteral("saveLayoutNameField")); }

    /// The dialog's part named @p name, found through the visual tree (what a body slot holds may have no
    /// QObject parent); null for none.
    [[nodiscard]] QQuickItem* part(QString const& name) const
    {
        if (dialog == nullptr)
            return nullptr;
        if (auto* direct = dialog->findChild<QQuickItem*>(name))
            return direct;
        auto const* content = dialog->property("contentItem").value<QQuickItem*>();
        if (content == nullptr)
            return nullptr;
        auto const found = itemsNamed(*content, name);
        return found.empty() ? nullptr : found.front();
    }
};

/// Builds the host and locates the dialog (not yet opened — the tests open it via the controller's
/// request, which is the path Main.qml takes).
[[nodiscard]] inline SaveLayoutHost makeSaveLayoutHost(
    QQmlEngine& engine,
    MockSaveLayoutController& controller,
    contour::test::ChromeSetup const& setup = contour::test::defaultChromeSetup())
{
    // The popup's background is a PopupSurface, which takes its shape and its drop shadow from
    // the design tokens -- so the engine needs them, as every other chrome component's does.
    contour::test::installChromeStyle(engine, setup);
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
inline void openSaveLayoutPrompt(MockSaveLayoutController& controller)
{
    controller.beginSaveLayoutPrompt();
    QCoreApplication::processEvents();
}

/// A clearance of @p rows rows of @p columns cells of 8 × 20 at the pane's top left, each with @p blank blank
/// cells at its right end, avoiding @p avoid.
[[nodiscard]] inline QVariantMap linkClearance(int columns, int rows, int blank, QVariantList avoid = {})
{
    return QVariantMap { { QStringLiteral("marginLeft"), 0.0 },
                         { QStringLiteral("marginTop"), 0.0 },
                         { QStringLiteral("cellWidth"), 8.0 },
                         { QStringLiteral("cellHeight"), 20.0 },
                         { QStringLiteral("columns"), columns },
                         { QStringLiteral("trailingBlank"), QVariantList(rows, QVariant(blank)) },
                         { QStringLiteral("avoidRows"), avoid } };
}

/// The scene-coordinate rectangle @p item occupies.
[[nodiscard]] inline QRectF sceneRectOf(QQuickItem const& item)
{
    return { item.mapToScene(QPointF(0, 0)), QSizeF(item.width(), item.height()) };
}

/// Builds the REAL TitleBar with its window controls enabled, under the chrome @p setup.
///
/// The one place TitleBar is instantiated for a test, so a change to how it is instantiated -- a new
/// required property, say -- lands on every test at once rather than on whichever host builder the
/// author happened to notice. The bar sits at z:1 as Main.qml puts it, which is what lets a caller
/// stack the REAL ResizeBorder (z:1000) over it via @p extraChildren.
///
/// @param engine             The engine to build in. Both style providers are installed on it.
/// @param controller         The mock the bar binds to.
/// @param setup              The chrome row (what decides the extents) and the window-control style (what
///                           decides side, order and shape).
/// @param extraChildren      QML appended after the bar, e.g. a ResizeBorder over it.
/// @param width              The host window's width. Wide by default, so the tab strip's 70% cap
///                           leaves the drag region real width and "left of the tabs" and "right of
///                           the drag region" are genuinely different places to be.
/// @return The host Window; the bar is reachable via findChild("titleBar").
[[nodiscard]] inline QmlHost makeTitleBarHostWith(QQmlEngine& engine,
                                                  MockTabController& controller,
                                                  contour::test::ChromeSetup const& setup,
                                                  QString const& extraChildren = {},
                                                  int width = 900)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine, setup);

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

/// makeTitleBarHostWith() for @p uiStyle resolved against HostPlatform::Other, with @p windowControlStyle.
[[nodiscard]] inline QmlHost makeTitleBarHost(
    QQmlEngine& engine,
    MockTabController& controller,
    contour::config::UiStyle uiStyle = contour::config::UiStyle::Native,
    contour::config::WindowControlStyle windowControlStyle = contour::config::WindowControlStyle::Windows,
    QString const& extraChildren = {},
    int width = 900)
{
    return makeTitleBarHostWith(engine,
                                controller,
                                contour::test::defaultChromeSetup(uiStyle, windowControlStyle),
                                extraChildren,
                                width);
}

/// Finds the first descendant of @p root satisfying @p predicate, in the VISUAL tree.
///
/// findChild() is not enough for anything a Repeater created: a delegate is given a parentItem but
/// no QObject parent, so it never appears among its ancestors' QObject children. The window controls
/// are exactly that, being one delegate per row of `windowControls.leadingButtons` or
/// `trailingButtons`, and so is whatever a Loader loads.
///
/// @param root      The item to search beneath.
/// @param predicate What to look for.
/// @return The item, or nullptr when no descendant satisfies @p predicate.
[[nodiscard]] inline QQuickItem* findDescendant(QQuickItem const& root, auto&& predicate)
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
[[nodiscard]] inline QQuickItem* findItemNamed(QQuickItem const& root, QString const& objectName)
{
    return findDescendant(root, [&](QQuickItem const& item) { return item.objectName() == objectName; });
}

/// Activates @p window and waits until it is.
inline void activate(QQuickWindow& window)
{
    window.requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(&window));
}

/// Creates the Banner under @p engine with @p initialProperties.
[[nodiscard]] inline std::unique_ptr<QObject> createBanner(QQmlEngine& engine,
                                                           QVariantMap const& initialProperties)
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
[[nodiscard]] inline qreal chromeToken(QQmlEngine& engine, char const* group, char const* name)
{
    auto* style = engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto* tokens = style->property(group).value<QObject*>();
    REQUIRE(tokens != nullptr);
    return tokens->property(name).toReal();
}

/// The Terminal row painted with the built-in color scheme @p schemeName.
[[nodiscard]] inline contour::test::ChromeSetup terminalRowSetup(std::string_view schemeName)
{
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
    auto setup = contour::test::defaultChromeSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.scheme = contour::config::chromeSchemeColorsOf(palette);
    return setup;
}

/// The contrast ratio of @p label's text colour on @p fill.
[[nodiscard]] inline double contrastOn(QObject const* label, vtbackend::RGBColor fill)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(label->property("color").value<QColor>()),
                                    fill);
}

/// The colour an item paints as, read from its `color` property.
[[nodiscard]] inline vtbackend::RGBColor fillOf(QObject const* item)
{
    return contour::platform::toRGBColor(item->property("color").value<QColor>());
}

/// The light/dark name of @p appearance, for a test's INFO.
[[nodiscard]] inline std::string_view appearanceName(vtbackend::ColorPreference appearance) noexcept
{
    return appearance == vtbackend::ColorPreference::Light ? "light" : "dark";
}

/// Creates the Toast under @p engine with @p initialProperties.
[[nodiscard]] inline std::unique_ptr<QObject> createToast(QQmlEngine& engine,
                                                          QVariantMap const& initialProperties)
{
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/Toast.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto toast = std::unique_ptr<QObject>(component.createWithInitialProperties(initialProperties));
    REQUIRE(toast != nullptr);
    QCoreApplication::processEvents();
    return toast;
}

/// Creates the KitDialog under @p engine, parented to @p parent, with @p initialProperties, and opens it.
[[nodiscard]] inline std::unique_ptr<QObject> openKitDialog(QQmlEngine& engine,
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

/// @return @p top composited over the opaque @p ground.
[[nodiscard]] inline QColor over(QColor const& top, QColor const& ground)
{
    auto const alpha = top.alphaF();
    auto const mix = [alpha](float a, float b) {
        return (a * alpha) + (b * (1.0F - alpha));
    };
    return QColor::fromRgbF(
        mix(top.redF(), ground.redF()), mix(top.greenF(), ground.greenF()), mix(top.blueF(), ground.blueF()));
}

/// The contrast ratio of the opaque colours @p a and @p b.
[[nodiscard]] inline double contrastOf(QColor const& a, QColor const& b)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(a), contour::platform::toRGBColor(b));
}

/// The `color` property of @p item.
[[nodiscard]] inline QColor colorOf(QQuickItem const& item)
{
    return item.property("color").value<QColor>();
}

/// Ends @p session the way an exited shell does: its code named, Restart offered.
inline void endWithExitCode(contour::test::MockSession& session)
{
    session.setEnd(static_cast<int>(contour::session::EndReason::Exited),
                   QStringLiteral("Process exited with code 1."),
                   static_cast<int>(contour::session::FooterAction::Restart));
}

/// Clicks the middle of @p item in @p host's window, once the item has stopped moving.
///
/// A footer's buttons sit in a row that places them on polish, a frame after the text that sizes them
/// changes, so a click aimed at where a button was a frame ago lands beside it.
inline void clickOn(PaneTreeHost& host, QQuickItem const& item)
{
    auto const centreOf = [&] {
        return item.mapToScene(QPointF { item.width() / 2, item.height() / 2 });
    };
    auto last = centreOf();
    REQUIRE(QTest::qWaitFor(
        [&] {
            QTest::qWait(PaneTreeHost::QuietFrameMs);
            auto const now = centreOf();
            auto const stable = now == last;
            last = now;
            return stable;
        },
        PaneTreeHost::SettleTimeoutMs));
    auto const centre = last.toPoint();
    QTest::mouseClick(&host.window, Qt::LeftButton, {}, centre);
    QCoreApplication::processEvents();
}

using contour::session::PermissionDecision;

using contour::session::PermissionMemory;

/// What a 2.4 MB paste of two shown lines asks, as the session publishes it.
[[nodiscard]] inline QVariantMap pasteQuestion(int id)
{
    auto const preview =
        contour::session::PastePreview { .lines = { "line 0", "line 1" }, .moreLines = 48'211 };
    return contour::session::permissionPromptMap(contour::session::bigPastePrompt("zsh", 2'400'000, preview),
                                                 id);
}

/// What a capture of 24 lines asks, as the session publishes it.
/// @param subject The session's title, as the program set it.
[[nodiscard]] inline QVariantMap captureQuestion(int id, std::string_view subject = "zsh")
{
    return contour::session::permissionPromptMap(
        contour::session::bufferCapturePrompt(subject, vtbackend::LineCount(24)), id);
}

/// Every item of @p dialog's content named @p name, top to bottom. The visual tree rather than
/// findChildren(): a Repeater's delegates, and what a body slot holds, have a parent item but not always a
/// QObject parent.
[[nodiscard]] inline QList<QQuickItem*> dialogPartsNamed(QObject const& dialog, QString const& name)
{
    auto parts = QList<QQuickItem*> {};
    if (auto const* content = dialog.property("contentItem").value<QQuickItem*>())
        for (auto* part: itemsNamed(*content, name))
            parts.append(part);
    // In their parent's order -- the order a layout places them in, settled or not.
    std::ranges::sort(parts, {}, [](QQuickItem* part) {
        auto const* parent = part->parentItem();
        return parent != nullptr ? parent->childItems().indexOf(part) : 0;
    });
    return parts;
}

/// A SessionChrome over @p session in a shown window, and its permission dialog.
struct PermissionChrome
{
    QQuickWindow window;
    QQmlEngine engine;
    MockTabController controller;
    std::unique_ptr<QObject> chrome;
    QObject* dialog = nullptr;

    explicit PermissionChrome(MockSession& session,
                              contour::test::ChromeSetup const& setup = contour::test::defaultChromeSetup())
    {
        window.resize(1024, 640);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        engine.rootContext()->setContextProperty("terminalSessions", &controller);
        contour::test::installChromeStyle(engine, setup);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SessionChrome.qml")));
        while (component.status() == QQmlComponent::Loading)
            QCoreApplication::processEvents();
        INFO(component.errorString().toStdString());
        REQUIRE(component.isReady());
        QVariantMap initial;
        initial.insert("session", QVariant::fromValue(static_cast<QObject*>(&session)));
        initial.insert("parent", QVariant::fromValue(window.contentItem()));
        chrome.reset(component.createWithInitialProperties(initial));
        REQUIRE(chrome != nullptr);
        dialog = chrome->findChild<QObject*>(QStringLiteral("permissionDialog"));
        REQUIRE(dialog != nullptr);
        QCoreApplication::processEvents();
    }

    /// Whether the dialog is open.
    [[nodiscard]] bool opened() const { return dialog->property("opened").toBool(); }

    /// Waits until the dialog opened, or closed.
    [[nodiscard]] bool waitOpened(bool open) const
    {
        return QTest::qWaitFor(
            [&] { return dialog->property("visible").toBool() == open && opened() == open; }, 2000);
    }

    /// The dialog's part named @p name.
    [[nodiscard]] QQuickItem* part(QString const& name) const
    {
        auto const parts = dialogPartsNamed(*dialog, name);
        REQUIRE(parts.size() == 1);
        return parts.front();
    }

    /// The terminal the dialog hands the focus back to.
    [[nodiscard]] QQuickItem* displayItem() const
    {
        return chrome->property("displayItem").value<QQuickItem*>();
    }
};

/// The labels a dialog shows its lines in, top to bottom.
[[nodiscard]] inline QList<QQuickItem*> permissionLines(QObject const& dialog)
{
    return dialogPartsNamed(dialog, QStringLiteral("permissionLine"));
}

} // namespace contour::test::qmlSupport
