// SPDX-License-Identifier: Apache-2.0
//
// Offscreen end-to-end keyboard-routing tests for the pane tree: typing (and thus every
// keybinding, which rides the same key-event path into the focused display) must keep working
// across tab creation, tab switching, and pane splitting.
//
// Regression pinned here: creating a second tab killed ALL keyboard input. main.qml restores
// terminal focus on every activeTabIndexChanged by force-focusing the loaded PaneNode ROOT; while
// that root was a plain Item (not a FocusScope), focusing it REPLACED the TerminalPane as the
// window scope's focus item — and Qt Quick key events do not bubble, so every subsequent key
// landed on the inert PaneNode and was dropped (typing dead, tab-switch shortcuts dead). With the
// root as a FocusScope, focusing it forwards to the remembered focus child (the active pane).
//
// The harness synthesizes REAL key events (QTest::keyClick -> QWindowSystemInterface), loads the
// REAL main.qml/PaneNode.qml/TerminalPane.qml chain, and records key delivery in a ContourTerminal
// stand-in that mirrors the real display's focus behavior (ItemIsFocusScope) and its silent-drop
// failure mode.

#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
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

#include <QtTest/QTest>
#include <vtmux/PaneLayout.h>

namespace
{

/// Key-recording stand-in for ContourTerminal. A focus scope like the real TerminalDisplay
/// (setFlag(ItemIsFocusScope)); records the text of every key press it receives so the tests can
/// assert END-TO-END delivery — mirroring the real display, where a key that lands anywhere else
/// is silently dropped (keyPressEvent early-outs, and Qt Quick keys do not bubble).
class KeyRecordingTerminal: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(double fontSize READ fontSize CONSTANT)

  public:
    KeyRecordingTerminal()
    {
        setFlag(ItemIsFocusScope);
        setFlag(ItemHasContents);
        setAcceptedMouseButtons(Qt::AllButtons);
    }

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

    void keyPressEvent(QKeyEvent* event) override
    {
        received += event->text();
        event->accept();
    }

    QString received;

  signals:
    void sessionChanged(QObject* session);
    void showNotification(QString const& title, QString const& body);
    void terminated();

  private:
    QObject* _session = nullptr;
};

/// Minimal TerminalSession stand-in exposing every property the TerminalPane/SessionChrome QML
/// binds (all constant — these tests drive focus and keys, not session state).
class RoutingMockSession: public QObject
{
    Q_OBJECT
    Q_PROPERTY(int pageLineCount READ pageLineCount CONSTANT)
    Q_PROPERTY(int pageColumnsCount READ pageColumnsCount CONSTANT)
    Q_PROPERTY(int historyLineCount READ historyLineCount CONSTANT)
    Q_PROPERTY(bool showResizeIndicator READ showResizeIndicator CONSTANT)
    Q_PROPERTY(int upTime READ upTime CONSTANT)
    Q_PROPERTY(float opacity READ opacity CONSTANT)
    Q_PROPERTY(float dimUnfocused READ dimUnfocused CONSTANT)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(QString bellSource READ bellSource CONSTANT)
    Q_PROPERTY(bool isScrollbarRight READ isScrollbarRight CONSTANT)
    Q_PROPERTY(bool isScrollbarVisible READ isScrollbarVisible CONSTANT)
    Q_PROPERTY(bool isImageBackground READ isImageBackground CONSTANT)
    Q_PROPERTY(bool isBlurBackground READ isBlurBackground CONSTANT)
    Q_PROPERTY(float opacityBackground READ opacityBackground CONSTANT)
    Q_PROPERTY(QString pathToBackground READ pathToBackground CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)

  public:
    [[nodiscard]] int pageLineCount() const { return 25; }
    [[nodiscard]] int pageColumnsCount() const { return 80; }
    [[nodiscard]] int historyLineCount() const { return 0; }
    [[nodiscard]] bool showResizeIndicator() const { return false; }
    [[nodiscard]] int upTime() const { return 5; }
    [[nodiscard]] float opacity() const { return 1.0F; }
    [[nodiscard]] float dimUnfocused() const { return 0.0F; }
    [[nodiscard]] QColor backgroundColor() const { return QColor(Qt::black); }
    [[nodiscard]] QString bellSource() const { return QString(); }
    [[nodiscard]] bool isScrollbarRight() const { return true; }
    [[nodiscard]] bool isScrollbarVisible() const { return false; }
    [[nodiscard]] bool isImageBackground() const { return false; }
    [[nodiscard]] bool isBlurBackground() const { return false; }
    [[nodiscard]] float opacityBackground() const { return 1.0F; }
    [[nodiscard]] QString pathToBackground() const { return QString(); }
    [[nodiscard]] QString title() const { return QStringLiteral("mock"); }
};

/// Dynamic PaneProxy stand-in (leaf or split) with settable session/active/children, so a test can
/// drive the exact transitions tab creation and splitting produce.
class RoutingMockPane: public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLeaf READ isLeaf NOTIFY changed)
    Q_PROPERTY(int orientation READ orientation NOTIFY changed)
    Q_PROPERTY(double ratio READ ratio NOTIFY changed)
    Q_PROPERTY(RoutingMockPane* first READ first NOTIFY changed)
    Q_PROPERTY(RoutingMockPane* second READ second NOTIFY changed)
    Q_PROPERTY(QObject* session READ session NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)

  public:
    explicit RoutingMockPane(bool leaf = true): _leaf { leaf } {}

    [[nodiscard]] bool isLeaf() const { return _leaf; }
    [[nodiscard]] int orientation() const { return 2; } // vtmux::SplitState::Vertical (side-by-side)
    [[nodiscard]] double ratio() const { return 0.5; }
    [[nodiscard]] RoutingMockPane* first() const { return _first; }
    [[nodiscard]] RoutingMockPane* second() const { return _second; }
    [[nodiscard]] QObject* session() const { return _session; }
    [[nodiscard]] bool active() const { return _active; }

    Q_INVOKABLE void activate()
    {
        _active = true;
        emit changed();
    }

    void setSession(QObject* s)
    {
        _session = s;
        emit changed();
    }
    void setActive(bool a)
    {
        _active = a;
        emit changed();
    }
    /// Turns this leaf into a split of two child leaves (what splitActivePane produces).
    void becomeSplit(RoutingMockPane* a, RoutingMockPane* b)
    {
        _leaf = false;
        _first = a;
        _second = b;
        if (a != nullptr)
            a->setParent(this);
        if (b != nullptr)
            b->setParent(this);
        emit changed();
    }

  signals:
    void changed();

  private:
    bool _leaf;
    bool _active = false;
    QObject* _session = nullptr;
    RoutingMockPane* _first = nullptr;
    RoutingMockPane* _second = nullptr;
};

/// Manager + WindowController in one mock (the MainWindowQml_test pattern), extended with a
/// SETTABLE activeTabRootPane/activeTabIndex so a test can replay the production event sequence of
/// a tab creation or switch: repoint the root proxy, then announce the active-index change (which
/// is what fires main.qml's restoreTerminalFocus()).
class RoutingMockController: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    Q_PROPERTY(bool multimediaReady READ multimediaReady CONSTANT)
    Q_PROPERTY(bool titleBarVisible READ titleBarVisible NOTIFY titleBarVisibleChanged)
    // Tab-strip placement + visibility main.qml binds; defaults (Top, shown) keep this routing test's
    // layout identical to production without needing to vary them.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(bool tabBarShouldShow READ tabBarShouldShow NOTIFY tabBarShouldShowChanged)
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(QObject* activeTabRootPane READ activeTabRootPane NOTIFY activeTabRootPaneChanged)
    Q_PROPERTY(QObject* activeSession READ activeSession CONSTANT)
    // Mirrors WindowController's command-palette surface: main.qml instantiates CommandPalette.qml
    // against this controller and Connections-targets its commandPaletteRequested signal, so the mock
    // must carry both or the run-wide QML message gate fails the suite.
    Q_PROPERTY(QObject* commandPalette READ commandPalette CONSTANT)
    // Ditto for the context-menu surface: main.qml instantiates ActionContextMenu.qml against this
    // controller, binds its `entries` to contextMenuModel and Connections-targets contextMenuRequested.
    Q_PROPERTY(QVariantList contextMenuModel READ contextMenuModel NOTIFY contextMenuModelChanged)
    // Ditto for the title bar's own menu surface, which main.qml instantiates alongside it.
    Q_PROPERTY(QVariantList titleBarContextMenuModel READ titleBarContextMenuModel NOTIFY
                   titleBarContextMenuModelChanged)

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

    [[nodiscard]] int activeTabIndex() const noexcept { return _activeTabIndex; }
    [[nodiscard]] bool multimediaReady() const noexcept { return false; }
    [[nodiscard]] bool titleBarVisible() const noexcept { return false; }
    [[nodiscard]] int tabBarPosition() const noexcept { return 0; }       // Top
    [[nodiscard]] bool tabBarShouldShow() const noexcept { return true; } // shown
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
    [[nodiscard]] QObject* activeTabRootPane() const noexcept { return _activeTabRootPane; }
    [[nodiscard]] QObject* activeSession() const noexcept { return nullptr; }

    /// Repoints the active tab's root proxy (the pane tree main.qml renders), as the production
    /// controller does on a tab create/switch when it rebuilds its proxies.
    void setActiveTabRootPane(QObject* pane)
    {
        _activeTabRootPane = pane;
        emit activeTabRootPaneChanged();
    }
    /// Announces the active-tab change — the signal main.qml reacts to with restoreTerminalFocus().
    void setActiveTabIndex(int index)
    {
        _activeTabIndex = index;
        emit activeTabIndexChanged();
    }

    [[nodiscard]] QObject* commandPalette() const noexcept { return nullptr; }
    Q_INVOKABLE void runCommand(QString const&) {}
    /// Mirrors WindowController::openCommandPalette(): what the manager calls for the
    /// OpenCommandPalette action, and what makes main.qml's popup appear.
    Q_INVOKABLE void openCommandPalette() { emit commandPaletteRequested(); }

    [[nodiscard]] QVariantList contextMenuModel() const { return {}; }
    [[nodiscard]] QVariantList titleBarContextMenuModel() const { return {}; }
    Q_INVOKABLE void openTitleBarContextMenu() { emit titleBarContextMenuRequested(); }
    Q_INVOKABLE void triggerTitleBarContextMenuAction(int /*actionId*/) {}
    Q_INVOKABLE void triggerContextMenuAction(int) {}
    /// Mirrors WindowController::openContextMenu(): what the manager calls for the OpenContextMenu
    /// action, and what makes main.qml pop the terminal's right-click menu.
    Q_INVOKABLE void openContextMenu() { emit contextMenuRequested(); }

    Q_INVOKABLE QObject* createWindowController() { return this; }
    Q_INVOKABLE void bindWindow(QObject*) {}
    Q_INVOKABLE bool consumePendingTransplant(QObject*) { return false; }
    Q_INVOKABLE bool consumeDefaultLayout(QObject*) { return false; }
    Q_INVOKABLE void createNewTab() {}
    Q_INVOKABLE void showInitial() {}
    Q_INVOKABLE void closeWindow() {}
    Q_INVOKABLE [[nodiscard]] bool canCloseWindow() const noexcept { return true; }
    Q_INVOKABLE void toggleTitleBar() {}
    Q_INVOKABLE void toggleMaximized() {}
    Q_INVOKABLE void minimizeWindow() {}
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const { return {}; }
    Q_INVOKABLE void activateTab(int) {}
    Q_INVOKABLE void moveTab(int, int) {}
    Q_INVOKABLE [[nodiscard]] quint64 windowIdValue() const noexcept { return 1; }
    Q_INVOKABLE void moveTabIntoThisWindow(quint64, int, int) {}
    Q_INVOKABLE void tearOffTab(int) {}
    Q_INVOKABLE void setTabTitle(int, QString const&) {}
    Q_INVOKABLE void resetTabTitle(int) {}
    Q_INVOKABLE void beginActiveTabTitleEdit() { emit tabTitleEditRequested(_activeTabIndex); }
    Q_INVOKABLE void beginActiveTabColorPick() { emit tabColorPickRequested(_activeTabIndex); }
    Q_INVOKABLE void beginSaveLayoutPrompt() { emit saveLayoutRequested(); }
    Q_INVOKABLE void saveLayoutAs(QString const&) {}
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
            case IsActiveRole: return index.row() == _activeTabIndex;
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

  signals:
    void activeTabIndexChanged();
    void titleBarVisibleChanged();
    void commandPaletteRequested();
    void contextMenuModelChanged();
    void contextMenuRequested();
    void titleBarContextMenuModelChanged();
    void titleBarContextMenuRequested();
    void tabBarPositionChanged();
    void tabBarShouldShowChanged();
    void chromeHeightChanged();
    void activeTabRootPaneChanged();
    void tabTitleEditRequested(int index);
    // Matches TabItem's Connections handler; a missing signal here is a QML warning, not a silent no-op.
    void tabColorPickRequested(int index);
    // Matches main.qml's Connections handler for the save-layout prompt; a missing signal here is a QML
    // warning the run-wide gate turns into a suite failure.
    void saveLayoutRequested();

  private:
    int _chromeHeight = 0;
    int _activeTabIndex = 0;
    QObject* _activeTabRootPane = nullptr;
};

/// Loads main.qml against @p controller, shows the (offscreen) window and pumps events.
[[nodiscard]] std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine, RoutingMockController& controller)
{
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/main.qml")));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    if (!component.isReady())
    {
        for (auto const& error: component.errors())
            UNSCOPED_INFO("main.qml error: " << error.toString().toStdString());
        return nullptr;
    }
    std::unique_ptr<QObject> root(component.create());
    QCoreApplication::processEvents();
    return root;
}

/// Collects every KeyRecordingTerminal in the VISUAL item tree under @p item. (QObject-based
/// findChildren() misses QML-created items, whose QObject parent is often null — the engine owns
/// them; only the childItems() chain reflects the rendered tree.)
void collectPanes(QQuickItem* item, QList<KeyRecordingTerminal*>& out)
{
    if (auto* pane = qobject_cast<KeyRecordingTerminal*>(item))
        out.append(pane);
    for (auto* child: item->childItems())
        collectPanes(child, out);
}

[[nodiscard]] QList<KeyRecordingTerminal*> allPanes(QQuickWindow& window)
{
    QList<KeyRecordingTerminal*> out;
    collectPanes(window.contentItem(), out);
    return out;
}

/// The single KeyRecordingTerminal bound to @p session, or nullptr.
[[nodiscard]] KeyRecordingTerminal* paneForSession(QQuickWindow& window, QObject const* session)
{
    for (auto* pane: allPanes(window))
        if (pane->session() == session)
            return pane;
    return nullptr;
}

/// Sends one real key event into @p window and returns the focused terminal's recorded text delta.
void typeKey(QQuickWindow& window, Qt::Key key)
{
    QTest::keyClick(&window, key);
    QCoreApplication::processEvents();
}

} // namespace

TEST_CASE("keyboard routing survives tab creation, tab switching, and splitting (offscreen)",
          "[contour][gui][qml][keyboard][behavior]")
{
    QQmlEngine engine;
    qmlRegisterType<KeyRecordingTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");

    RoutingMockController controller;
    QQmlEngine::setObjectOwnership(&controller, QQmlEngine::CppOwnership);

    // Tab 1: a single active leaf, present before main.qml loads (the post-startup state).
    RoutingMockSession sessionA;
    RoutingMockPane rootA(/*leaf*/ true);
    rootA.setSession(&sessionA);
    rootA.setActive(true);
    controller.setActiveTabRootPane(&rootA);

    contour::test::QmlMessageCapture warnings;

    auto root = loadMainWindow(engine, controller);
    REQUIRE(root != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    REQUIRE(window != nullptr);
    window->show();
    QCoreApplication::processEvents();

    auto* paneA = paneForSession(*window, &sessionA);
    REQUIRE(paneA != nullptr);

    // Startup: the from-birth-active pane grabbed keyboard focus (TerminalPane.Component.onCompleted),
    // so a typed key reaches it.
    typeKey(*window, Qt::Key_A);
    {
        INFO("active focus item after startup: "
             << (window->activeFocusItem() != nullptr ? window->activeFocusItem()->metaObject()->className()
                                                      : "null"));
        CHECK(paneA->received == QStringLiteral("a"));
    }

    SECTION("creating a second tab keeps keys flowing into the new tab's pane")
    {
        // Tab creation, production event order (WindowController::onActiveTabChanged): announce the
        // active-index change FIRST — which fires main.qml's restoreTerminalFocus() — then rebuild
        // the proxies, repointing the active tab's root proxy (the persistent pane rebinds its
        // session). The regression: restore focused the PaneNode ROOT, which as a plain Item
        // swallowed every subsequent key (typing dead, and with it every tab-switch keybinding,
        // which needs the key to reach a display first).
        RoutingMockSession sessionB;
        RoutingMockPane rootB(/*leaf*/ true);
        rootB.setSession(&sessionB);
        rootB.setActive(true);
        controller.setActiveTabIndex(1);
        controller.setActiveTabRootPane(&rootB);
        QCoreApplication::processEvents();

        auto* paneB = paneForSession(*window, &sessionB);
        REQUIRE(paneB != nullptr);

        typeKey(*window, Qt::Key_B);
        {
            INFO("active focus item after tab create: "
                 << (window->activeFocusItem() != nullptr
                         ? window->activeFocusItem()->metaObject()->className()
                         : "null"));
            CHECK(paneB->received.endsWith(QStringLiteral("b")));
        }

        // And switching back to tab 1 keeps keys flowing as well.
        controller.setActiveTabIndex(0);
        controller.setActiveTabRootPane(&rootA);
        QCoreApplication::processEvents();

        auto* paneA2 = paneForSession(*window, &sessionA);
        REQUIRE(paneA2 != nullptr);
        typeKey(*window, Qt::Key_C);
        {
            INFO("active focus item after tab switch: "
                 << (window->activeFocusItem() != nullptr
                         ? window->activeFocusItem()->metaObject()->className()
                         : "null"));
            CHECK(paneA2->received.endsWith(QStringLiteral("c")));
        }
    }

    SECTION("splitting routes keys to the newly active pane")
    {
        // A split makes the NEW pane active from birth; its TerminalPane must grab keyboard focus on
        // creation so typing continues into it without a mouse click.
        RoutingMockSession sessionC;
        auto* leftLeaf = new RoutingMockPane(/*leaf*/ true);
        leftLeaf->setSession(&sessionA);
        auto* rightLeaf = new RoutingMockPane(/*leaf*/ true);
        rightLeaf->setSession(&sessionC);
        rightLeaf->setActive(true);
        rootA.becomeSplit(leftLeaf, rightLeaf);
        QCoreApplication::processEvents();

        auto* paneC = paneForSession(*window, &sessionC);
        REQUIRE(paneC != nullptr);

        typeKey(*window, Qt::Key_D);
        {
            INFO("active focus item after split: "
                 << (window->activeFocusItem() != nullptr
                         ? window->activeFocusItem()->metaObject()->className()
                         : "null"));
            CHECK(paneC->received.endsWith(QStringLiteral("d")));
        }

        // Focus-follows-active across the split: activating the OTHER pane (the model's
        // activePaneChanged flips the proxies' active flags) moves key delivery there.
        auto* paneLeft = paneForSession(*window, &sessionA);
        REQUIRE(paneLeft != nullptr);
        rightLeaf->setActive(false);
        leftLeaf->setActive(true);
        QCoreApplication::processEvents();

        typeKey(*window, Qt::Key_E);
        {
            INFO("active focus item after pane focus change: "
                 << (window->activeFocusItem() != nullptr
                         ? window->activeFocusItem()->metaObject()->className()
                         : "null"));
            CHECK(paneLeft->received.endsWith(QStringLiteral("e")));
        }
    }

    // The whole scenario must be free of QML diagnostics (the run-wide gate re-checks this too).
    for (auto const& w: warnings.messages())
        INFO("QML warning: " << w.toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

#include <KeyboardRouting_test.moc>
