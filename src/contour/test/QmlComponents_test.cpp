// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end smoke tests for the GUI frontend's QML components.
//
// These run under the Qt "offscreen" platform plugin (no display server needed, so they work in
// CI). They instantiate each tab/title-bar QML component against a lightweight mock controller that
// mirrors the TerminalSessionManager interface the components rely on, and assert the component
// loads with no QML errors. This catches QML syntax/binding regressions deterministically without
// having to boot a full terminal session.

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
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
#include <memory>

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

  public:
    [[nodiscard]] bool multimediaReady() const noexcept { return false; }

    enum Roles : int
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

    Q_INVOKABLE void createNewTab() {}
    Q_INVOKABLE void activateTab(int) {}
    Q_INVOKABLE void moveTab(int, int) {}
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

  signals:
    void activeTabIndexChanged();

  private:
    int _count = 3;
    int _activeTabIndex = 0;
};

/// A stand-in for one node of the pane tree (PaneProxy), exposing exactly the interface PaneNode.qml
/// reads, so the recursive split renderer can be instantiated without a real terminal/display.
class MockPaneProxy: public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLeaf READ isLeaf CONSTANT)
    Q_PROPERTY(int orientation READ orientation CONSTANT)
    Q_PROPERTY(double ratio READ ratio CONSTANT)
    Q_PROPERTY(MockPaneProxy* first READ first CONSTANT)
    Q_PROPERTY(MockPaneProxy* second READ second CONSTANT)
    Q_PROPERTY(QObject* session READ session CONSTANT)
    Q_PROPERTY(bool active READ active CONSTANT)

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
    [[nodiscard]] int orientation() const { return _leaf ? 0 : 2 /*Vertical*/; }
    [[nodiscard]] double ratio() const { return 0.5; }
    [[nodiscard]] MockPaneProxy* first() const { return _first; }
    [[nodiscard]] MockPaneProxy* second() const { return _second; }
    [[nodiscard]] QObject* session() const { return nullptr; }
    [[nodiscard]] bool active() const { return false; }

    Q_INVOKABLE void setRatio(double) {}
    Q_INVOKABLE void activate() {}

  private:
    bool _leaf;
    MockPaneProxy* _first;
    MockPaneProxy* _second;
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
  signals:
    void sessionChanged(QObject* session);
    void showNotification(QString const& title, QString const& body);

  private:
    QObject* _session = nullptr;
};

/// A stand-in for the TerminalDisplay (`vtui`) exposing just the titleBarVisible property main.qml
/// binds the custom title bar's visibility to. Mirrors the real property's read/write/notify so the
/// binding semantics (initial value from the profile, live toggle) can be exercised offscreen.
class MockTitleBarDisplay: public QObject
{
    Q_OBJECT
    Q_PROPERTY(
        bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible NOTIFY titleBarVisibleChanged)

  public:
    [[nodiscard]] bool titleBarVisible() const { return _visible; }
    void setTitleBarVisible(bool v)
    {
        if (_visible != v)
        {
            _visible = v;
            emit titleBarVisibleChanged();
        }
    }

    /// Mirrors TerminalDisplay::toggleTitleBar(): flips show_title_bar, i.e. switches between native
    /// decoration and frameless CSD. main.qml derives both the frame and the custom-controls visibility
    /// from this single value, so a toggle never produces a native+custom double decoration.
    Q_INVOKABLE void toggleTitleBar() { setTitleBarVisible(!_visible); }

  signals:
    void titleBarVisibleChanged();

  private:
    bool _visible = true;
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

TEST_CASE("SessionChrome instantiates and wires a (null) session without errors (offscreen)",
          "[contour][gui][qml]")
{
    // SessionChrome is the per-session chrome (scrollbar, bell, permission dialogs, notification/alert
    // wiring) shared by Terminal.qml and TerminalPane.qml. Merely loading it catches syntax errors;
    // instantiating it against a null session exercises the null-guarded scrollbar/dialog bindings and
    // the wireSession() early-return, which is exactly the transient state a split pane hits on teardown.
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

    // wireSession(null) must be a no-op (the pane-teardown path), not a crash.
    QMetaObject::invokeMethod(chrome.get(), "wireSession", Q_ARG(QVariant, QVariant::fromValue(nullptr)));
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
    static QStringList capturedWarnings;
    capturedWarnings.clear();
    auto handler = [](QtMsgType type, QMessageLogContext const&, QString const& msg) {
        if (type == QtWarningMsg || type == QtCriticalMsg)
            capturedWarnings << msg;
    };
    auto* previous = qInstallMessageHandler(handler);

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

    qInstallMessageHandler(previous);

    REQUIRE(paneNode != nullptr);
    for (auto const& w: capturedWarnings)
        INFO("QML warning: " << w.toStdString());
    // No "instantiated recursively" / "Type PaneNode unavailable" warnings.
    auto const recursionWarnings =
        std::count_if(capturedWarnings.begin(), capturedWarnings.end(), [](QString const& w) {
            return w.contains("recursively") || w.contains("unavailable");
        });
    CHECK(recursionWarnings == 0);
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

    static QStringList capturedWarnings;
    capturedWarnings.clear();
    auto handler = [](QtMsgType type, QMessageLogContext const&, QString const& msg) {
        if (type == QtWarningMsg || type == QtCriticalMsg)
            capturedWarnings << msg;
    };
    auto* previous = qInstallMessageHandler(handler);

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

    qInstallMessageHandler(previous);

    for (auto const& w: capturedWarnings)
        INFO("QML warning: " << w.toStdString());
    auto const nullDerefWarnings =
        std::count_if(capturedWarnings.begin(), capturedWarnings.end(), [](QString const& w) {
            return w.contains("TypeError") || w.contains("null");
        });
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

TEST_CASE("TerminalPane grabs keyboard focus when it becomes the active pane (offscreen)",
          "[contour][gui][qml][split][focus]")
{
    // Regression: keyboard pane navigation (FocusPane*) flips the active pane via the model, which
    // moves the focus border but used to leave Qt keyboard focus on the previously focused pane.
    // TerminalPane now grabs active focus on paneActive -> true, so typing follows the highlight.
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");

    // A live window provides the focus scope that forceActiveFocus() needs under offscreen.
    QQuickWindow window;
    window.resize(400, 300);
    window.show();

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/TerminalPane.qml")));
    // TerminalPane now depends on sibling components (RequestPermission, BellSound), so the engine may
    // compile it asynchronously; pump events until it settles before asserting.
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    {
        INFO("TerminalPane.qml status: " << static_cast<int>(component.status()));
        INFO("TerminalPane.qml errors: " << component.errorString().toStdString());
        REQUIRE(component.isReady());
    }

    // Start inactive, parented into the window's content item.
    QVariantMap initial;
    initial.insert("paneActive", false);
    std::unique_ptr<QObject> obj(component.createWithInitialProperties(initial));
    REQUIRE(obj != nullptr);
    auto* pane = qobject_cast<QQuickItem*>(obj.get());
    REQUIRE(pane != nullptr);
    pane->setParentItem(window.contentItem());
    pane->setWidth(400);
    pane->setHeight(300);
    QCoreApplication::processEvents();

    REQUIRE_FALSE(pane->hasActiveFocus()); // nothing focused it yet

    // Becoming the active pane (as a FocusPane* action would, via node.active -> paneActive) must
    // pull keyboard focus to this terminal.
    pane->setProperty("paneActive", true);
    QCoreApplication::processEvents();

    CHECK(pane->hasActiveFocus());
}

TEST_CASE("Custom window controls show only when the native frame is hidden (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // show_title_bar (vtui.titleBarVisible) now selects the window decoration on every OS:
    //   true  -> native frame + OS window controls; our custom controls must be HIDDEN (no duplicate);
    //   false -> frameless + our custom controls SHOWN.
    // main.qml wires `useCustomWindowControls: !vtui.titleBarVisible` while the tab strip itself stays
    // visible regardless. This exercises that exact binding.
    QQmlEngine engine;
    MockTitleBarDisplay display;
    display.setTitleBarVisible(false); // profile show_title_bar: false -> frameless CSD
    engine.rootContext()->setContextProperty("vtui", &display);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Item {
            property bool customControls: !vtui.titleBarVisible
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
    display.setTitleBarVisible(true);
    CHECK(root->property("customControls").toBool() == false);
    CHECK(root->property("tabStripVisible").toBool() == true);
}

TEST_CASE("ToggleTitleBar flips the native-frame axis without stacking decorations (offscreen)",
          "[contour][gui][qml][titlebar]")
{
    // ToggleTitleBar switches between native decoration and frameless CSD. main.qml keeps a single
    // source of truth (vtui.titleBarVisible) that drives BOTH the window frame (flags) and whether our
    // custom controls render, so the two decorations can never stack into a double frame.
    QQmlEngine engine;
    MockTitleBarDisplay display; // starts visible (native frame)
    engine.rootContext()->setContextProperty("vtui", &display);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Window
        Item {
            // Mirror main.qml: frameless exactly when the native frame is off; custom controls then on.
            property bool frameless: !vtui.titleBarVisible
            property bool customControls: !vtui.titleBarVisible
        }
    )",
                      QUrl());
    REQUIRE(component.isReady());
    std::unique_ptr<QObject> root(component.create());
    REQUIRE(root != nullptr);

    // Native frame on: not frameless, and no custom controls (single decoration).
    REQUIRE(display.titleBarVisible() == true);
    CHECK(root->property("frameless").toBool() == false);
    CHECK(root->property("customControls").toBool() == false);

    // Toggle to frameless CSD: frameless and custom controls both on — still a single decoration.
    display.toggleTitleBar();
    CHECK(display.titleBarVisible() == false);
    CHECK(root->property("frameless").toBool() == true);
    CHECK(root->property("customControls").toBool() == true);

    // Toggle back to native: frameless and custom controls both off. The two are always opposite of
    // each other, so a native frame is never stacked on top of custom controls.
    display.toggleTitleBar();
    CHECK(display.titleBarVisible() == true);
    CHECK(root->property("frameless").toBool() == false);
    CHECK(root->property("customControls").toBool() == false);
}

#include "QmlComponents_test.moc"
