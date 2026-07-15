// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end smoke tests for the GUI frontend's QML components.
//
// These run under the Qt "offscreen" platform plugin (no display server needed, so they work in
// CI). They instantiate each tab/title-bar QML component against a lightweight mock controller that
// mirrors the TerminalSessionManager interface the components rely on, and assert the component
// loads with no QML errors. This catches QML syntax/binding regressions deterministically without
// having to boot a full terminal session.

#include <contour/ColorConversion.h>
#include <contour/CommandCatalog.h>
#include <contour/CommandHistory.h>
#include <contour/CommandPaletteModel.h>
#include <contour/Config.h>
#include <contour/ContextMenu.h>
#include <contour/ContextMenuModel.h>
#include <contour/Shortcut.h>
#include <contour/TabColorScheme.h>
#include <contour/test/QmlMessageCapture.h>

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
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>

#include <QtTest/QTest>
#include <vtmux/PaneLayout.h>

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
    // axis main.qml binds its flags / custom controls / resize border to.
    Q_PROPERTY(
        bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible NOTIFY titleBarVisibleChanged)
    // Mirrors TerminalSessionManager::splitHandleThickness; PaneNode.qml's explicit SplitView handle
    // binds its implicit size to it.
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)

  public:
    [[nodiscard]] bool multimediaReady() const noexcept { return false; }

    [[nodiscard]] constexpr int splitHandleThickness() const noexcept
    {
        return vtmux::DefaultSplitHandleThickness;
    }

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

    /// Mirrors WindowController::triggerContextMenuAction(): what TerminalContextMenu.qml calls on a pick.
    Q_INVOKABLE void triggerContextMenuAction(int actionId) { lastTriggeredActionId = actionId; }

    int lastTriggeredActionId = -1;

    enum Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1,
        ColorRole,
        IsActiveRole,
        PaneCountRole,
        SessionIdRole,
        RawTitleRole, //!< Must mirror TerminalSessionManager; TabStrip's delegate requires it.
        ZoomedRole,   //!< Ditto: TabStrip's delegate requires it (drives TabItem's zoom badge).
    };

    [[nodiscard]] int activeTabIndex() const noexcept { return _activeTabIndex; }

    [[nodiscard]] int rowCount(QModelIndex const& = QModelIndex()) const override { return _count; }

    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override
    {
        if (index.row() < 0 || index.row() >= _count)
            return {};
        switch (role)
        {
            case TitleRole: return QStringLiteral("tab %1").arg(index.row());
            case ColorRole: return QColor(Qt::transparent);
            case IsActiveRole: return index.row() == _activeTabIndex;
            case PaneCountRole: return 1;
            case RawTitleRole: return QString {}; // never-renamed tab: empty raw template (editor blank)
            case ZoomedRole: return false;
            default: return index.row();
        }
    }

    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        // Must match TerminalSessionManager::roleNames() so the delegates bind the same roles —
        // including rawTitle, which TabStrip's delegate declares as a required property.
        return { { Qt::DisplayRole, "display" }, { TitleRole, "title" },
                 { ColorRole, "accentColor" },   { IsActiveRole, "isActive" },
                 { PaneCountRole, "paneCount" }, { SessionIdRole, "sessionId" },
                 { RawTitleRole, "rawTitle" },   { ZoomedRole, "zoomed" } };
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
    /// Sixteen distinct colors, matching the SHAPE of the real palette (vtmux SessionModel's
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
        auto const state = [&] {
            if (active)
                return contour::TabVisualState::Active;
            if (hovered)
                return contour::TabVisualState::Hover;
            return windowActive ? contour::TabVisualState::Inactive
                                : contour::TabVisualState::InactiveWindowUnfocused;
        }();
        return contour::toQColor(contour::tabBackgroundColor(
            contour::toRGBColor(tabColor), contour::toRGBColor(rowBackground), state));
    }
    Q_INVOKABLE [[nodiscard]] QColor tabTextColor(QColor const& tabBackground) const
    {
        return contour::toQColor(contour::contrastingTextColor(contour::toRGBColor(tabBackground)));
    }

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void tabTitleEditRequested(int index);
    // TabItem's Connections declares onTabColorPickRequested; without the matching signal HERE, every
    // test that instantiates a TabItem earns a "no signal of the target matches" QML warning — which the
    // suite's message capture turns into a failure.
    void tabColorPickRequested(int index);

  private:
    int _count = 3;
    int _activeTabIndex = 0;
    bool _titleBarVisible = true;
    int _openSessions = 1;
};

/// A dynamic stand-in for a TerminalSession, exposing the full property + signal surface the QML
/// (TerminalPane
/// + SessionChrome) actually reads and connects to. Unlike a static stub, its properties are settable and
/// carry NOTIFY signals, and it declares every signal the QML `.connect()`s or Connections-targets — so a
/// test can DRIVE the state transitions (resize, opacity change) that real split lifecycle churn produces.
/// This is what makes the connect-leak / null-deref class of split bug reproducible in a headless test.
class MockSession: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int pageLineCount READ pageLineCount NOTIFY lineCountChanged)
    Q_PROPERTY(int pageColumnsCount READ pageColumnsCount NOTIFY columnsCountChanged)
    Q_PROPERTY(int historyLineCount READ historyLineCount NOTIFY historyLineCountChanged)
    Q_PROPERTY(bool showResizeIndicator READ showResizeIndicator CONSTANT)
    Q_PROPERTY(int upTime READ upTime CONSTANT)
    Q_PROPERTY(float opacity READ opacity NOTIFY opacityChanged)
    Q_PROPERTY(float dimUnfocused READ dimUnfocused NOTIFY dimUnfocusedChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(QString bellSource READ bellSource CONSTANT)
    // Mirrors TerminalSession's profile-derived scrollbar properties: both are NOTIFY-driven (a config
    // reload flips them live), so the mock must be able to drive that transition too.
    Q_PROPERTY(bool isScrollbarRight READ isScrollbarRight NOTIFY isScrollbarRightChanged)
    Q_PROPERTY(bool isScrollbarVisible READ isScrollbarVisible NOTIFY isScrollbarVisibleChanged)
    Q_PROPERTY(bool isImageBackground READ isImageBackground CONSTANT)
    Q_PROPERTY(bool isBlurBackground READ isBlurBackground CONSTANT)
    Q_PROPERTY(float opacityBackground READ opacityBackground CONSTANT)
    Q_PROPERTY(QString pathToBackground READ pathToBackground CONSTANT)

  public:
    [[nodiscard]] int pageLineCount() const { return _lines; }
    [[nodiscard]] int pageColumnsCount() const { return _columns; }
    [[nodiscard]] int historyLineCount() const { return _historyLineCount; }
    [[nodiscard]] bool showResizeIndicator() const { return true; }
    [[nodiscard]] int upTime() const { return 5; } // > 1.0 so the resize popup path is exercised
    [[nodiscard]] float opacity() const { return _opacity; }
    [[nodiscard]] float dimUnfocused() const { return _dimUnfocused; }
    [[nodiscard]] bool isImageBackground() const { return false; }
    [[nodiscard]] bool isBlurBackground() const { return false; }
    [[nodiscard]] float opacityBackground() const { return 1.0F; }
    [[nodiscard]] QString pathToBackground() const { return QString(); }
    [[nodiscard]] QColor backgroundColor() const { return QColor(Qt::black); }
    [[nodiscard]] QString bellSource() const { return QString(); }
    [[nodiscard]] bool isScrollbarRight() const { return _scrollbarRight; }
    [[nodiscard]] bool isScrollbarVisible() const { return _scrollbarVisible; }

    // Drivers: mutate state and fire the NOTIFY the QML binds to, reproducing a live resize / opacity change.
    void setPageSize(int columns, int lines)
    {
        _columns = columns;
        _lines = lines;
        emit columnsCountChanged();
        emit lineCountChanged();
    }
    /// Give the session scrollback, so SessionChrome's `hasScrollback` (thumb size < 1.0) turns true and
    /// the scrollbar is actually shown.
    void setHistoryLineCount(int lines)
    {
        _historyLineCount = lines;
        emit historyLineCountChanged();
    }
    /// Move the bar to the other edge, as a profile reload / switch does (TerminalSession::activateProfile
    /// emits isScrollbarRightChanged for exactly this).
    void setScrollbarRight(bool right)
    {
        _scrollbarRight = right;
        emit isScrollbarRightChanged();
    }
    /// Show or hide the bar, as scrollbar.position: Hidden — or hide_in_alt_screen on the alt screen — does.
    void setScrollbarVisible(bool visible)
    {
        _scrollbarVisible = visible;
        emit isScrollbarVisibleChanged();
    }
    void setOpacity(float o)
    {
        _opacity = o;
        emit opacityChanged();
    }
    void setDimUnfocused(float dim)
    {
        _dimUnfocused = dim;
        emit dimUnfocusedChanged();
    }

    // Permission dialog stubs (SessionChrome connects these signals to dialog .open slots).
    Q_INVOKABLE void applyPendingFontChange(bool, bool) {}
    Q_INVOKABLE void applyPendingPaste(bool, bool) {}
    Q_INVOKABLE void executePendingBufferCapture(bool, bool) {}
    Q_INVOKABLE void executeShowHostWritableStatusLine(bool, bool) {}

  signals:
    void lineCountChanged();
    void columnsCountChanged();
    void historyLineCountChanged();
    void isScrollbarRightChanged();
    void isScrollbarVisibleChanged();
    void opacityChanged();
    void dimUnfocusedChanged();
    void onBell();
    void onAlert();
    void onShowNotification(QString const& title, QString const& body);
    void onScrollOffsetChanged(int value);
    void requestPermissionForFontChange();
    void requestPermissionForBufferCapture();
    void requestPermissionForShowHostWritableStatusLine();
    void requestPermissionForPasteLargeFile();

  private:
    int _columns = 80;
    int _lines = 25;
    int _historyLineCount = 0;
    bool _scrollbarRight = true;
    bool _scrollbarVisible = false;
    float _opacity = 1.0F;
    float _dimUnfocused = 0.0F;
};

/// A DYNAMIC stand-in for a PaneProxy (one node of the split tree). Unlike the old static mock, session /
/// active / isLeaf / first / second are settable and carry NOTIFY signals, so a test can drive the exact
/// transitions a real split/collapse produces: a leaf becoming a split node, a session rebinding
/// null->non-null->null, a pane's active flag flipping. This is what exercises PaneNode's Loader
/// re-activation and TerminalPane's onSessionChanged/paneActive handlers — where the split bugs actually
/// live.
class MockPaneProxy: public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLeaf READ isLeaf NOTIFY changed)
    Q_PROPERTY(int orientation READ orientation NOTIFY changed)
    Q_PROPERTY(double ratio READ ratio NOTIFY changed)
    Q_PROPERTY(MockPaneProxy* first READ first NOTIFY changed)
    Q_PROPERTY(MockPaneProxy* second READ second NOTIFY changed)
    Q_PROPERTY(QObject* session READ session NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)

  public:
    explicit MockPaneProxy(bool leaf, MockPaneProxy* a = nullptr, MockPaneProxy* b = nullptr):
        _leaf { leaf }, _first { a }, _second { b }
    {
        if (a)
            a->setParent(this);
        if (b)
            b->setParent(this);
    }

    [[nodiscard]] bool isLeaf() const { return _leaf; }
    [[nodiscard]] int orientation() const { return _orientation; }
    [[nodiscard]] double ratio() const { return _ratio; }
    [[nodiscard]] MockPaneProxy* first() const { return _first; }
    [[nodiscard]] MockPaneProxy* second() const { return _second; }
    [[nodiscard]] QObject* session() const { return _session; }
    [[nodiscard]] bool active() const { return _active; }

    // setRatio (from the divider drag) is a no-op stub — the QML calls it, no test asserts the value.
    // activate() (from a pane tap) is call-tracked so click->activate routing is observable.
    Q_INVOKABLE void setRatio(double) {}
    Q_INVOKABLE void activate() { ++activateCalls; }
    int activateCalls = 0;

    // Drivers used by the behavioral split tests to reproduce lifecycle transitions.
    void setSession(QObject* s)
    {
        if (_session == s)
            return;
        _session = s;
        emit changed();
    }
    void setActive(bool a)
    {
        if (_active == a)
            return;
        _active = a;
        emit changed();
    }
    /// Turn this leaf into a split node with two leaf children (as vtmux Pane::split does), then notify.
    /// The split axis defaults to Vertical (2, matching vtmux::SplitState); use setOrientation() to change
    /// it.
    void becomeSplit(MockPaneProxy* a, MockPaneProxy* b)
    {
        _leaf = false;
        _first = a;
        _second = b;
        a->setParent(this);
        b->setParent(this);
        emit changed();
    }
    void setOrientation(int o)
    {
        _orientation = o;
        emit changed();
    }
    /// Collapse this split node back to a leaf (as Tab::closePane absorbs the sibling), adopting @p
    /// survivor's session, then notify.
    void collapseToLeaf(QObject* survivorSession)
    {
        _leaf = true;
        _first = nullptr;
        _second = nullptr;
        _session = survivorSession;
        emit changed();
    }

  signals:
    void changed();

  private:
    bool _leaf;
    MockPaneProxy* _first;
    MockPaneProxy* _second;
    QObject* _session = nullptr;
    bool _active = false;
    int _orientation = 2; // vtmux::SplitState::Vertical for a split node; ignored for a leaf
    double _ratio = 0.5;
};

/// A trivial stub so PaneNode.qml's leaf branch (TerminalPane → ContourTerminal) can instantiate in
/// the test engine, which does not link the real display type. Behaves as a plain Item.
///
/// `session` is exposed with a NOTIFY signal (mirroring the real ContourTerminal::sessionChanged) and
/// a `showNotification` signal so TerminalPane.qml's onSessionChanged handler and its
/// `vt.onShowNotification.connect(pane.showNotification)` wiring resolve against the stub. The stub
/// session stays null in these tests, and TerminalPane's onSessionChanged early-returns on a null
/// session, so none of the per-session signal connections are dereferenced.
class StubContourTerminal: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    // Terminal.qml / TerminalPane read `fontSize` and connect a window-level `opacityChanged`; expose both
    // so the pane instantiates against the stub.
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
    void opacityChanged(); //!< relayed opacity signal (single-pane).
    void terminated();     //!< TerminalPane's onTerminated handler binds to this.

  private:
    QObject* _session = nullptr;
};

/// Loads a QML component from the contour resources and returns its error string list (empty when
/// the component is valid). The mock controller and a dummy window are exposed as context
/// properties so components that require them instantiate.
[[nodiscard]] QStringList loadErrors(QQmlEngine& engine, QString const& url)
{
    QQmlComponent component(&engine, QUrl(url));
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

    // Each component is registered in resources.qrc under qrc:/contour/ui/.
    auto const components = std::array {
        QStringLiteral("qrc:/contour/ui/TabItem.qml"),
        QStringLiteral("qrc:/contour/ui/TabContextMenu.qml"),
        QStringLiteral("qrc:/contour/ui/TabColorFlyout.qml"),
        QStringLiteral("qrc:/contour/ui/TabStrip.qml"),
        QStringLiteral("qrc:/contour/ui/WindowControls.qml"),
        QStringLiteral("qrc:/contour/ui/ResizeBorder.qml"),
        QStringLiteral("qrc:/contour/ui/TitleBar.qml"),
        QStringLiteral("qrc:/contour/ui/SessionChrome.qml"),
        QStringLiteral("qrc:/contour/ui/CommandPalette.qml"),
        QStringLiteral("qrc:/contour/ui/SaveLayoutDialog.qml"),
        QStringLiteral("qrc:/contour/ui/TerminalContextMenu.qml"),
        QStringLiteral("qrc:/contour/ui/SettingsNavItem.qml"),
        QStringLiteral("qrc:/contour/ui/SettingRow.qml"),
        QStringLiteral("qrc:/contour/ui/ColorSchemeEditor.qml"),
        QStringLiteral("qrc:/contour/ui/SettingsPage.qml"),
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

TEST_CASE("Tab context menu exposes all its actions (offscreen)", "[contour][gui][qml]")
{
    // Regression guard for the Windows empty-menu bug: since Qt 6.8 a Controls Menu defaults to a
    // native OS popup where one exists, and the native Windows menu could not represent this menu's
    // custom surface, so it came up EMPTY. TabContextMenu forces the in-scene popup (popupType:
    // Popup.Item); this test asserts the five actionable items are present once instantiated, which the
    // empty native popup would have failed. (The color flyout that originally provoked the bug now lives
    // on the TabItem — see the SetTabColor tests — but the in-scene popup is still what gives the menu an
    // opaque, themed surface, so the pin stays.)
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabContextMenu.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("tabIndex", 0);
    std::unique_ptr<QObject> menu(component.createWithInitialProperties(initial));
    REQUIRE(menu != nullptr);

    // Menu.count reflects the number of menu items ("Choose Color…", "Rename…", "Close",
    // "Close Other Tabs", "Close Tabs to the Right" — at least five). An empty menu would be zero.
    auto const count = menu->property("count");
    REQUIRE(count.isValid());
    CHECK(count.toInt() >= 5);
}

TEST_CASE("Terminal context menu builds its rows from the C++ model (offscreen)", "[contour][gui][qml]")
{
    // End-to-end over the real bridge: the pure table (ContextMenu.h) -> the Qt model (ContextMenuModel.h)
    // -> the QML that turns rows into menu entries. Nothing is hand-rolled here except the state, so a row
    // that the table stops producing, or an actionId that stops lining up, fails HERE rather than in a
    // silent no-op at runtime.
    //
    // The menu is never popup()'d: offscreen there is no overlay to open into. That is exactly why
    // TerminalContextMenu.qml populates on Component.onCompleted / model change rather than in an
    // about-to-show hook — the rows are there to be asserted the moment the component is complete.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    auto const state = contour::ContextMenuState {
        .hasSelection = false, // Copy must come out present-but-disabled
        .clipboardHasText = true,
        .hasLastCommand = false, // the three "last command" rows must be absent
        .hasLocalWorkingDirectory = true,
        .hasSplits = false,
        .hyperlinkUnderCursor = "", // the two hyperlink rows must be absent
        .activeProfile = "dark",
        .profileNames = { "dark", "light" },
    };

    auto actions = std::vector<contour::actions::Action> {};
    auto const model = contour::toContextMenuModel(contour::buildContextMenu(state), actions);
    REQUIRE_FALSE(model.isEmpty());

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TerminalContextMenu.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(&controller));
    initial.insert("entries", model);
    std::unique_ptr<QObject> menu(component.createWithInitialProperties(initial));
    REQUIRE(menu != nullptr);

    // Every row became an entry: separators and the "Advanced"/"Change Profile" sub-menus included. A
    // native popup (the Windows trap TabContextMenu documents) would have come up empty.
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
    REQUIRE(QMetaObject::invokeMethod(copyItem, "triggered"));
    CHECK(controller.lastTriggeredActionId == 0);
    REQUIRE_FALSE(actions.empty());
    CHECK(contour::commandId(actions[0]) == "CopySelection");

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

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabStrip.qml")));
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
    contour::test::QmlMessageCapture warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabItem.qml")));
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
/// @return The host Window; the TabItem is reachable as its `tabItem` property.
[[nodiscard]] std::unique_ptr<QObject> makeTabItemHost(QQmlEngine& engine,
                                                       MockTabController& controller,
                                                       TabStripAt stripAt = TabStripAt::Top,
                                                       int windowWidth = 800)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    // `y` mirrors main.qml, which anchors the TitleBar to the bottom edge under `tab_bar_position: Bottom`
    // (`y: parent.height - titleBar.height`). `x` puts the tab at the window's RIGHT edge in that case
    // too, so one host covers both axes a popup can overflow on.
    auto const bottom = stripAt == TabStripAt::Bottom;
    auto const source = QStringLiteral("import QtQuick\n"
                                       "import QtQuick.Window\n"
                                       "import \"qrc:/contour/ui\"\n"
                                       "Window {\n"
                                       "  id: host\n"
                                       "  width: %1; height: 600; visible: true\n"
                                       "  property alias tabItem: item\n"
                                       "  TabItem {\n"
                                       "    id: item\n"
                                       "    x: %2 ? host.width - width : 0\n"
                                       "    y: %2 ? host.height - height : 0\n"
                                       "    controller: terminalSessions\n"
                                       "    window: host\n"
                                       "    tabIndex: 0\n"
                                       "    tabTitle: \"shell\"\n"
                                       "    tabRawTitle: \"\"\n"
                                       "    tabColor: \"transparent\"\n"
                                       "    tabActive: true\n"
                                       "    tabPaneCount: 1\n"
                                       "    tabZoomed: false\n"
                                       "  }\n"
                                       "}\n")
                            .arg(windowWidth)
                            .arg(bottom ? QStringLiteral("true") : QStringLiteral("false"));

    QQmlComponent component(&engine);
    component.setData(source.toUtf8(), QUrl(QStringLiteral("qrc:/contour/ui/TabItemTestHost.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();

    INFO("TabItemTestHost errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    std::unique_ptr<QObject> host(component.create());
    REQUIRE(host != nullptr);
    QCoreApplication::processEvents();
    return host;
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
    // is the reason the flyout was hoisted out of TabContextMenu — there is no menu open here at all.
    contour::test::QmlMessageCapture warnings;
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

TEST_CASE("The tab color flyout opens inside the window, wherever the tab strip sits (offscreen)",
          "[contour][gui][qml]")
{
    // The picker is MODAL: one that opens outside the window is not merely misplaced, it is invisible AND
    // still eats every click — the user presses SetTabColor, sees nothing, and cannot color the tab at all
    // until they guess at Escape. So the contract is not "below the tab", it is "on screen"; below the tab
    // is only how it is honored when there is room there.
    //
    // The tab is hosted at the window's edge here, which is exactly what `tab_bar_position: Bottom` does
    // (main.qml anchors the TitleBar at `parent.height - titleBar.height`) — and what the tests above, all
    // of which host the tab at 0,0 in a big window, structurally cannot see.
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabItem.qml")));
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

TEST_CASE("GUI tab strip survives controller destruction without QML errors (offscreen)",
          "[contour][gui][qml]")
{
    // Regression: closing a window destroys the C++ WindowController (deleteLater in
    // removeWindowController) while the QML tree is still alive. QML then resets every reference to
    // the dead controller to null and re-evaluates dependent bindings once more before teardown —
    // any unguarded `controller.` binding raises a TypeError in the shutdown log (seen live:
    // TabStrip.qml "Cannot read property 'activeTabIndex' of null"). The message capture (chained to
    // the run-wide gate) turns such a TypeError into a test failure.
    contour::test::QmlMessageCapture capture;

    QQmlEngine engine;
    auto controller = std::make_unique<MockTabController>();
    engine.rootContext()->setContextProperty("terminalSessions", controller.get());

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabStrip.qml")));
    REQUIRE(component.isReady());

    QVariantMap initial;
    initial.insert("controller", QVariant::fromValue(controller.get()));
    initial.insert("window", QVariant::fromValue(static_cast<QObject*>(&engine)));
    std::unique_ptr<QObject> strip(component.createWithInitialProperties(initial));
    REQUIRE(strip != nullptr);
    QCoreApplication::processEvents();

    // Push the null through the strip's `controller` property the way the app's teardown does:
    // main.qml's `win` var property nulls with notification when the C++ controller dies, and QML
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

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/SessionChrome.qml")));
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

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/SessionChrome.qml")));
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
                                        "import \"qrc:/contour/ui\"\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property alias chrome: chromeItem\n"
                                        "  SessionChrome { id: chromeItem; session: null }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/contour/ui/ScrollBarWrapper.qml")));
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
    contour::test::QmlMessageCapture warnings;

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
    // SessionChrome.qml and the TitleBar in main.qml.
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
    auto scanned = 0;
    auto it = QDirIterator(QStringLiteral(":/contour/ui"),
                           QStringList { QStringLiteral("*.qml") },
                           QDir::Files,
                           QDirIterator::Subdirectories);
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
                offenders
                    << QStringLiteral("%1:%2: %3").arg(file.fileName()).arg(lineNumber).arg(line.trimmed());
        }
    }

    REQUIRE(scanned > 0); // a zero-file scan would pass vacuously

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
    // QtQuick.Dialogs dependency (a missing such module used to cascade up and break the whole main.qml
    // where it is not installed), yet still expose a hex input field for entering any color. Loading the
    // component without error already proves the absent-module hazard is gone; finding the field proves
    // the entry point is there.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TabColorFlyout.qml")));
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
    contour::test::QmlMessageCapture warnings;

    // A split node (vertical) with two leaf children.
    auto* leftLeaf = new MockPaneProxy(/*leaf*/ true);
    auto* rightLeaf = new MockPaneProxy(/*leaf*/ true);
    MockPaneProxy splitNode(/*leaf*/ false, leftLeaf, rightLeaf);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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

    contour::test::QmlMessageCapture warnings;

    auto* leftLeaf = new MockPaneProxy(/*leaf*/ true);
    auto* rightLeaf = new MockPaneProxy(/*leaf*/ true);
    auto splitNode = std::make_unique<MockPaneProxy>(/*leaf*/ false, leftLeaf, rightLeaf);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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

    auto leaf = std::make_unique<MockPaneProxy>(/*leaf*/ true);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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
// Focus-follows-active is covered deterministically at the vtmux model layer; the "renders active pane
// without error" GUI case is covered by the split behavioral test. The global QML-message gate in
// test_main.cpp additionally fails the run on ANY QML error, so a stray ReferenceError cannot pass silently.)

TEST_CASE("Custom window controls show only when the native frame is hidden (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // show_title_bar (terminalSessions.titleBarVisible, re-homed from the removed vtui) now selects the
    // window decoration on every OS:
    //   true  -> native frame + OS window controls; our custom controls must be HIDDEN (no duplicate);
    //   false -> frameless + our custom controls SHOWN.
    // main.qml wires `useCustomWindowControls: !terminalSessions.titleBarVisible` while the tab strip itself
    // stays visible regardless. This exercises that exact binding.
    QQmlEngine engine;
    MockTabController controller;
    controller.setTitleBarVisible(false); // profile show_title_bar: false -> frameless CSD
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

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
    // ToggleTitleBar switches between native decoration and frameless CSD. main.qml keeps a single
    // source of truth (terminalSessions.titleBarVisible) that drives BOTH the window frame (flags) and
    // whether our custom controls render, so the two decorations can never stack into a double frame.
    QQmlEngine engine;
    MockTabController controller; // starts visible (native frame)
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Window
        Item {
            // Mirror main.qml: frameless exactly when the native frame is off; custom controls then on.
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

    contour::test::QmlMessageCapture warnings;
    MockPaneProxy leaf(/*leaf*/ true);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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

TEST_CASE("TerminalPane.onTerminated closes the window only when it is the last session (offscreen)",
          "[contour][gui][qml][split][close]")
{
    // With the pane tree as the sole renderer, TerminalPane.onTerminated is the ONLY window-close path.
    // Bug "closing one pane of a split killed the whole window": after one pane exits, ONE survivor remains,
    // so canCloseWindow() (false while any session remains) must NOT close the window. Only the last exit
    // does. onTerminated routes through THIS window's controller (main.qml exposes it as the window's `win`
    // property), so load the pane inside a Window that declares `property var win` pointing at the mock —
    // mirroring how main.qml's ApplicationWindow exposes its WindowController.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::QmlMessageCapture warnings;

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import \"qrc:/contour/ui\"\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property var win: terminalSessions\n"
                                        "  property alias pane: paneItem\n"
                                        "  TerminalPane { id: paneItem; anchors.fill: parent }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/contour/ui/TestWrapper.qml")));
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
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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

    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession sessionA;
    MockSession sessionB;
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&sessionA);

    contour::test::QmlMessageCapture warnings;

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
    // focus-follows-active is tested at the vtmux model layer.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    contour::test::QmlMessageCapture warnings;

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

    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession original;
    MockPaneProxy rootNode(/*leaf*/ true);
    rootNode.setSession(&original);

    contour::test::QmlMessageCapture warnings;

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

    contour::test::QmlMessageCapture warnings;

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
// (unlike Qt keyboard focus). These complement the vtmux model tests.
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
        QQuickWindow window;
        window.resize(400, 300);
        window.show();

        auto* left = new MockPaneProxy(/*leaf*/ true);
        auto* right = new MockPaneProxy(/*leaf*/ true);
        MockPaneProxy splitNode(/*leaf*/ false, left, right);
        splitNode.setOrientation(nodeOrientation);

        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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

    // node.orientation 2 (vtmux Vertical, side-by-side) -> Qt.Horizontal(1); anything else -> Qt.Vertical(2).
    run(/*node*/ 2, /*qt*/ 1);
    run(/*node*/ 1, /*qt*/ 2);
}

// (No GUI test asserts the SplitView's laid-out child PIXEL widths for a given ratio: SplitView defers its
// layout under the offscreen platform, so a firstChild.width ≈ ratio*width assertion is flaky. The ratio
// contract is covered where it is deterministic: the divider-drag clamp in "PaneNode.clampRatio bounds a
// divider drag..." above, and the model-layer clamp+emit in vtmux SessionModel_test's setPaneRatio test.)

TEST_CASE("TerminalPane: active-pane focus border width follows node.active (offscreen)",
          "[contour][gui][qml][split][behavior][focus]")
{
    // border.width is a plain rendered int: the RELIABLE focus-follows-active GUI oracle (replacing the
    // unreliable offscreen keyboard-focus check). It is the GUI-observable proxy for "the right pane looks
    // focused".
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    MockSession session;
    MockPaneProxy leaf(/*leaf*/ true);
    leaf.setSession(&session);
    leaf.setActive(false);

    auto paneNode = instantiatePaneNode(engine, window, leaf);
    REQUIRE(paneNode != nullptr);

    auto* rootItem = qobject_cast<QQuickItem*>(paneNode.get());
    REQUIRE(rootItem != nullptr);
    auto* pane = rootItem->findChild<StubContourTerminal*>();
    REQUIRE(pane != nullptr);

    // Find the focus-border Rectangle: the child Rectangle with a non-transparent border color and z==10.
    auto findFocusBorder = [&]() -> QQuickItem* {
        for (auto* r: pane->findChildren<QQuickItem*>())
            if (QString(r->metaObject()->className()).contains("Rectangle") && r->property("z").toInt() == 10)
                return r;
        return nullptr;
    };
    auto* border = findFocusBorder();
    REQUIRE(border != nullptr);

    // Inactive -> border width 0.
    CHECK(border->property("border").value<QObject*>()->property("width").toInt() == 0);

    // Active -> border width 1 (the focus highlight).
    leaf.setActive(true);
    QCoreApplication::processEvents();
    CHECK(border->property("border").value<QObject*>()->property("width").toInt() == 1);
}

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
    QQuickWindow window;
    window.resize(600, 400);
    window.show();

    contour::test::QmlMessageCapture warnings;

    // root = [ leaf | split(leaf, leaf) ]  -> three TerminalPane leaves.
    auto* a = new MockPaneProxy(/*leaf*/ true);
    auto* b = new MockPaneProxy(/*leaf*/ true);
    auto* c = new MockPaneProxy(/*leaf*/ true);
    auto* rightSplit = new MockPaneProxy(/*leaf*/ false, b, c);
    MockPaneProxy root(/*leaf*/ false, a, rightSplit);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/PaneNode.qml")));
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
    contour::test::QmlMessageCapture warnings;

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TerminalPane.qml")));
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
    engine.rootContext()->setContextProperty("mockSession", &session);
    contour::test::QmlMessageCapture warnings;

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import \"qrc:/contour/ui\"\n"
                                        "Window {\n"
                                        "  width: 400; height: 300; visible: true\n"
                                        "  property alias pane: paneItem\n"
                                        "  TerminalPane { id: paneItem; anchors.fill: parent;"
                                        " session: mockSession }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/contour/ui/DimWrapper.qml")));
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
        _history { 5 }, _model { std::make_unique<contour::CommandPaletteModel>(_history) }
    {
        _history.record("TogglePaneZoom");
        _model->setSources({ &_actionCommands });
        _model->setShortcuts(contour::shortcutIndex(contour::config::defaultInputMappings));
        _model->refresh();
    }

    [[nodiscard]] QObject* commandPalette() const noexcept { return _model.get(); }
    [[nodiscard]] contour::CommandPaletteModel& model() noexcept { return *_model; }

    /// Records what the popup asked to run, so a test can assert the pick actually routed.
    ///
    /// OpenCommandPalette is dispatched for real, not just recorded: that is the one command the palette
    /// offers that re-enters the palette (WindowController::runCommand -> executeAction -> the manager ->
    /// WindowController::openCommandPalette -> commandPaletteRequested -> main.qml's `open()`), and the
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
    /// What WindowController raises to ask its window to show the palette; main.qml answers it with open().
    void commandPaletteRequested();

  private:
    contour::CommandHistory _history;
    contour::ActionCommandSource _actionCommands;
    std::unique_ptr<contour::CommandPaletteModel> _model;
};

/// Hosts CommandPalette.qml in a Window that mirrors main.qml's ApplicationWindow — in particular it
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
    engine.rootContext()->setContextProperty("paletteController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import \"qrc:/contour/ui\"\n"
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
                                        // main.qml's wiring, verbatim: the controller asks, the window
                                        // opens the palette. Without it here, the one command that re-opens
                                        // the palette (OpenCommandPalette) would look inert in a test and
                                        // its re-entry could not be tested at all.
                                        "  Connections {\n"
                                        "    target: paletteController\n"
                                        "    function onCommandPaletteRequested() { paletteItem.open(); }\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/contour/ui/PaletteTestHost.qml")));
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
    contour::test::QmlMessageCapture warnings;
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
        CHECK(controller.model().data(index, contour::CommandPaletteModel::TitleRole).toString()
              == QStringLiteral("Split Vertical"));
        CHECK(controller.model().data(index, contour::CommandPaletteModel::ShortcutRole).toString()
              == QStringLiteral("Ctrl+Shift+E"));
        CHECK_FALSE(controller.model()
                        .data(index, contour::CommandPaletteModel::DescriptionRole)
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
                .data(controller.model().index(0, 0), contour::CommandPaletteModel::IdRole)
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture capture;
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
/// the saveLayoutRequested signal main.qml opens the dialog on, and the saveLayoutAs() the dialog calls
/// back with the typed name.
class MockSaveLayoutController: public QObject
{
    Q_OBJECT

  public:
    /// Mirrors WindowController::beginSaveLayoutPrompt(): what the manager calls for a nameless
    /// SaveLayout action, and what makes main.qml's prompt appear.
    Q_INVOKABLE void beginSaveLayoutPrompt() { emit saveLayoutRequested(); }

    /// Records the name the dialog accepted, so a test can assert the accept routed through with it.
    Q_INVOKABLE void saveLayoutAs(QString const& name) { saved << name; }
    QStringList saved;

  signals:
    /// What WindowController raises to ask its window to open the prompt; main.qml answers with open().
    void saveLayoutRequested();
};

/// Hosts SaveLayoutDialog.qml in a Window mirroring main.qml's ApplicationWindow — it declares
/// restoreTerminalFocus() (which the dialog calls on close) and wires saveLayoutRequested -> open(),
/// exactly as main.qml does, so the open path and the focus-restore contract are both exercised.
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
/// request, which is the path main.qml takes).
[[nodiscard]] SaveLayoutHost makeSaveLayoutHost(QQmlEngine& engine, MockSaveLayoutController& controller)
{
    engine.rootContext()->setContextProperty("saveLayoutController", &controller);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "import QtQuick.Window\n"
                                        "import \"qrc:/contour/ui\"\n"
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
                                        // main.qml's wiring, verbatim: the controller asks, the window
                                        // opens the prompt.
                                        "  Connections {\n"
                                        "    target: saveLayoutController\n"
                                        "    function onSaveLayoutRequested() { dialogItem.open(); }\n"
                                        "  }\n"
                                        "}\n"),
                      QUrl(QStringLiteral("qrc:/contour/ui/SaveLayoutTestHost.qml")));
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

/// Opens the dialog the way a nameless SaveLayout action does (controller request -> main.qml open()) and
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture capture;
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
    contour::test::QmlMessageCapture warnings;
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
    contour::test::QmlMessageCapture warnings;
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

#include <QmlComponents_test.moc>
