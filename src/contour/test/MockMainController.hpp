// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/TabBarMode.hpp>
#include <contour/test/ChromeSnapshotPlan.hpp>
#include <contour/test/MainWindowControllerMock.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

#include <vtworkspace/PaneLayout.hpp>

namespace contour::test
{

/// The shape a MockMainController is built in: what its window holds, fixed for its lifetime.
///
/// The defaults are the mock every Main.qml load case has always seen: two local tabs, a strip that is
/// always shown, and no pane tree, palette model or settings controller.
struct MainWindowScene
{
    int tabCount = 2;                      ///< How many tabs the strip lists.
    QObject* rootPane = nullptr;           ///< The active tab's pane tree; borrowed, may be null.
    QObject* commandPalette = nullptr;     ///< The command palette's model; borrowed, may be null.
    QObject* settingsController = nullptr; ///< The settings page's controller; borrowed, may be null.
    snapshot::ContentPage page = snapshot::ContentPage::Terminal; ///< What the content area shows.
    QString hostLabel; ///< The attached host; empty for a local window.
    /// When the tab strip is shown, resolved against @c tabCount into the initial tabBarShouldShow.
    config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always;
};

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
    Q_PROPERTY(bool titleBarVisible READ titleBarVisible NOTIFY titleBarVisibleChanged)
    // Tab-strip placement + resolved visibility, mirroring the production WindowController properties
    // Main.qml binds. Writable here so a test can flip them and re-load Main.qml to assert the layout.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition WRITE setTabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(
        bool tabBarShouldShow READ tabBarShouldShow WRITE setTabBarShouldShow NOTIFY tabBarShouldShowChanged)
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)
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
    // Mirrors WindowController's hosting notice; writable here so a test can raise it before the load.
    Q_PROPERTY(QString hostingNotice READ hostingNotice WRITE setHostingNotice NOTIFY hostingNoticeChanged)
    // Mirrors WindowController's settings surface: the content area shows the settings page while
    // settingsActive holds, bound to settingsController.
    Q_PROPERTY(bool settingsActive READ settingsActive CONSTANT)
    Q_PROPERTY(QObject* settingsController READ settingsController CONSTANT)

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
        ProgramStatusRole = Qt::UserRole + 10,
        ProgramStatusTextRole = Qt::UserRole + 11,
        ProgramStatusProgressRole = Qt::UserRole + 12,
    };

    /// @param scene What the window holds; see MainWindowScene. Its pointers must outlive the mock.
    explicit MockMainController(MainWindowScene scene = {}):
        _scene { std::move(scene) },
        _tabBarShouldShow { config::isTabBarShown(_scene.tabBarVisibility, _scene.tabCount) }
    {
        attachedHost = _scene.hostLabel;
    }

    [[nodiscard]] int activeTabIndex() const noexcept { return 0; }
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
    [[nodiscard]] bool titleBarVisible() const noexcept { return false; } // frameless CSD (default)

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
    [[nodiscard]] constexpr int splitHandleThickness() const noexcept
    {
        return vtworkspace::DefaultSplitHandleThickness;
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
    /// Mirrors WindowController::settingsActive(): whether the content area shows the settings page.
    [[nodiscard]] bool settingsActive() const noexcept
    {
        return _scene.page == snapshot::ContentPage::Settings;
    }
    [[nodiscard]] QObject* settingsController() const noexcept { return _scene.settingsController; }
    /// Mirrors WindowController::closeSettings(): what the settings page's close button calls. The
    /// page is part of the scene, so it stays.
    Q_INVOKABLE void closeSettings() {}
    Q_INVOKABLE void runCommand(QString const&) {}
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
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const { return {}; }
    Q_INVOKABLE void activateTab(int) {}
    Q_INVOKABLE void moveTab(int, int) {}
    Q_INVOKABLE [[nodiscard]] quint64 windowIdValue() const noexcept { return 1; }
    Q_INVOKABLE void moveTabIntoThisWindow(quint64, int, int) {}
    Q_INVOKABLE void tearOffTab(int) {}
    Q_INVOKABLE void setTabTitle(int, QString const&) {}
    Q_INVOKABLE void resetTabTitle(int) {}
    Q_INVOKABLE void beginActiveTabTitleEdit() { emit tabTitleEditRequested(0); }
    Q_INVOKABLE void beginActiveTabColorPick() { emit tabColorPickRequested(0); }
    Q_INVOKABLE void beginSaveLayoutPrompt() { emit saveLayoutRequested(); }
    Q_INVOKABLE void saveLayoutAs(QString const&) {}
    Q_INVOKABLE void setTabColor(int, QColor const&) {}
    Q_INVOKABLE void resetTabColor(int) {}
    /// Mirrors WindowController::tabWorkingDirectory(): what TabItem's hover tooltip asks for the
    /// hovered tab, which a shown window's tab strip can be. No session runs anywhere, so none.
    Q_INVOKABLE [[nodiscard]] QString tabWorkingDirectory(int /*index*/) const { return {}; }
    Q_INVOKABLE void closeTabAtIndex(int) {}
    Q_INVOKABLE void closeOtherTabs(int) {}
    Q_INVOKABLE void closeTabsToRight(int) {}

    // {{{ QAbstractListModel (feeds the TabStrip delegates)
    [[nodiscard]] int rowCount(QModelIndex const& = QModelIndex()) const override { return _scene.tabCount; }
    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        if (index.row() < 0 || index.row() >= _scene.tabCount)
            return {};
        switch (static_cast<Roles>(role))
        {
            case Roles::TitleRole: return QStringLiteral("Tab %1").arg(index.row() + 1);
            case Roles::RawTitleRole: return QString();
            case Roles::ColorRole: return QColor(Qt::transparent);
            case Roles::IsActiveRole: return index.row() == 0;
            case Roles::PaneCountRole: return 1;
            case Roles::SessionIdRole: return index.row();
            case Roles::ZoomedRole: return false;
            // 0 = ProgressState::Inactive: no operation in flight, so TabItem paints no bar.
            case Roles::ProgressStateRole:
            case Roles::ProgressPercentageRole: return 0;
            // 0 = BadgeKind::None: no program status, so TabItem paints no badge.
            case Roles::ProgramStatusRole: return 0;
            case Roles::ProgramStatusTextRole: return QString {};
            case Roles::ProgramStatusProgressRole: return -1;
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
            { static_cast<int>(Roles::ProgramStatusRole), "programStatus" },
            { static_cast<int>(Roles::ProgramStatusTextRole), "programStatusText" },
            { static_cast<int>(Roles::ProgramStatusProgressRole), "programStatusProgress" },
        };
    }
    // }}}

    QStringList calls;
    QObject* boundWindow = nullptr;

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void contextMenuModelChanged();
    void titleBarContextMenuModelChanged();
    void tabBarPositionChanged();
    void tabBarShouldShowChanged();
    void chromeHeightChanged();
    void multimediaReadyChanged();
    void hostingNoticeChanged();
    void tabTitleEditRequested(int /*index*/);
    // Matches TabItem's Connections handler; a missing signal here is a QML warning, not a silent no-op.
    void tabColorPickRequested(int /*index*/);

  private:
    MainWindowScene _scene;
    int _chromeHeight = 0;
    int _tabBarPosition = 0;       // 0 = Top (default), 1 = Bottom
    bool _tabBarShouldShow;        // seeded from the scene's tab-bar visibility
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
