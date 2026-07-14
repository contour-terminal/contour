// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end load test for main.qml — the startup contract between the root
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

#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include <vtmux/PaneLayout.h>

namespace
{

/// A trivial stand-in registered as Contour.Terminal/ContourTerminal so main.qml's static type
/// chain (PaneNode -> TerminalPane -> ContourTerminal) resolves in the test engine. Never
/// instantiated here: activeTabRootPane stays null, so the pane Loader remains inactive.
class StubContourTerminal: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(double fontSize READ fontSize CONSTANT)
  public:
    [[nodiscard]] QObject* session() const { return _session; }
    void setSession(QObject* s)
    {
        if (_session != s)
        {
            _session = s;
            emit sessionChanged(s);
        }
    }
    [[nodiscard]] double fontSize() const { return 12.0; }
  signals:
    void sessionChanged(QObject* session);
    void showNotification(QString const& title, QString const& body);
    void opacityChanged();
    void terminated();

  private:
    QObject* _session = nullptr;
};

/// Manager + WindowController in one mock: `terminalSessions` context property AND the object
/// main.qml assigns to `win` (createWindowController returns this). The QAbstractListModel rows
/// feed the TitleBar's TabStrip like the production WindowController's model does.
class MockMainController: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    Q_PROPERTY(bool multimediaReady READ multimediaReady CONSTANT)
    Q_PROPERTY(bool titleBarVisible READ titleBarVisible NOTIFY titleBarVisibleChanged)
    // Tab-strip placement + resolved visibility, mirroring the production WindowController properties
    // main.qml binds. Writable here so a test can flip them and re-load main.qml to assert the layout.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition WRITE setTabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(
        bool tabBarShouldShow READ tabBarShouldShow WRITE setTabBarShouldShow NOTIFY tabBarShouldShowChanged)
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(QObject* activeTabRootPane READ activeTabRootPane CONSTANT)
    Q_PROPERTY(QObject* activeSession READ activeSession CONSTANT)
    // Mirrors WindowController's command-palette surface. main.qml instantiates CommandPalette.qml
    // against this controller and Connections-targets its commandPaletteRequested signal, so the mock
    // must carry both — a missing signal is a QML warning, and the run-wide gate turns that into a
    // failure of the whole suite. A null model is what an unopened palette shows anyway.
    Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)
    // Ditto for the context-menu surface: main.qml instantiates TerminalContextMenu.qml against this
    // controller, binds its `entries` to contextMenuModel and Connections-targets contextMenuRequested.
    // An empty model is what a menu that has never been opened holds anyway.
    Q_PROPERTY(QVariantList contextMenuModel READ contextMenuModel NOTIFY contextMenuModelChanged)

  public:
    enum Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1,
        ColorRole,
        IsActiveRole,
        PaneCountRole,
        SessionIdRole,
        RawTitleRole,
        ZoomedRole,
    };

    [[nodiscard]] int activeTabIndex() const noexcept { return 0; }
    [[nodiscard]] bool multimediaReady() const noexcept { return false; }
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
        return vtmux::DefaultSplitHandleThickness;
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
    [[nodiscard]] QObject* activeTabRootPane() const noexcept { return nullptr; }
    [[nodiscard]] QObject* activeSession() const noexcept { return nullptr; }
    [[nodiscard]] QObject* commandPalette() const noexcept { return nullptr; }
    Q_INVOKABLE void runCommand(QString const&) {}
    /// Mirrors WindowController::openCommandPalette(): what the manager calls for the
    /// OpenCommandPalette action, and what makes main.qml's popup appear.
    Q_INVOKABLE void openCommandPalette() { emit commandPaletteRequested(); }

    [[nodiscard]] QVariantList contextMenuModel() const { return {}; }
    Q_INVOKABLE void triggerContextMenuAction(int) {}
    /// Mirrors WindowController::openContextMenu(): what the manager calls for the OpenContextMenu
    /// action, and what makes main.qml pop the terminal's right-click menu.
    Q_INVOKABLE void openContextMenu() { emit contextMenuRequested(); }

    // The startup sequence main.qml's Component.onCompleted drives, recorded for the order check.
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
    Q_INVOKABLE void setTabColor(int, QColor const&) {}
    Q_INVOKABLE void resetTabColor(int) {}
    Q_INVOKABLE void closeTabAtIndex(int) {}
    Q_INVOKABLE void closeOtherTabs(int) {}
    Q_INVOKABLE void closeTabsToRight(int) {}

    // {{{ QAbstractListModel (feeds the TabStrip delegates)
    [[nodiscard]] int rowCount(QModelIndex const& = QModelIndex()) const override { return 2; }
    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        if (index.row() < 0 || index.row() >= 2)
            return {};
        switch (role)
        {
            case TitleRole: return QStringLiteral("Tab %1").arg(index.row() + 1);
            case RawTitleRole: return QString();
            case ColorRole: return QColor(Qt::transparent);
            case IsActiveRole: return index.row() == 0;
            case PaneCountRole: return 1;
            case SessionIdRole: return index.row();
            case ZoomedRole: return false;
            default: return {};
        }
    }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        return {
            { Qt::DisplayRole, "display" }, { TitleRole, "title" },         { ColorRole, "accentColor" },
            { IsActiveRole, "isActive" },   { PaneCountRole, "paneCount" }, { SessionIdRole, "sessionId" },
            { RawTitleRole, "rawTitle" },   { ZoomedRole, "zoomed" },
        };
    }
    // }}}

    QStringList calls;
    QObject* boundWindow = nullptr;

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void commandPaletteRequested();
    void contextMenuModelChanged();
    void contextMenuRequested();
    void tabBarPositionChanged();
    void tabBarShouldShowChanged();
    void chromeHeightChanged();
    void tabTitleEditRequested(int index);

  private:
    int _chromeHeight = 0;
    int _tabBarPosition = 0;       // 0 = Top (default), 1 = Bottom
    bool _tabBarShouldShow = true; // strip shown by default
};

/// Loads main.qml offscreen against @p controller and returns the created root object: registers the
/// ContourTerminal stub type, pins the mock to C++ ownership, exposes it as `terminalSessions`, waits
/// for the component to finish loading, and asserts a diagnostic-free load. Every main.qml load case
/// (startup ordering + the tab-strip layout cases) goes through here so the load protocol lives in one
/// place. Fails the enclosing test (via REQUIRE) if the component does not load cleanly.
/// @param engine     The QML engine (kept alive by the caller for the root's lifetime).
/// @param controller The mock manager + window controller to expose as `terminalSessions`.
/// @param warnings   Captures QML diagnostics emitted during the load.
/// @return The created root ApplicationWindow object.
[[nodiscard]] std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine,
                                                      MockMainController& controller,
                                                      contour::test::QmlMessageCapture& warnings)
{
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    // createWindowController() returns `this` through a Q_INVOKABLE, which defaults the returned object
    // to JavaScript ownership — the engine's GC would delete the stack-allocated mock. Pin it, exactly
    // as the production manager pins its controllers.
    QQmlEngine::setObjectOwnership(&controller, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/main.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    if (!component.isReady())
        for (auto const& error: component.errors())
            UNSCOPED_INFO("main.qml error: " << error.toString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);
    QCoreApplication::processEvents();

    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
    return root;
}

} // namespace

TEST_CASE("main.qml startup: sized-before-shown ordering and declared chrome (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    QQmlEngine engine;
    MockMainController controller;
    contour::test::QmlMessageCapture warnings;
    auto root = loadMainWindow(engine, controller, warnings);

    // 1. Startup order: mint the controller, bind THIS window, check for a torn-off tab to adopt
    //    (none here -> returns false), create the first tab (so the window can be sized from real
    //    metrics), and only then map it.
    CHECK(controller.calls
          == QStringList { QStringLiteral("createWindowController"),
                           QStringLiteral("bindWindow"),
                           QStringLiteral("consumePendingTransplant"),
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

TEST_CASE("main.qml tab strip: visibility gate collapses the chrome when hidden (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    // When win.tabBarShouldShow is false (tab_bar_visibility: Never, or Multiple with a single tab),
    // the TitleBar collapses: its effectiveHeight -> 0 -> the declared chromeHeight -> 0, and the strip
    // item is not visible. When true, the strip occupies its 34px implicitHeight (as the startup test
    // already pins). Driving the mock property proves the real main.qml binding reacts.
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

TEST_CASE("main.qml tab strip: position flips the title-bar / content stacking (offscreen)",
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

#include <MainWindowQml_test.moc>
