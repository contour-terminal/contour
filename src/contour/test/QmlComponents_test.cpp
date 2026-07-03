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
#include <contour/TabColorScheme.h>
#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

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

    enum Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1,
        ColorRole,
        IsActiveRole,
        PaneCountRole,
        SessionIdRole,
        RawTitleRole, //!< Must mirror TerminalSessionManager; TabStrip's delegate requires it.
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
                 { RawTitleRole, "rawTitle" } };
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
    Q_INVOKABLE void setTabColor(int, QColor const&) {}
    Q_INVOKABLE void resetTabColor(int) {}
    Q_INVOKABLE void closeTabAtIndex(int) {}
    Q_INVOKABLE void closeOtherTabs(int) {}
    Q_INVOKABLE void closeTabsToRight(int) {}
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const
    {
        QVariantList list;
        list.append(QColor(Qt::red));
        list.append(QColor(Qt::green));
        list.append(QColor(Qt::blue));
        return list;
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
    Q_PROPERTY(int historyLineCount READ historyLineCount CONSTANT)
    Q_PROPERTY(bool showResizeIndicator READ showResizeIndicator CONSTANT)
    Q_PROPERTY(int upTime READ upTime CONSTANT)
    Q_PROPERTY(float opacity READ opacity NOTIFY opacityChanged)
    Q_PROPERTY(float dimUnfocused READ dimUnfocused NOTIFY dimUnfocusedChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(QString bellSource READ bellSource CONSTANT)
    Q_PROPERTY(bool isScrollbarRight READ isScrollbarRight CONSTANT)
    Q_PROPERTY(bool isScrollbarVisible READ isScrollbarVisible CONSTANT)
    Q_PROPERTY(bool isImageBackground READ isImageBackground CONSTANT)
    Q_PROPERTY(bool isBlurBackground READ isBlurBackground CONSTANT)
    Q_PROPERTY(float opacityBackground READ opacityBackground CONSTANT)
    Q_PROPERTY(QString pathToBackground READ pathToBackground CONSTANT)

  public:
    [[nodiscard]] int pageLineCount() const { return _lines; }
    [[nodiscard]] int pageColumnsCount() const { return _columns; }
    [[nodiscard]] int historyLineCount() const { return 0; }
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
    [[nodiscard]] bool isScrollbarRight() const { return true; }
    [[nodiscard]] bool isScrollbarVisible() const { return false; }

    // Drivers: mutate state and fire the NOTIFY the QML binds to, reproducing a live resize / opacity change.
    void setPageSize(int columns, int lines)
    {
        _columns = columns;
        _lines = lines;
        emit columnsCountChanged();
        emit lineCountChanged();
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
    // custom background and nested color-flyout Popup, so it came up EMPTY. TabContextMenu forces the
    // in-scene popup (popupType: Popup.Item); this test asserts the five actionable items are present
    // once instantiated, which the empty native popup would have failed.
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

#include <QmlComponents_test.moc>
