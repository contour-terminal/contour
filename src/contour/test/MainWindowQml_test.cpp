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
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(QObject* activeTabRootPane READ activeTabRootPane CONSTANT)
    Q_PROPERTY(QObject* activeSession READ activeSession CONSTANT)

  public:
    enum Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1,
        ColorRole,
        IsActiveRole,
        PaneCountRole,
        SessionIdRole,
        RawTitleRole,
    };

    [[nodiscard]] int activeTabIndex() const noexcept { return 0; }
    [[nodiscard]] bool multimediaReady() const noexcept { return false; }
    [[nodiscard]] bool titleBarVisible() const noexcept { return false; } // frameless CSD (default)
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
            default: return {};
        }
    }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override
    {
        return {
            { Qt::DisplayRole, "display" }, { TitleRole, "title" },         { ColorRole, "accentColor" },
            { IsActiveRole, "isActive" },   { PaneCountRole, "paneCount" }, { SessionIdRole, "sessionId" },
            { RawTitleRole, "rawTitle" },
        };
    }
    // }}}

    QStringList calls;
    QObject* boundWindow = nullptr;

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void chromeHeightChanged();
    void tabTitleEditRequested(int index);

  private:
    int _chromeHeight = 0;
};

} // namespace

TEST_CASE("main.qml startup: sized-before-shown ordering and declared chrome (offscreen)",
          "[contour][gui][qml][mainwindow]")
{
    QQmlEngine engine;
    qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
    MockMainController controller;
    // createWindowController() returns `this` through a Q_INVOKABLE, which defaults the returned
    // object to JavaScript ownership — the engine's GC would delete the stack-allocated mock.
    // Pin it, exactly as the production manager pins its controllers.
    QQmlEngine::setObjectOwnership(&controller, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("terminalSessions", &controller);

    contour::test::QmlMessageCapture warnings;

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

    // 1. Startup order: mint the controller, bind THIS window, check for a torn-off tab to adopt
    //    (none here -> returns false), create the first tab (so the window can be sized from real
    //    metrics), and only then map it.
    CHECK(controller.calls
          == QStringList { QStringLiteral("createWindowController"),
                           QStringLiteral("bindWindow"),
                           QStringLiteral("consumePendingTransplant"),
                           QStringLiteral("createNewTab"),
                           QStringLiteral("showInitial") });
    CHECK(controller.boundWindow == root.get());

    // 2. Visible-only-after-size: nothing in the QML sets visible; the (mocked, inert) showInitial
    //    is the only mapper, so the window must still be hidden after a full load.
    CHECK_FALSE(root->property("visible").toBool());

    // 3. Declared chrome: with the custom title bar shown, win.chromeHeight tracks the TitleBar's
    //    effective height (34 logical px, its implicitHeight).
    CHECK(controller.chromeHeight() == 34);

    // 4. The whole load must be free of QML diagnostics (TypeErrors, binding loops, missing
    //    properties) — the run-wide gate in test_main re-checks this over the entire suite.
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

#include <MainWindowQml_test.moc>
