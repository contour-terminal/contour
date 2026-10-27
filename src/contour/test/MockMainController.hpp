// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/TabBarMode.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/ChromeFocus.hpp>
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/SaveLayoutCheck.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/client/ReleaseDrift.hpp>

namespace contour::test
{

/// The colour row 0 wears under snapshot::TabDecor::EveryBadge.
inline constexpr auto DecorTabColor = vtbackend::RGBColor { 0xE74856u };

/// One badged row under snapshot::TabDecor::EveryBadge.
struct DecorBadge
{
    window::TabBadgeKind kind = window::TabBadgeKind::None; ///< The badge the row shows.
    int progress = -1;                                      ///< Its progress percent; -1 for none.
};

/// The badges of rows 1, 2, ... under snapshot::TabDecor::EveryBadge: one per kind, Working at 40 %.
inline constexpr auto DecorBadges = std::array {
    DecorBadge { .kind = window::TabBadgeKind::Output, .progress = -1 },
    DecorBadge { .kind = window::TabBadgeKind::Working, .progress = 40 },
    DecorBadge { .kind = window::TabBadgeKind::Blocked, .progress = -1 },
    DecorBadge { .kind = window::TabBadgeKind::Done, .progress = -1 },
    DecorBadge { .kind = window::TabBadgeKind::Error, .progress = -1 },
};
static_assert(DecorBadges.size() + 1 == window::TabBadgeKindCount, "one decor row per badge kind");

/// How long row 0's title is under snapshot::TabDecor::LongTitle: longer than any tab is wide.
inline constexpr auto DecorLongTitleLength = 200;

/// The shape a MockMainController is built in: what its window holds, fixed for its lifetime.
///
/// The defaults are the mock every Main.qml load case has always seen: two local tabs, a strip that is
/// always shown, and no pane tree or palette model.
struct MainWindowScene
{
    int tabCount = 2;                  ///< How many tabs the strip lists.
    QObject* rootPane = nullptr;       ///< The active tab's pane tree; borrowed, may be null.
    QObject* commandPalette = nullptr; ///< The command palette's model; borrowed, may be null.
    QString hostLabel {};              ///< The attached host; empty for a local window.
    QString hostingNotice {};          ///< The hosting notice the window starts with; empty for none.
    /// When the tab strip is shown, resolved against @c tabCount into the initial tabBarShouldShow.
    config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always;
    snapshot::TabDecor decor = snapshot::TabDecor::Plain; ///< What the tabs carry beyond their titles.
    int activeTab = 0;                                    ///< The active tab's index.
    /// How the attached host's connection stands; read only while @c hostLabel names one.
    window::HostChipState hostChipState = window::HostChipState::Connected;
    config::TabBarPosition tabBarPosition = config::TabBarPosition::Top; ///< The configured tab-bar edge.
    snapshot::SnapshotDrift drift = snapshot::SnapshotDrift::None;       ///< Whether the daemon drifted.
    snapshot::SnapshotFrame frame = snapshot::SnapshotFrame::ClientBand; ///< Who draws the title area.
    /// What the OS reserves at the band's leading edge for its own window controls while the chrome draws
    /// the band (macOS's traffic lights); 0 under the native frame, which draws them in a bar of its own.
    qreal nativeControlsInset = 0;
    snapshot::SecureDecor secure = snapshot::SecureDecor::None; ///< Which tabs wait for a password.
};

/// The client and daemon releases a drifted scene reports, as DaemonDriftModel_test assesses them.
inline constexpr auto DriftClientVersion = std::string_view { "0.8.2" };
inline constexpr auto DriftDaemonVersion = std::string_view { "0.8.1" };

/// Manager + WindowController in one mock: `terminalSessions` context property AND the object
/// Main.qml assigns to `win` (createWindowController returns this). The QAbstractListModel rows
/// feed the TitleBar's TabStrip like the production WindowController's model does.
class MockMainController: public MainWindowControllerMock
{
    Q_OBJECT
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    // Writable here so a test can let multimedia become ready after the window loaded, as it does in
    // production once the background probe finishes.
    Q_PROPERTY(bool multimediaReady READ multimediaReady NOTIFY multimediaReadyChanged)
    Q_PROPERTY(
        bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible NOTIFY titleBarVisibleChanged)
    // Mirrors WindowController's frame adapter: what the OS reserves for its own window controls, which
    // follows the frame (zero once show_title_bar hands the title area to the system).
    Q_PROPERTY(qreal nativeControlsInset READ nativeControlsInset NOTIFY nativeControlsInsetChanged)
    // Mirrors WindowController's macOS frame hints, which Main.qml's `flags` read; false (no hint) unless a
    // test sets them, as it does to load the real Main.qml under Qt::ExpandedClientAreaHint.
    Q_PROPERTY(bool needsExpandedClientArea READ needsExpandedClientArea WRITE setNeedsExpandedClientArea
                   NOTIFY frameHintsChanged)
    Q_PROPERTY(bool clearsTitleBarBackground READ clearsTitleBarBackground WRITE setClearsTitleBarBackground
                   NOTIFY frameHintsChanged)
    // Tab-strip placement + resolved visibility, mirroring the production WindowController properties
    // Main.qml binds. Writable here so a test can flip them and re-load Main.qml to assert the layout.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition WRITE setTabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(
        bool tabBarShouldShow READ tabBarShouldShow WRITE setTabBarShouldShow NOTIFY tabBarShouldShowChanged)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(QObject* activeTabRootPane READ activeTabRootPane CONSTANT)
    Q_PROPERTY(QObject* activeSession READ activeSession CONSTANT)
    // Mirrors WindowController's command-palette surface. Main.qml instantiates CommandPalette.qml
    // against this controller and Connections-targets its commandPaletteRequested signal, so the mock
    // must carry both — a missing signal is a QML warning, and the run-wide gate turns that into a
    // failure of the whole suite. A null model is what an unopened palette shows anyway.
    Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)
    // Ditto for the context-menu surface: Main.qml instantiates ActionContextMenu.qml against this
    // controller, binds its `entries` to contextMenuModel and Connections-targets contextMenuRequested.
    // An empty model is what a menu that has never been opened holds anyway.
    Q_PROPERTY(QVariantList contextMenuModel READ contextMenuModel NOTIFY contextMenuModelChanged)
    // Ditto for the title bar's own menu surface, which Main.qml instantiates alongside it.
    Q_PROPERTY(QVariantList titleBarContextMenuModel READ titleBarContextMenuModel NOTIFY
                   titleBarContextMenuModelChanged)
    // Mirrors WindowController's keyboard focus in the chrome, which Main.qml, the tab strip and the toolbar
    // read. Moved through focusTabWell() and leaveChrome(), as the production controller moves it.
    Q_PROPERTY(int chromeFocusArea READ chromeFocusArea NOTIFY chromeFocusChanged)
    Q_PROPERTY(int focusedTabIndex READ focusedTabIndex NOTIFY chromeFocusChanged)
    Q_PROPERTY(int focusedToolbarIndex READ focusedToolbarIndex NOTIFY chromeFocusChanged)
    // Mirrors WindowController's hosting notice; writable here so a test can raise it before the load.
    Q_PROPERTY(QString hostingNotice READ hostingNotice WRITE setHostingNotice NOTIFY hostingNoticeChanged)

  public:
    enum class Roles : std::uint16_t
    {
        DisplayRole = Qt::DisplayRole,
        TitleRole = Qt::UserRole + 1,
        ColorRole = Qt::UserRole + 2,
        IsActiveRole = Qt::UserRole + 3,
        PaneCountRole = Qt::UserRole + 4,
        SessionIdRole = Qt::UserRole + 5,
        RawTitleRole = Qt::UserRole + 6,
        ZoomedRole = Qt::UserRole + 7,
        ProgressStateRole = Qt::UserRole + 8,
        ProgressPercentageRole = Qt::UserRole + 9,
        BadgeRole = Qt::UserRole + 10,
        BadgeTextRole = Qt::UserRole + 11,
        BadgeProgressRole = Qt::UserRole + 12,
        SecureRole = Qt::UserRole + 13,
    };

    /// @param scene What the window holds; see MainWindowScene. Its pointers must outlive the mock.
    explicit MockMainController(MainWindowScene scene = {}):
        _scene { std::move(scene) },
        _tabs { std::ranges::to<std::vector<int>>(std::views::iota(0, _scene.tabCount)) },
        _nextTabId { _scene.tabCount },
        _activeTabId { _scene.activeTab >= 0 && _scene.activeTab < _scene.tabCount ? _scene.activeTab : -1 },
        _tabBarPosition { static_cast<int>(_scene.tabBarPosition) },
        _tabBarShouldShow { config::isTabBarShown(_scene.tabBarVisibility, _scene.tabCount) },
        _titleBarVisible { _scene.frame == snapshot::SnapshotFrame::NativeFrame },
        _hostingNotice { _scene.hostingNotice }
    {
        attachedHost = _scene.hostLabel;
        setHostChipState(static_cast<int>(_scene.hostChipState));
        if (_scene.drift == snapshot::SnapshotDrift::Shown)
            _drift.show(vthost::client::assessDrift(
                DriftClientVersion, DriftDaemonVersion, vthost::client::LayoutAfterRestart::Lost));
    }

    /// @return Whether the scene's daemon drifted, so Main.qml is given the drift model.
    [[nodiscard]] bool hasDrift() const noexcept { return _scene.drift == snapshot::SnapshotDrift::Shown; }

    /// @return The drift model Main.qml reads as `daemonDrift` when the scene drifted.
    [[nodiscard]] window::DaemonDriftModel& daemonDrift() noexcept { return _drift; }

    [[nodiscard]] int activeTabIndex() const noexcept { return rowOf(_activeTabId); }
    /// @return The focused window::ChromeArea as its underlying value.
    [[nodiscard]] int chromeFocusArea() const noexcept { return static_cast<int>(_chromeArea); }
    /// @return The focused tab's row while the tab well has focus, else -1.
    [[nodiscard]] int focusedTabIndex() const noexcept { return rowOf(_focusedTabId); }
    /// @return The identity of the focused tab while the tab well has focus, else -1. Rows move under it;
    /// this does not.
    [[nodiscard]] int focusedTabId() const noexcept { return _focusedTabId; }
    /// @return The identity of the tab on row @p row, or -1 when there is none.
    [[nodiscard]] int tabIdAt(int row) const noexcept
    {
        return row >= 0 && std::cmp_less(row, _tabs.size()) ? _tabs[static_cast<std::size_t>(row)] : -1;
    }
    /// @return The focused toolbar item's index while the toolbar has focus, else -1.
    [[nodiscard]] int focusedToolbarIndex() const noexcept { return _focusedToolbar; }
    /// Puts keyboard focus in the tab well, on tab @p tab, as the controller does for Ctrl+F6.
    /// @param tab The row of the focused tab.
    void focusTabWell(int tab)
    {
        _chromeArea = window::ChromeArea::Tabs;
        _focusedTabId = tabIdAt(tab);
        _focusedToolbar = -1;
        emit chromeFocusChanged();
    }
    /// Puts keyboard focus in the toolbar, on item @p item, as the controller does for the toolbar chord.
    /// @param item The index of the focused toolbar item.
    void focusToolbarItem(int item)
    {
        _chromeArea = window::ChromeArea::Toolbar;
        _focusedTabId = -1;
        _focusedToolbar = item;
        emit chromeFocusChanged();
    }
    /// Mirrors WindowController::leaveChrome(): what a pointer press or the terminal taking focus calls.
    Q_INVOKABLE void leaveChrome()
    {
        calls << QStringLiteral("leaveChrome");
        _chromeArea = window::ChromeArea::Terminal;
        _focusedTabId = -1;
        _focusedToolbar = -1;
        emit chromeFocusChanged();
    }
    /// Records what the window reports it shows: how Main.qml tells the controller what there is to visit.
    Q_INVOKABLE void setChromePresence(bool tabsShown, int toolbarItems)
    {
        reportedTabsShown = tabsShown;
        reportedToolbarItems = toolbarItems;
    }
    /// Records a key the chrome offered, and answers as the controller does for one it handles.
    /// @return Whether the key counts as handled: true unless a test set @c chromeKeysHandled to false.
    Q_INVOKABLE [[nodiscard]] bool chromeKey(int qtKey, int qtModifiers)
    {
        chromeKeys.emplace_back(qtKey, qtModifiers);
        if (mirrorsChromeKeys && _chromeArea == window::ChromeArea::Tabs)
        {
            // The two keys the controller answers by changing the model, whose announcements Main.qml must
            // not mistake for the user leaving the chrome.
            auto const alt = (qtModifiers & Qt::AltModifier) != 0;
            if (alt && (qtKey == Qt::Key_Left || qtKey == Qt::Key_Right))
            {
                // The focus is on the tab, so it goes with it: nothing is written here, as in the controller.
                auto const row = focusedTabIndex();
                auto const target = row + (qtKey == Qt::Key_Left ? -1 : 1);
                if (target >= 0 && std::cmp_less(target, _tabs.size()))
                    moveTab(row, target);
                return true;
            }
            if (qtKey == Qt::Key_Escape)
            {
                leaveChrome();
                return true;
            }
        }
        return chromeKeysHandled;
    }
    /// Records the confirmed close of the tab at @p row.
    Q_INVOKABLE void confirmCloseTab(quint64 tab) { calls << QStringLiteral("confirmCloseTab:%1").arg(tab); }
    /// Mirrors WindowController::activateTab(): makes tab @p index the active one.
    void setActiveTab(int index)
    {
        auto const id = tabIdAt(index);
        if (id < 0 || _activeTabId == id)
            return;
        _activeTabId = id;
        emitActiveRoleChanged();
        announceActivation();
    }
    /// What a tab arriving in this window does to the rows, as the model announces an insert.
    enum class TabActivation : std::uint8_t
    {
        Keep,    ///< The active tab stays; the strip only gains a row.
        Activate ///< The new tab becomes the active one, as a tab moved in from another window does.
    };
    /// Mirrors WindowController::onTabAboutToBeAdded()/onTabAdded(): a new tab takes row @p row.
    /// @param row        Where the tab goes.
    /// @param activation Whether the model activates it.
    /// @return The identity of the new tab.
    int insertTab(int row, TabActivation activation)
    {
        auto const at = std::clamp(row, 0, static_cast<int>(_tabs.size()));
        auto const id = _nextTabId++;
        beginInsertRows(QModelIndex(), at, at);
        _tabs.insert(_tabs.begin() + at, id);
        endInsertRows();
        if (_chromeArea == window::ChromeArea::Tabs)
            emit chromeFocusChanged(); // the focused tab may have moved down a row
        if (activation == TabActivation::Activate)
        {
            _activeTabId = id;
            emitActiveRoleChanged();
            announceActivation();
        }
        else
            emitActiveRoleChanged();
        return id;
    }
    [[nodiscard]] QString const& hostingNotice() const noexcept { return _hostingNotice; }
    void setHostingNotice(QString notice)
    {
        if (_hostingNotice != notice)
        {
            _hostingNotice = std::move(notice);
            emit hostingNoticeChanged();
        }
    }
    /// Mirrors WindowController::dismissHostingNotice(): what the bar's Dismiss button calls.
    Q_INVOKABLE void dismissHostingNotice() { setHostingNotice({}); }
    [[nodiscard]] bool multimediaReady() const noexcept { return _multimediaReady; }
    void setMultimediaReady(bool ready)
    {
        if (_multimediaReady != ready)
        {
            _multimediaReady = ready;
            emit multimediaReadyChanged();
        }
    }
    /// @return Whether the system frame draws the title area (show_title_bar); false, frameless CSD, by
    /// default.
    [[nodiscard]] bool titleBarVisible() const noexcept { return _titleBarVisible; }
    /// What ToggleTitleBar does: hands the title area to the system frame, or takes it back.
    void setTitleBarVisible(bool visible)
    {
        if (_titleBarVisible != visible)
        {
            _titleBarVisible = visible;
            emit titleBarVisibleChanged();
            emit nativeControlsInsetChanged();
        }
    }
    /// @return The scene's inset while the chrome draws the band, and 0 under the native frame.
    [[nodiscard]] qreal nativeControlsInset() const noexcept
    {
        return _titleBarVisible ? 0 : _scene.nativeControlsInset;
    }
    /// Moves the OS's window controls, as WindowController's frame adapter reports once the band's
    /// title-bar growth changed (Minimal's bare band detaches the compact toolbar: 84 -> 78 pt).
    /// @param inset What the OS reserves while the chrome draws the band.
    void setNativeControlsInset(qreal inset)
    {
        if (_scene.nativeControlsInset == inset)
            return;
        _scene.nativeControlsInset = inset;
        emit nativeControlsInsetChanged();
    }

    [[nodiscard]] bool needsExpandedClientArea() const noexcept { return _needsExpandedClientArea; }
    void setNeedsExpandedClientArea(bool needed)
    {
        if (_needsExpandedClientArea != needed)
        {
            _needsExpandedClientArea = needed;
            emit frameHintsChanged();
        }
    }
    [[nodiscard]] bool clearsTitleBarBackground() const noexcept { return _clearsTitleBarBackground; }
    void setClearsTitleBarBackground(bool clears)
    {
        if (_clearsTitleBarBackground != clears)
        {
            _clearsTitleBarBackground = clears;
            emit frameHintsChanged();
        }
    }

    [[nodiscard]] int tabBarPosition() const noexcept { return _tabBarPosition; }
    void setTabBarPosition(int position)
    {
        if (_tabBarPosition != position)
        {
            _tabBarPosition = position;
            emit tabBarPositionChanged();
        }
    }
    [[nodiscard]] bool tabBarShouldShow() const noexcept { return _tabBarShouldShow; }
    void setTabBarShouldShow(bool show)
    {
        if (_tabBarShouldShow != show)
        {
            _tabBarShouldShow = show;
            emit tabBarShouldShowChanged();
        }
    }
    [[nodiscard]] int chromeHeight() const noexcept { return _chromeHeight; }
    void setChromeHeight(int height)
    {
        if (_chromeHeight != height)
        {
            _chromeHeight = height;
            emit chromeHeightChanged();
        }
    }
    [[nodiscard]] QObject* activeTabRootPane() const noexcept { return _scene.rootPane; }
    [[nodiscard]] QObject* activeSession() const noexcept { return nullptr; }
    [[nodiscard]] QObject* commandPalette() const noexcept { return _scene.commandPalette; }
    /// Records what the palette ran. ShowAbout is dispatched for real (WindowController::runCommand ->
    /// the manager -> showAbout()), so a test can watch a picked command open a dialog of its own.
    Q_INVOKABLE void runCommand(QString const& id)
    {
        ranCommands << id;
        if (id == QStringLiteral("ShowAbout"))
            showAbout();
    }
    QStringList ranCommands; ///< The ids runCommand() was called with, oldest first.
    /// Mirrors WindowController::openCommandPalette(): what the manager calls for the
    /// OpenCommandPalette action, and what makes Main.qml's popup appear.
    Q_INVOKABLE void openCommandPalette() { emit commandPaletteRequested(); }

    [[nodiscard]] QVariantList contextMenuModel() const { return {}; }
    [[nodiscard]] QVariantList titleBarContextMenuModel() const { return {}; }
    Q_INVOKABLE void openTitleBarContextMenu() { emit titleBarContextMenuRequested(); }
    Q_INVOKABLE void triggerTitleBarContextMenuAction(int /*actionId*/) {}
    Q_INVOKABLE void triggerContextMenuAction(int) {}
    /// Mirrors WindowController::openContextMenu(): what the manager calls for the OpenContextMenu
    /// action, and what makes Main.qml pop the terminal's right-click menu.
    Q_INVOKABLE void openContextMenu() { emit contextMenuRequested(); }

    // The startup sequence Main.qml's Component.onCompleted drives, recorded for the order check.
    Q_INVOKABLE QObject* createWindowController()
    {
        calls << QStringLiteral("createWindowController");
        return this;
    }
    Q_INVOKABLE void bindWindow(QObject* osWindow)
    {
        calls << QStringLiteral("bindWindow");
        boundWindow = osWindow;
    }
    Q_INVOKABLE void createNewTab() { calls << QStringLiteral("createNewTab"); }
    Q_INVOKABLE void showInitial() { calls << QStringLiteral("showInitial"); }
    // No tear-off is staged in the plain startup test, so the window creates its own first tab.
    Q_INVOKABLE bool consumePendingTransplant(QObject*)
    {
        calls << QStringLiteral("consumePendingTransplant");
        return false;
    }
    // Not attached in these tests, so no daemon window is adopted (the window falls through).
    Q_INVOKABLE bool consumeAttachWindow(QObject*)
    {
        calls << QStringLiteral("consumeAttachWindow");
        return false;
    }
    // No startup layout is configured in these tests, so the window creates its own first tab.
    Q_INVOKABLE bool consumeDefaultLayout(QObject*)
    {
        calls << QStringLiteral("consumeDefaultLayout");
        return false;
    }

    Q_INVOKABLE void closeWindow() { calls << QStringLiteral("closeWindow"); }
    Q_INVOKABLE [[nodiscard]] bool canCloseWindow() const noexcept { return true; }
    Q_INVOKABLE void toggleTitleBar() {}
    /// Mirrors WindowController::tabColorPalette(): the colours the flyout offers, the mock's menu palette.
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const
    {
        auto colors = QVariantList {};
        for (auto const color: MockTabPalette)
            colors.append(platform::toQColor(color));
        return colors;
    }
    /// Mirrors WindowController::tabBackgroundColor(): the WT-style fill of a coloured native Flat tab.
    Q_INVOKABLE [[nodiscard]] QColor tabBackgroundColor(QColor const& tabColor,
                                                        QColor const& rowBackground,
                                                        bool active,
                                                        bool hovered,
                                                        bool windowActive) const
    {
        return mockTabBackgroundColor(tabColor, rowBackground, active, hovered, windowActive);
    }
    /// Mirrors WindowController::tabTextColor(): the label that reads on a coloured fill.
    Q_INVOKABLE [[nodiscard]] QColor tabTextColor(QColor const& tabBackground) const
    {
        return mockTabTextColor(tabBackground);
    }
    Q_INVOKABLE void activateTab(int) {}
    /// Mirrors WindowController::moveTab(): the active tab goes with the tab, and the model always announces
    /// the active row, as WindowController::onTabMoved() does.
    Q_INVOKABLE void moveTab(int from, int to)
    {
        auto const count = static_cast<int>(_tabs.size());
        if (from < 0 || from >= count || to < 0 || to >= count || from == to)
            return;
        // The rows move, then the focus is announced, then the active row: the controller's order, which
        // the strip's delegates see (onTabAboutToBeMoved(), onTabMoved()).
        beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
        auto const id = _tabs[static_cast<std::size_t>(from)];
        _tabs.erase(_tabs.begin() + from);
        _tabs.insert(_tabs.begin() + to, id);
        endMoveRows();
        if (_chromeArea == window::ChromeArea::Tabs)
            emit chromeFocusChanged();
        emitActiveRoleChanged();
        emit activeTabIndexChanged();
    }
    Q_INVOKABLE [[nodiscard]] quint64 windowIdValue() const noexcept { return 1; }
    Q_INVOKABLE void moveTabIntoThisWindow(quint64, int, int) {}
    Q_INVOKABLE void tearOffTab(int) {}
    Q_INVOKABLE void setTabTitle(int, QString const&) {}
    Q_INVOKABLE void resetTabTitle(int) {}
    Q_INVOKABLE void beginActiveTabTitleEdit() { emit tabTitleEditRequested(0); }
    Q_INVOKABLE void beginActiveTabColorPick() { emit tabColorPickRequested(0); }
    Q_INVOKABLE void beginSaveLayoutPrompt() { emit saveLayoutRequested(); }
    Q_INVOKABLE void saveLayoutAs(QString const& name) { savedLayouts << name; }
    /// Mirrors WindowController::replaceLayout(), recording the name.
    Q_INVOKABLE void replaceLayout(QString const& name) { replacedLayouts << name; }
    /// Mirrors WindowController::checkLayoutName() against @c existingLayouts.
    [[nodiscard]] Q_INVOKABLE QVariantMap checkLayoutName(QString const& name, bool replace) const
    {
        auto const trimmed = name.trimmed();
        return window::WindowController::layoutCheckMap(
            window::checkLayoutName(trimmed.toStdString(),
                                    existingLayouts,
                                    replace ? window::LayoutReplace::Replace : window::LayoutReplace::Keep),
            trimmed);
    }
    /// Mirrors WindowController::layoutSummary() over @c layoutTabs and @c layoutPanes.
    [[nodiscard]] Q_INVOKABLE QString layoutSummary() const
    {
        return QString::fromStdString(window::layoutSummaryText(layoutTabs, layoutPanes));
    }
    int layoutTabs = 1;                       ///< The tabs layoutSummary() counts.
    int layoutPanes = 1;                      ///< Their panes.
    std::vector<std::string> existingLayouts; ///< The layouts checkLayoutName() reads as taken.
    QStringList savedLayouts;                 ///< Every saveLayoutAs(), in order.
    QStringList replacedLayouts;              ///< Every replaceLayout(), in order.
    Q_INVOKABLE void setTabColor(int, QColor const&) {}
    Q_INVOKABLE void resetTabColor(int) {}
    /// Mirrors WindowController::tabWorkingDirectory(): what TabItem's hover tooltip asks for the
    /// hovered tab, which a shown window's tab strip can be. No session runs anywhere, so none.
    Q_INVOKABLE [[nodiscard]] QString tabWorkingDirectory(int /*index*/) const { return {}; }
    /// Mirrors WindowController::closeTabAtIndex() as far as the window sees it: the model announces the
    /// active row after every close, whichever tab went (WindowController::onTabClosed()).
    Q_INVOKABLE void closeTabAtIndex(int index)
    {
        calls << QStringLiteral("closeTabAtIndex:%1").arg(index);
        auto const id = tabIdAt(index);
        if (id < 0)
            return;
        // The focus ends only when its own tab goes, after the row is gone (onTabAboutToBeRemoved(),
        // onTabClosed()).
        auto const endsFocus = _chromeArea == window::ChromeArea::Tabs && _focusedTabId == id;
        beginRemoveRows(QModelIndex(), index, index);
        _tabs.erase(_tabs.begin() + index);
        if (_activeTabId == id)
            _activeTabId =
                _tabs.empty() ? -1 : _tabs[std::min(static_cast<std::size_t>(index), _tabs.size() - 1)];
        endRemoveRows();
        if (endsFocus)
        {
            _chromeArea = window::ChromeArea::Terminal;
            _focusedTabId = -1;
            emit chromeFocusChanged();
        }
        else if (_chromeArea == window::ChromeArea::Tabs)
            emit chromeFocusChanged(); // the focused tab may have moved up a row
        emitActiveRoleChanged();
        emit activeTabIndexChanged();
    }
    Q_INVOKABLE void closeOtherTabs(int) {}
    Q_INVOKABLE void closeTabsToRight(int) {}

    // {{{ QAbstractListModel (feeds the TabStrip delegates)
    [[nodiscard]] int rowCount(QModelIndex const& = QModelIndex()) const override
    {
        return static_cast<int>(_tabs.size());
    }
    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        if (index.row() < 0 || index.row() >= static_cast<int>(_tabs.size()))
            return {};
        auto const badge = decorBadge(index.row());
        switch (static_cast<Roles>(role))
        {
            case Roles::TitleRole: return title(index.row());
            case Roles::RawTitleRole: return QString();
            case Roles::ColorRole: return color(index.row());
            case Roles::IsActiveRole: return index.row() == activeTabIndex();
            case Roles::PaneCountRole: return 1;
            case Roles::SessionIdRole: return tabIdAt(index.row());
            case Roles::ZoomedRole: return false;
            // 0 = ProgressState::Inactive: no operation in flight, so TabItem paints no bar.
            case Roles::ProgressStateRole:
            case Roles::ProgressPercentageRole: return 0;
            // TabBadgeKind::None unless the decor badges the row: nothing then competes for the slot.
            case Roles::BadgeRole: return static_cast<int>(badge.kind);
            case Roles::BadgeTextRole: return QString {};
            case Roles::BadgeProgressRole: return badge.progress;
            case Roles::SecureRole: return isLocked(index.row()) ? 1 : 0;
            default: return {};
        }
    }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        return {
            { static_cast<int>(Roles::DisplayRole), "display" },
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
            { static_cast<int>(Roles::BadgeProgressRole), "badgeProgress" },
            { static_cast<int>(Roles::SecureRole), "tabSecure" },
        };
    }
    // }}}

    QStringList calls;
    QObject* boundWindow = nullptr;
    /// What setChromePresence() was last told; nothing until Main.qml reports.
    bool reportedTabsShown = false;
    int reportedToolbarItems = -1;
    /// The keys chromeKey() was offered, as (Qt::Key, Qt::KeyboardModifiers), oldest first.
    std::vector<std::pair<int, int>> chromeKeys;
    /// What chromeKey() answers.
    bool chromeKeysHandled = true;
    /// Whether chromeKey() acts on Alt+Left, Alt+Right and Escape in the tab well as the controller does.
    bool mirrorsChromeKeys = false;

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void nativeControlsInsetChanged();
    void frameHintsChanged();
    void contextMenuModelChanged();
    void titleBarContextMenuModelChanged();
    void tabBarPositionChanged();
    void tabBarShouldShowChanged();
    void chromeHeightChanged();
    void multimediaReadyChanged();
    void hostingNoticeChanged();
    void chromeFocusChanged();
    /// Closing a busy tab asks first: the tab's id, its label and the running program (empty when unknown).
    void closeTabConfirmationRequested(quint64 tab, QString const& tabLabel, QString const& program);
    /// The tab the question was about is going away.
    void closeTabConfirmationWithdrawn(quint64 tab);
    void tabTitleEditRequested(int /*index*/);
    // Matches TabItem's Connections handler; a missing signal here is a QML warning, not a silent no-op.
    void tabColorPickRequested(int /*index*/);

  private:
    /// The title of row @p row: "Tab n", or a title too long for any tab on row 0 under LongTitle.
    [[nodiscard]] QString title(int row) const
    {
        if (_scene.decor == snapshot::TabDecor::LongTitle && row == 0)
            return { DecorLongTitleLength, QChar(u'w') };
        return QStringLiteral("Tab %1").arg(tabIdAt(row) + 1);
    }

    /// The colour of row @p row: DecorTabColor on row 0 under EveryBadge, else none (transparent).
    [[nodiscard]] QColor color(int row) const
    {
        if (_scene.decor == snapshot::TabDecor::EveryBadge && row == 0)
            return platform::toQColor(DecorTabColor);
        return { Qt::transparent };
    }

    /// The badge of row @p row: rows 1, 2, ... carry DecorBadges under EveryBadge.
    /// @return Whether tab @p row waits for a password under the scene's SecureDecor: the active tab and the
    ///         last one.
    [[nodiscard]] bool isLocked(int row) const noexcept
    {
        switch (_scene.secure)
        {
            case snapshot::SecureDecor::None: break;
            case snapshot::SecureDecor::Locked: return row == _scene.activeTab || row == _scene.tabCount - 1;
        }
        return false;
    }

    [[nodiscard]] DecorBadge decorBadge(int row) const
    {
        auto const slot = static_cast<std::size_t>(row) - 1;
        if (_scene.decor != snapshot::TabDecor::EveryBadge || row < 1 || slot >= DecorBadges.size())
            return {};
        return DecorBadges[slot];
    }

    /// @return The row of the tab @p id, or -1 when it is not in the strip.
    [[nodiscard]] int rowOf(int id) const noexcept
    {
        auto const found = std::ranges::find(_tabs, id);
        return id >= 0 && found != _tabs.end() ? static_cast<int>(found - _tabs.begin()) : -1;
    }

    /// Re-reads the IsActiveRole of every row, as the controller does when the active tab changes.
    void emitActiveRoleChanged()
    {
        if (!_tabs.empty())
            emit dataChanged(index(0),
                             index(static_cast<int>(_tabs.size()) - 1),
                             { static_cast<int>(Roles::IsActiveRole) });
    }

    /// The announcements of a real activation, in WindowController::onActiveTabChanged()'s order: a keyboard
    /// in the tab well goes to the activated tab, then the active row is announced.
    void announceActivation()
    {
        if (_chromeArea == window::ChromeArea::Tabs && _focusedTabId != _activeTabId)
        {
            _focusedTabId = _activeTabId;
            emit chromeFocusChanged();
        }
        emit activeTabIndexChanged();
    }

    MainWindowScene _scene;
    std::vector<int> _tabs; ///< The strip's rows, by tab identity; ids are minted once and never reused.
    int _nextTabId;
    int _activeTabId;
    int _chromeHeight = 0;
    window::ChromeArea _chromeArea = window::ChromeArea::Terminal;
    int _focusedTabId = -1; ///< The focused tab's identity, -1 outside the tab well.
    int _focusedToolbar = -1;
    int _tabBarPosition;                    // seeded from the scene; 0 = Top, 1 = Bottom
    bool _tabBarShouldShow;                 // seeded from the scene's tab-bar visibility
    bool _titleBarVisible;                  // seeded from the scene's frame
    bool _needsExpandedClientArea = false;  // Qt::ExpandedClientAreaHint, which a test sets
    bool _clearsTitleBarBackground = false; // Qt::NoTitleBarBackgroundHint, which a test sets
    window::DaemonDriftModel _drift;
    bool _multimediaReady = false; // the background probe has not finished
    QString _hostingNotice;
};

/// Loads Main.qml offscreen against @p controller and returns the created root object: registers the
/// ContourTerminal stub type, pins the mock to C++ ownership, exposes it as `terminalSessions`, waits
/// for the component to finish loading, and asserts a diagnostic-free load. Every Main.qml load case
/// (startup ordering + the tab-strip layout cases) goes through here so the load protocol lives in one
/// place. Fails the enclosing test (via REQUIRE) if the component does not load cleanly.
/// @param engine     The QML engine (kept alive by the caller for the root's lifetime).
/// @param controller The mock manager + window controller to expose as `terminalSessions`.
/// @param warnings   Captures QML diagnostics emitted during the load.
/// @param chrome     The chrome row, font and window controls to install before the load.
/// @return The created root ApplicationWindow object.
[[nodiscard]] inline std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine,
                                                             MockMainController& controller,
                                                             QmlMessageCapture& warnings,
                                                             ChromeSetup const& chrome)
{
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    // createWindowController() returns `this` through a Q_INVOKABLE, which defaults the returned object
    // to JavaScript ownership — the engine's GC would delete the stack-allocated mock. Pin it, exactly
    // as the production manager pins its controllers.
    QQmlEngine::setObjectOwnership(&controller, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    if (controller.hasDrift())
        engine.rootContext()->setContextProperty("daemonDrift", &controller.daemonDrift());

    installChromeStyle(engine, chrome);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/Main.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    if (!component.isReady())
        for (auto const& error: component.errors())
            UNSCOPED_INFO("Main.qml error: " << error.toString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);
    QCoreApplication::processEvents();
    // What the bindings unloaded while `win` resolved (a Loader deletes its item later) is gone, so a
    // findChild() sees only what the window shows.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(isQmlDiagnostic) == 0);
    return root;
}

/// Loads Main.qml offscreen under the default chrome tokens (see installChromeStyle).
/// @param engine     The QML engine (kept alive by the caller for the root's lifetime).
/// @param controller The mock manager + window controller to expose as `terminalSessions`.
/// @param warnings   Captures QML diagnostics emitted during the load.
/// @return The created root ApplicationWindow object.
[[nodiscard]] inline std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine,
                                                             MockMainController& controller,
                                                             QmlMessageCapture& warnings)
{
    return loadMainWindow(engine, controller, warnings, defaultChromeSetup());
}

} // namespace contour::test
