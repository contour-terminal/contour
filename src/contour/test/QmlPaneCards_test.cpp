// SPDX-License-Identifier: Apache-2.0
//
// QML component tests, continued: the pane tree, the split handle, pane cards and headers, and drops on a
// pane.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/DisplayPixels.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PopupWaits.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlComponentsTestSupport.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SplitChrome.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowController.hpp>

#include <vtpty/ChannelPty.hpp>

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
#include <QtGui/QStyleHints>
#include <QtQml/QJSValue>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlExpression>
#include <QtQml/QQmlProperty>
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

using namespace contour::test::qmlSupport;

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

/// Find the SplitView child of a loaded PaneNode split, or nullptr.
[[nodiscard]] QQuickItem* findSplitView(QQuickItem* root)
{
    for (auto* child: root->findChildren<QQuickItem*>())
        if (QString(child->metaObject()->className()).contains("SplitView"))
            return child;
    return nullptr;
}

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

/// How long the long-path case's working directory is.
constexpr auto LongPathLength = 4096;

/// How many nested splits the deep-tree case shows.
constexpr auto DeepTreeSplits = 8;

/// The pane header roles' indices, as QML names them (chromeStyle.roles).
constexpr auto HeaderTextRole = contour::window::ChromePaneRoleNames::paneHeaderText();

constexpr auto HeaderTextDimmedRole = contour::window::ChromePaneRoleNames::paneHeaderTextDimmed();

/// A sticky header as TerminalSession::stickyHeader() binds it.
[[nodiscard]] QVariantMap stickyEntry(int outcome, int host)
{
    using contour::config::StatusColorRole;
    auto const role = [outcome] {
        switch (static_cast<vtbackend::CommandBlockOutcome>(outcome))
        {
            case vtbackend::CommandBlockOutcome::Running: return StatusColorRole::Running;
            case vtbackend::CommandBlockOutcome::Failure: return StatusColorRole::Danger;
            case vtbackend::CommandBlockOutcome::Success: return StatusColorRole::Success;
        }
        return StatusColorRole::Success;
    }();
    return QVariantMap { { QStringLiteral("glyph"), QStringLiteral("✓") },
                         { QStringLiteral("command"), QStringLiteral("cmake --build") },
                         { QStringLiteral("detail"), QStringLiteral("2m 14s") },
                         { QStringLiteral("outcome"), outcome },
                         { QStringLiteral("statusRole"), static_cast<int>(role) },
                         { QStringLiteral("host"), host },
                         { QStringLiteral("jumpLabel"), QStringLiteral("Jump to Command ↑") } };
}

/// A drag the window is sent by hand: the drag-enter, -move, -drop and -leave events a platform's drag
/// manager would deliver, carrying @ref mime. Exercises the real DropArea of PaneCard.qml, offscreen.
struct DragSource
{
    QQuickWindow& window;
    QMimeData mime;
    /// Whether the last enter or move was accepted by a DropArea.
    bool accepted = false;

    explicit DragSource(QQuickWindow& target): window { target } {}

    static constexpr auto Actions = Qt::CopyAction | Qt::MoveAction;

    /// Qt 6.12 gave the drag events QPointF constructors and deprecated the QPoint ones.
    /// @return @p scene in the type the installed Qt's drag-event constructors take.
    [[nodiscard]] static auto dragPosition(QPointF scene)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        return scene;
#else
        return scene.toPoint();
#endif
    }

    void enter(QPointF scene)
    {
        auto event = QDragEnterEvent { dragPosition(scene), Actions, &mime, Qt::LeftButton, Qt::NoModifier };
        event.ignore();
        QCoreApplication::sendEvent(&window, &event);
        accepted = event.isAccepted();
    }

    void move(QPointF scene)
    {
        auto event = QDragMoveEvent { dragPosition(scene), Actions, &mime, Qt::LeftButton, Qt::NoModifier };
        event.ignore();
        QCoreApplication::sendEvent(&window, &event);
        accepted = event.isAccepted();
    }

    void leave()
    {
        auto event = QDragLeaveEvent {};
        QCoreApplication::sendEvent(&window, &event);
    }

    /// @return What the drop resolved to: IgnoreAction when no DropArea accepted it.
    [[nodiscard]] Qt::DropAction drop(QPointF scene)
    {
        auto event = QDropEvent { scene, Actions, &mime, Qt::LeftButton, Qt::NoModifier };
        event.ignore();
        QCoreApplication::sendEvent(&window, &event);
        return event.isAccepted() ? event.dropAction() : Qt::IgnoreAction;
    }
};

constexpr auto TabMime = "application/x-contour-tab";

constexpr auto TabJson = R"({"windowId":1,"tabIndex":0})";

/// A lone pane card in a window that carries a drop-aware controller.
struct DropHost
{
    PaneTreeHost host;
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    DragSource source;

    DropHost(): host { paneCardSetup(), QSize { 400, 300 } }, source { host.window }
    {
        leaf.setSession(&session);
        host.show(leaf, /*visiblePanes*/ 1);
    }

    [[nodiscard]] MockPaneWindowController& controller() { return host.window.controller(); }
    [[nodiscard]] QQuickItem& card() const { return host.cardOf(leaf); }
    [[nodiscard]] QQuickItem& part(char const* name) const { return PaneTreeHost::partOf(card(), name); }

    /// @return The point of the card at @p fx, @p fy (fractions of its size), in window coordinates.
    [[nodiscard]] QPointF at(qreal fx, qreal fy) const
    {
        return card().mapToScene(QPointF { card().width() * fx, card().height() * fy });
    }

    /// Starts a drag of what @ref source carries over the card at @p fx, @p fy.
    void dragOver(qreal fx, qreal fy)
    {
        source.enter(at(fx, fy));
        source.move(at(fx, fy));
    }
};

/// Whether @p ink is @p mark over @p ground, as an edge anti-aliased across pixels draws it: every channel
/// moved toward the mark by about the same share, and by a good part of the way.
/// @param mark   The colour the edge is drawn in.
/// @param ground What it is drawn over.
/// @param ink    The strongest pixel of the edge.
[[nodiscard]] bool isBlendOf(vtbackend::RGBColor mark, vtbackend::RGBColor ground, vtbackend::RGBColor ink)
{
    constexpr auto MinimumShare = 0.4;
    constexpr auto ShareSpread = 0.1;
    auto shares = std::vector<double> {};
    auto const channels = std::array { std::tuple { mark.red, ground.red, ink.red },
                                       std::tuple { mark.green, ground.green, ink.green },
                                       std::tuple { mark.blue, ground.blue, ink.blue } };
    for (auto const& [markValue, groundValue, inkValue]: channels)
        if (markValue != groundValue)
            shares.push_back(static_cast<double>(inkValue - groundValue)
                             / static_cast<double>(markValue - groundValue));
    return !shares.empty() && *std::ranges::min_element(shares) >= MinimumShare
           && *std::ranges::max_element(shares) - *std::ranges::min_element(shares) <= ShareSpread;
}

/// A pane card over a REAL TerminalDisplay and a live session, in a shown window: what a user drags a tab
/// over. The grid is full of text, so the ring and the ghost are drawn over cells that are not all one
/// colour.
struct DropGridHost
{
    contour::test::TestApp testApp;
    vtpty::ChannelPty* pty = nullptr; ///< Owned by the session's terminal.
    std::unique_ptr<contour::session::TerminalSession> session;
    PaneTreeHost host;
    MockPaneProxy leaf { /*leaf*/ true };
    DragSource source;

    /// How long a frame is given to reach the scene graph before it is grabbed.
    static constexpr int FrameSettleMs = 50;
    /// How long the scene may take to stop changing by itself.
    static constexpr int FrameSettleTimeoutMs = 10000;
    /// The lines of text the page is filled with.
    static constexpr int FilledLines = 25;

    DropGridHost(): host { paneCardSetup(), QSize { 400, 300 } }, source { host.window }
    {
        auto ptyOwned = std::make_unique<vtpty::ChannelPty>(
            vtbackend::PageSize { vtbackend::LineCount(FilledLines), vtbackend::ColumnCount(80) });
        pty = ptyOwned.get();
        session = contour::test::makeTestSession(
            &testApp.app().sessionsManager(), std::move(ptyOwned), testApp.app());
        // PaneTreeHost stands the stub in for the terminal; the card here draws over the real one.
        qmlRegisterType<contour::display::TerminalDisplay>("Contour.Terminal", 1, 0, "ContourTerminal");
        leaf.setSession(session.get());
        host.show(leaf, /*visiblePanes*/ 1);
        source.mime.setData(TabMime, TabJson);
        fillGrid();
    }

    DropGridHost(DropGridHost const&) = delete;
    DropGridHost& operator=(DropGridHost const&) = delete;

    ~DropGridHost()
    {
        session->terminate(); // closes the PTY; the blocked reader wakes and winds down
        QCoreApplication::processEvents();
        host.paneNode.reset(); // the display detaches from the session while it still lives
        QCoreApplication::processEvents();
        session.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    [[nodiscard]] QQuickItem& card() const { return host.cardOf(leaf); }
    [[nodiscard]] QQuickItem& part(char const* name) const { return PaneTreeHost::partOf(card(), name); }

    /// @return The point of the card at @p fx, @p fy (fractions of its size), in window coordinates.
    [[nodiscard]] QPointF at(qreal fx, qreal fy) const
    {
        return card().mapToScene(QPointF { card().width() * fx, card().height() * fy });
    }

    /// Starts a drag of a tab over the card at @p fx, @p fy.
    void dragOver(qreal fx, qreal fy)
    {
        source.enter(at(fx, fy));
        source.move(at(fx, fy));
    }

    /// @return The card's rectangle in the grabbed frame's pixels.
    [[nodiscard]] QRect cardPixels() const
    {
        auto const dpr = host.window.devicePixelRatio();
        auto const rect = card().mapRectToScene(QRectF { QPointF {}, card().size() });
        return QRect { QPointF { rect.topLeft() * dpr }.toPoint(), QSizeF { rect.size() * dpr }.toSize() };
    }

    /// Fills the page with lines of text, and waits until the terminal has parsed them all.
    void fillGrid()
    {
        auto text = std::string {};
        for ([[maybe_unused]] auto const line: std::views::iota(0, FilledLines))
            text += "\033[1m" + std::string(80, 'X') + "\033[m\r\n";
        text += contour::test::DisplayHarness::StatusRequest;
        pty->feed(text);
        REQUIRE(QTest::qWaitFor(
            [&] { return pty->stdinSnapshot().contains(contour::test::DisplayHarness::StatusAnswer); }));
        (void) frame();
    }

    /// @return A frame of the window, grabbed once the scene has stopped changing by itself: what fades
    ///         out on its own (the overlay scrollbar after output) has done so.
    [[nodiscard]] QImage frame()
    {
        auto image = QImage {};
        REQUIRE(QTest::qWaitFor(
            [&] {
                QTest::qWait(FrameSettleMs);
                auto const next = host.window.grabWindow();
                auto const settled = !next.isNull() && next == image;
                image = next;
                return settled;
            },
            FrameSettleTimeoutMs));
        return image;
    }
};

} // namespace

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
                                         std::pair { ChromeStyle::Fluent, ChromeDensity::Comfortable },
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
    // The solver's own rounding, with no tolerance of its own: one would hide a product that lands a hair
    // above a whole pixel at a non-binary ratio, which the solver counts as one pixel more.
    auto const ceilOf = [](qreal value) {
        return contour::window::detail::ceilOf(value);
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
        // The inline header a card draws above its grid (PaneCard.qml), and the card's content inset below
        // it: the grid's origin is `header + contentInset` into the card.
        auto const header = provider->paneHeaderHeight(dpr);
        auto const contentInset = provider->paneCardContentInset(dpr);

        CHECK(isWhole(inset * dpr));
        CHECK(isWhole((inset + first) * dpr));
        CHECK(isWhole((inset + first + gap) * dpr));
        CHECK(isWhole((inset + header + contentInset) * dpr));
        CHECK(isWhole((inset + first + gap + header + contentInset) * dpr));
        CHECK(gap * dpr >= 1); // never floored away

        // What is drawn never exceeds what the solver counts, per axis -- and the solver counts exactly
        // that, rounded up to a whole logical pixel.
        CHECK(gap <= chrome.sideBySideGap);
        CHECK(stackedGap <= chrome.stackedGap);
        CHECK(inset <= chrome.rootInset);
        CHECK(ceilOf(gap) == chrome.sideBySideGap);
        CHECK(ceilOf(stackedGap) == chrome.stackedGap);
        CHECK(ceilOf(inset) == chrome.rootInset);
        CHECK(header <= chrome.leafHeader);
        CHECK(ceilOf(header) == chrome.leafHeader);
    }
}

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

TEST_CASE("pane card: the sticky header takes the pane header's name, and the card says where it lives",
          "[contour][gui][qml][blocks][panecard]")
{
    using contour::config::ChromeStyle;
    using contour::window::StickyHeaderHost;
    auto const row = GENERATE(ChromeStyle::MacOS, ChromeStyle::Terminal);
    CAPTURE(static_cast<int>(row));
    auto setup = paneCardSetup();
    setup.row = row;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    contour::test::QmlMessageCapture const warnings;

    SECTION("in a split it is the pane header's")
    {
        auto panes = TwoPanes {};
        host.show(panes.split, /*visiblePanes*/ 2);
        auto const& card = host.cardOf(panes.left());
        // The card told its session: its header is shown, so the header is the host.
        REQUIRE_FALSE(panes.leftSession.stickyHosts.empty());
        CHECK(panes.leftSession.stickyHosts.back() == static_cast<int>(StickyHeaderHost::PaneHeader));

        auto& sticky = PaneTreeHost::partOf(card, "paneHeaderSticky");
        auto& name = PaneTreeHost::partOf(card, "paneHeaderName");
        CHECK_FALSE(sticky.isVisible());
        CHECK(name.isVisible());

        panes.leftSession.setStickyHeader(
            stickyEntry(static_cast<int>(vtbackend::CommandBlockOutcome::Success),
                        static_cast<int>(StickyHeaderHost::PaneHeader)));
        QCoreApplication::processEvents();
        CHECK(sticky.isVisible());
        CHECK(sticky.property("text").toString() == QStringLiteral("✓ cmake --build · 2m 14s"));
        CHECK_FALSE(name.isVisible());
        CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeaderCwd").isVisible());

        auto& jump = PaneTreeHost::partOf(card, "paneHeaderJump");
        REQUIRE(jump.isVisible());
        CHECK(jump.property("text").toString() == QStringLiteral("Jump to Command ↑"));
        QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier, centreOf(jump));
        QCoreApplication::processEvents();
        CHECK(panes.leftSession.stickyJumps == 1);

        // The other pane has no header to swap.
        CHECK_FALSE(PaneTreeHost::partOf(host.cardOf(panes.right()), "paneHeaderSticky").isVisible());

        // Gone, and the name is back.
        panes.leftSession.setStickyHeader({});
        QCoreApplication::processEvents();
        CHECK(name.isVisible());
        CHECK_FALSE(sticky.isVisible());
        CHECK_FALSE(jump.isVisible());
    }

    SECTION("running is the accent in the glyph, never yellow; failure is danger")
    {
        auto panes = TwoPanes {};
        host.show(panes.split, /*visiblePanes*/ 2);
        auto const& card = host.cardOf(panes.left());
        auto& glyph = PaneTreeHost::partOf(card, "paneHeaderStickyGlyph");
        auto& style = host.style();
        auto const ground = [&] {
            return panes.leftSession.backgroundColor();
        };
        auto const expectInk = [&](vtbackend::CommandBlockOutcome outcome,
                                   contour::config::StatusColorRole role) {
            panes.leftSession.setStickyHeader(
                stickyEntry(static_cast<int>(outcome), static_cast<int>(StickyHeaderHost::PaneHeader)));
            QCoreApplication::processEvents();
            auto const ink = glyph.property("color").value<QColor>();
            // Inked by its role on the pane's ground (the contrast of every such pair is looped in
            // UiStyleProvider_test).
            return std::pair { ink, style.statusMarkOn(static_cast<int>(role), ground()) };
        };
        auto const [running, runningExpected] =
            expectInk(vtbackend::CommandBlockOutcome::Running, contour::config::StatusColorRole::Running);
        auto const [failed, failedExpected] =
            expectInk(vtbackend::CommandBlockOutcome::Failure, contour::config::StatusColorRole::Danger);
        CHECK(running != QColor(Qt::yellow));
        CHECK(running != failed);
        if (row == ChromeStyle::MacOS)
        {
            CHECK(running == runningExpected);
            CHECK(failed == failedExpected);
        }
    }

    SECTION("a lone pane's header is the title band's, which the card announces")
    {
        MockSession session;
        MockPaneProxy leaf { /*leaf*/ true };
        leaf.setSession(&session);
        leaf.setActive(true);
        host.show(leaf, /*visiblePanes*/ 1);
        // No band shown: the header lives over the grid.
        REQUIRE_FALSE(session.stickyHosts.empty());
        CHECK(session.stickyHosts.back() == static_cast<int>(StickyHeaderHost::Grid));

        // The band shows the title (Main.qml's titleBandPresence reaches the card through PaneNode).
        host.paneNode->setProperty("titleBand", static_cast<int>(contour::window::TitleBandPresence::Shown));
        QCoreApplication::processEvents();
        CHECK(session.stickyHosts.back() == static_cast<int>(StickyHeaderHost::TitleBand));
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
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

TEST_CASE("pane card: the Terminal row's find bar takes the header row and no grid cells (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto const& other = host.cardOf(panes.right());
    auto const& terminal = PaneTreeHost::terminalOf(card);
    auto const closed = QRectF { terminal.x(), terminal.y(), terminal.width(), terminal.height() };

    auto& bar = openFindBarOf(card);
    CHECK(parentOf(bar) == &PaneTreeHost::partOf(card, "paneHeaderFindSlot"));
    CHECK(bar.property("height").toReal() == Catch::Approx(host.style().cellHeight()));
    CHECK(QRectF { terminal.x(), terminal.y(), terminal.width(), terminal.height() } == closed);
    CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeaderCwd").isVisible());
    CHECK_FALSE(PaneTreeHost::partOf(card, "paneHeaderActions").isVisible());
    CHECK(PaneTreeHost::partOf(card, "paneHeaderName").isVisible());
    // The other pane's header is its own.
    CHECK(PaneTreeHost::partOf(other, "paneHeaderCwd").isVisible());

    QMetaObject::invokeMethod(&bar, "close");
    QCoreApplication::processEvents();
    CHECK(PaneTreeHost::partOf(card, "paneHeaderCwd").isVisible());
    CHECK(PaneTreeHost::partOf(card, "paneHeaderActions").isVisible());
}

TEST_CASE("pane card: a Terminal-row pane without a header gets the floating bar (offscreen)",
          "[contour][gui][qml][panecard]")
{
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    MockSession session;
    MockPaneProxy leaf { /*leaf*/ true };
    leaf.setSession(&session);
    leaf.setActive(true);
    host.show(leaf, /*visiblePanes*/ 1);

    auto const& card = host.cardOf(leaf);
    auto& bar = openFindBarOf(card);
    CHECK(parentOf(bar) == &PaneTreeHost::terminalOf(card));
    CHECK(bar.property("placement").toMap().value("anchor").toString() == QStringLiteral("topRight"));
    CHECK(bar.property("height").toReal() == Catch::Approx(host.style().cellHeight()));
}

TEST_CASE("pane card: the native find bar sits under the header (offscreen)", "[contour][gui][qml][panecard]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto const& terminal = PaneTreeHost::terminalOf(card);
    auto& bar = openFindBarOf(card);
    CHECK(parentOf(bar) == &terminal);
    // In the card's coordinates: the header, the card's content inset (F5), and the bar's own vertical inset
    // (space.findBarInsetRows, which placeFindBar takes as insetY).
    auto const header = host.style().property("size").value<QObject*>()->property("paneHeader").toReal();
    auto const inset =
        host.style().property("space").value<QObject*>()->property("findBarInsetRows").toReal();
    REQUIRE(inset > 0.0);
    CHECK(terminal.y() + bar.property("y").toReal()
          == Catch::Approx(header + host.style().paneCardContentInset(1.0) + inset));
}

TEST_CASE("pane card: a Minimal header revealed over the grid leaves the find bar where it was (offscreen)",
          "[contour][gui][qml][panecard]")
{
    // F5: the bar asks whether the pane has an INLINE header; Minimal's, revealed while ⌥ is held, lies over
    // the grid, and the bar does not hop in and out with the key.
    auto setup = paneCardSetup();
    setup.row = contour::config::ChromeStyle::Terminal;
    setup.density = contour::config::ChromeDensity::Minimal;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.left());
    auto& bar = openFindBarOf(card);
    auto const before = QPointF { bar.property("x").toReal(), bar.property("y").toReal() };
    auto* const parentBefore = parentOf(bar);
    REQUIRE(parentBefore == &PaneTreeHost::terminalOf(card));

    host.paneNode->setProperty("headerReveal", 1);
    QCoreApplication::processEvents();
    REQUIRE(PaneTreeHost::partOf(card, "paneHeader").isVisible());
    CHECK(parentOf(bar) == parentBefore);
    CHECK(QPointF { bar.property("x").toReal(), bar.property("y").toReal() } == before);
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

namespace
{
/// Makes activate() on either leaf of @p panes move the active pane to it, as the model does: the leaf turns
/// active and pointer-focused, the other inactive and unfocused.
void followActivation(TwoPanes& panes)
{
    auto const activate = [&panes](MockPaneProxy& leaf) {
        for (auto* each: { &panes.left(), &panes.right() })
        {
            each->setActive(each == &leaf);
            each->setFocus(static_cast<int>(each == &leaf ? contour::window::PaneFocus::Focused
                                                          : contour::window::PaneFocus::Unfocused));
        }
    };
    panes.left().onActivate = [&panes, activate] {
        activate(panes.left());
    };
    panes.right().onActivate = [&panes, activate] {
        activate(panes.right());
    };
}
} // namespace

TEST_CASE("pane card: a press on a pane activates it, however long it is held or far it drags (offscreen)",
          "[contour][gui][qml][panecard][focus]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    followActivation(panes);
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& terminal = PaneTreeHost::terminalOf(host.cardOf(panes.right()));
    auto const at = centreOf(terminal);

    SECTION("a click")
    {
        QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier, at);
    }
    SECTION("a press held past the long-press time")
    {
        QTest::mousePress(&host.window, Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() * 2);
        QTest::mouseRelease(&host.window, Qt::LeftButton, Qt::NoModifier, at);
    }
    SECTION("a press that drags a selection")
    {
        auto const step = QPoint { QGuiApplication::styleHints()->startDragDistance(), 0 };
        QTest::mousePress(&host.window, Qt::LeftButton, Qt::NoModifier, at);
        for (auto const i: std::views::iota(1, 5))
            QTest::mouseMove(&host.window, at + step * i);
        QTest::mouseRelease(&host.window, Qt::LeftButton, Qt::NoModifier, at + step * 4);
    }

    CHECK(panes.right().activateCalls == 1);
    CHECK(panes.right().active());
    CHECK(QTest::qWaitFor([&terminal] { return terminal.hasActiveFocus(); }));
}

TEST_CASE("pane card: a click on the active pane takes the keyboard back (offscreen)",
          "[contour][gui][qml][panecard][focus]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    followActivation(panes);
    host.show(panes.split, /*visiblePanes*/ 2);

    auto& left = PaneTreeHost::terminalOf(host.cardOf(panes.left()));
    REQUIRE(QTest::qWaitFor([&left] { return left.hasActiveFocus(); }));

    // The keyboard went somewhere else while the model kept the left pane active: a chrome item that closed
    // and handed it on, or a header button of the pane's own card.
    SECTION("to another pane's grid")
    {
        PaneTreeHost::terminalOf(host.cardOf(panes.right())).forceActiveFocus();
    }
    SECTION("to a header button")
    {
        auto const& header = PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeader");
        QTest::mouseMove(&host.window, centreOf(header));
        auto& button = PaneTreeHost::partOf(host.cardOf(panes.left()), "paneHeaderAction0");
        REQUIRE(QTest::qWaitFor([&button] { return button.isVisible(); }));
        button.forceActiveFocus();
    }
    REQUIRE_FALSE(left.hasActiveFocus());
    REQUIRE(panes.left().active());

    QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier, centreOf(left));
    CHECK(QTest::qWaitFor([&left] { return left.hasActiveFocus(); }));
    CHECK(panes.left().active());
}

TEST_CASE("pane card: clicks move the active pane and the keyboard together, in any order (offscreen)",
          "[contour][gui][qml][panecard][focus]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    followActivation(panes);
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const leaves =
        std::array { &panes.right(), &panes.left(), &panes.left(), &panes.right(), &panes.left() };
    for (auto* leaf: leaves)
    {
        auto const& card = host.cardOf(*leaf);
        auto const& terminal = PaneTreeHost::terminalOf(card);
        // The pointer passes over the card's header on its way, as it does reaching for a pane's actions.
        QTest::mouseMove(&host.window, centreOf(PaneTreeHost::partOf(card, "paneHeader")));
        QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier, centreOf(terminal));
        CHECK(leaf->active());
        CHECK(QTest::qWaitFor([&terminal] { return terminal.hasActiveFocus(); }));
    }
}

TEST_CASE("pane card: the actions show while the pointer is anywhere on the card (offscreen)",
          "[contour][gui][qml][panecard][focus]")
{
    auto host = PaneTreeHost { paneCardSetup(), QSize { 800, 400 } };
    auto panes = TwoPanes {};
    host.show(panes.split, /*visiblePanes*/ 2);

    auto const& card = host.cardOf(panes.right());
    auto const& actions = PaneTreeHost::partOf(card, "paneHeaderActions");
    QTest::mouseMove(&host.window, centreOf(PaneTreeHost::terminalOf(host.cardOf(panes.left()))));
    REQUIRE(QTest::qWaitFor([&actions] { return actions.opacity() == 0.0; }));

    // Over the grid, which takes the hover for its own pointer tracking, as much as over the header.
    QTest::mouseMove(&host.window, centreOf(PaneTreeHost::terminalOf(card)));
    CHECK(QTest::qWaitFor([&actions] { return actions.opacity() == 1.0; }));

    auto const& zoom = PaneTreeHost::partOf(card, "paneHeaderAction2");
    QTest::mouseMove(&host.window, centreOf(zoom));
    QTest::mouseClick(&host.window, Qt::LeftButton, Qt::NoModifier, centreOf(zoom));
    CHECK(panes.right().headerActions == std::vector<int> { 2 });
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

TEST_CASE("pane card: a header button that hides hands the focus back to its terminal (offscreen)",
          "[contour][gui][qml][panecard]")
{
    using contour::config::ChromeDensity;
    using contour::config::ChromeStyle;
    auto const [row, density] = GENERATE(std::pair { ChromeStyle::MacOS, ChromeDensity::Minimal },
                                         std::pair { ChromeStyle::Terminal, ChromeDensity::Comfortable });
    CAPTURE(static_cast<int>(row), static_cast<int>(density));
    auto setup = paneCardSetup();
    setup.row = row;
    setup.density = density;
    auto host = PaneTreeHost { setup, QSize { 800, 400 } };
    host.window.requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(&host.window));
    auto panes = TwoPanes {};
    // Minimal shows the actions while the modifier is held and the header was reached by keyboard (its
    // actions otherwise wait for hover); the Terminal row on the focused pane.
    if (density == ChromeDensity::Minimal)
        panes.left().setFocus(static_cast<int>(contour::window::PaneFocus::KeyboardFocused));
    host.show(panes.split,
              /*visiblePanes*/ 2,
              static_cast<int>(density == ChromeDensity::Minimal ? contour::window::HeaderReveal::HeldModifier
                                                                 : contour::window::HeaderReveal::None));

    // The pointer rests on the other pane, so that no hover keeps the left pane's actions shown. The window
    // delivers the moves (and the hover they settle) asynchronously, so wait until the left card's hover
    // handler has seen it leave.
    auto const& card = host.cardOf(panes.left());
    auto const handlers = card.findChildren<QObject*>(Qt::FindDirectChildrenOnly);
    auto const hover = std::ranges::find_if(
        handlers, [](QObject const* handler) { return handler->inherits("QQuickHoverHandler"); });
    REQUIRE(hover != handlers.end());
    QTest::mouseMove(&host.window, QPoint { host.window.width() - 2, host.window.height() - 2 });
    QTest::mouseMove(&host.window, QPoint { host.window.width() - 1, host.window.height() - 1 });
    REQUIRE(QTest::qWaitFor([&] { return !(*hover)->property("hovered").toBool(); }));

    auto& button = PaneTreeHost::partOf(card, "paneHeaderAction0");
    REQUIRE(QTest::qWaitFor([&] { return button.isVisible(); }));
    button.forceActiveFocus();
    REQUIRE(button.hasActiveFocus());

    // The modifier goes up (the header hides), or the pane loses focus (the actions fade, the header stays).
    if (density == ChromeDensity::Minimal)
        host.paneNode->setProperty("headerReveal", static_cast<int>(contour::window::HeaderReveal::None));
    else
        panes.left().setFocus(static_cast<int>(contour::window::PaneFocus::Unfocused));
    REQUIRE(QTest::qWaitFor([&] { return !button.isVisible(); }));

    // The header settles where the focus went once the hide has run: wait for it, not for a fixed time.
    auto const& terminal = PaneTreeHost::terminalOf(card);
    CHECK(QTest::qWaitFor([&] { return terminal.hasActiveFocus(); }));
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

    SECTION("and an ended pane's footer too")
    {
        panes.leftSession.setEnd(static_cast<int>(contour::session::EndReason::Exited),
                                 QStringLiteral("Process exited with code 1."),
                                 static_cast<int>(contour::session::FooterAction::Restart));
        host.settle();
        auto const& footer = PaneTreeHost::partOf(card, "exitedFooter");
        REQUIRE(footer.isVisible());
        CHECK(PaneTreeHost::partOf(card, "exitedFooterBackground").opacity() == Catch::Approx(0.5));

        // The terminal's background and dim stop square at the footer's top: no rounded corner of theirs
        // reaches into it (a wedge of backdrop beside it) and no strip of theirs lies under it (composited
        // twice with the footer's own).
        auto const footerTop = footer.mapToScene(QPointF {}).y();
        for (auto const* name: { "paneBackground", "dimOverlay" })
        {
            INFO("layer: " << name);
            auto const& layer = PaneTreeHost::partOf(card, name);
            CHECK(layer.mapToScene(QPointF { 0, layer.height() }).y() == footerTop);
            CHECK(layer.clip());
        }
    }
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
        std::array { QStringLiteral("┼"), QStringLiteral("⊟"), QStringLiteral("Z"), QStringLiteral("×") };
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

TEST_CASE(
    "pane card: a tab dragged to an edge shows the ring and the ghost, and drops as a split (offscreen)",
    "[contour][gui][qml][panecard][drop]")
{
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);
    auto& ring = drop.part("dropRing");
    auto& ghost = drop.part("dropGhost");
    CHECK_FALSE(ring.isVisible());
    CHECK_FALSE(ghost.isVisible());

    drop.dragOver(0.95, 0.5);
    CHECK(drop.source.accepted);
    CHECK(QTest::qWaitFor([&] { return ring.isVisible() && ghost.isVisible(); }));
    // The ghost is the half on the right edge's side, inset from the card, and says what it will do.
    CHECK(ghost.x() > drop.card().width() / 2);
    CHECK(ghost.width() < drop.card().width() / 2);
    CHECK(PaneTreeHost::partOf(ghost, "dropGhostLabel").property("text").toString()
          == QStringLiteral("Tab → split right"));
    CHECK(drop.controller().lastTabPayload == QString::fromLatin1(TabJson));

    // Both marks are the colour UiStyleProvider_test holds to 3:1 on every scheme: the engaged scrollbar
    // thumb's role, resolved on the card's own fill.
    auto const mark =
        drop.host.style().colorOn(static_cast<int>(contour::config::ChromeColorRole::ScrollbarThumbActive),
                                  drop.session.backgroundColor());
    CHECK(QQmlProperty(&ring, "border.color").read().value<QColor>() == mark);
    CHECK(QQmlProperty(&ghost, "border.color").read().value<QColor>() == mark);

    CHECK(drop.source.drop(drop.at(0.95, 0.5)) == Qt::MoveAction);
    CHECK(drop.controller().droppedZones
          == std::vector<int> { static_cast<int>(contour::window::DropZone::SplitRight) });
    CHECK(drop.controller().droppedTabPayloads == std::vector<QString> { QString::fromLatin1(TabJson) });
    CHECK(QTest::qWaitFor([&] { return !ring.isVisible() && !ghost.isVisible(); }));
}

TEST_CASE("display: a tab dragged to a pane's edge draws the ring and the ghost over the cells, on that "
          "edge's half only",
          "[display][panecard][drop]")
{
    using namespace contour::test::pixel;
    REQUIRE_DISPLAY_OR_SKIP();
    auto drop = DropGridHost {};
    auto const dpr = drop.host.window.devicePixelRatio();
    auto const cardRect = drop.cardPixels();
    auto const before = drop.frame();

    drop.dragOver(0.95, 0.5);
    REQUIRE(drop.source.accepted);
    auto& ghost = drop.part("dropGhost");
    REQUIRE(QTest::qWaitFor([&] { return ghost.isVisible(); }));
    auto const during = drop.frame();

    auto const mark =
        drop.host.style().colorOn(static_cast<int>(contour::config::ChromeColorRole::ScrollbarThumbActive),
                                  drop.session->getBackgroundColor());
    auto const markRgb = vtbackend::RGBColor { static_cast<uint8_t>(mark.red()),
                                               static_cast<uint8_t>(mark.green()),
                                               static_cast<uint8_t>(mark.blue()) };

    // The ring: the card's own edge, in the mark's colour, and 3:1 against what it covers.
    auto const ringProbe = QRect { cardRect.center().x() - 2, cardRect.top(), 5, 4 };
    auto const ring = regionColors(during, ringProbe);
    auto const beneathRing = regionColors(before, ringProbe);
    INFO("ring ink " << hexOf(ring.ink) << " mark " << hexOf(markRgb) << " beneath "
                     << hexOf(beneathRing.ground));
    CHECK(isBlendOf(markRgb, beneathRing.ground, ring.ink));
    CHECK(vtbackend::contrastRatio(ring.ink, beneathRing.ground) >= MarkContrast);

    // The ghost's edge, over the cells: the half on the right edge's side, in the same colour.
    auto const ghostRect = QRectF { ghost.mapToScene(QPointF {}), ghost.size() };
    auto const ghostLeft = static_cast<int>(ghostRect.left() * dpr);
    auto const ghostMid = static_cast<int>(ghostRect.center().y() * dpr);
    auto const ghostProbe = QRect { ghostLeft - 1, ghostMid - 2, 5, 5 };
    auto const ghostEdge = regionColors(during, ghostProbe);
    auto const beneathGhost = regionColors(before, ghostProbe);
    INFO("ghost ink " << hexOf(ghostEdge.ink) << " beneath " << hexOf(beneathGhost.ground));
    CHECK(isBlendOf(markRgb, beneathGhost.ground, ghostEdge.ink));
    CHECK(vtbackend::contrastRatio(ghostEdge.ink, beneathGhost.ground) >= MarkContrast);

    // Inside the ring, only the right half changed: nothing is drawn over the left half.
    auto const interior = cardRect.adjusted(6, 6, -6, -6);
    auto const changed = changedBounds(before, during, interior);
    REQUIRE_FALSE(changed.isNull());
    CHECK(changed.left() >= cardRect.center().x() - 2);

    // The tab leaves: the frame is what it was before the drag.
    drop.source.leave();
    REQUIRE(QTest::qWaitFor([&] { return !ghost.isVisible(); }));
    auto const after = drop.frame();
    auto const left = changedBounds(before, after, cardRect);
    INFO("left over: " << left.x() << "," << left.y() << " " << left.width() << "x" << left.height());
    CHECK(left.isNull());
}

TEST_CASE("pane card: the ghost follows the edge the tab is nearest, and the middle shows none (offscreen)",
          "[contour][gui][qml][panecard][drop]")
{
    using contour::window::DropZone;
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);
    drop.dragOver(0.5, 0.5);

    // Each edge, with the zone the controller would answer there and the label the ghost shows.
    for (auto const& [fx, fy, zone]: { std::tuple { 0.02, 0.5, DropZone::SplitLeft },
                                       std::tuple { 0.98, 0.5, DropZone::SplitRight },
                                       std::tuple { 0.5, 0.02, DropZone::SplitTop },
                                       std::tuple { 0.5, 0.98, DropZone::SplitBottom } })
    {
        CAPTURE(fx, fy);
        drop.source.move(drop.at(fx, fy));
        auto const& ghost = drop.part("dropGhost");
        CHECK(QTest::qWaitFor([&] { return ghost.isVisible(); }));
        CHECK(PaneTreeHost::partOf(ghost, "dropGhostLabel").property("text").toString()
              == drop.controller().dropGhostLabel(static_cast<int>(zone)));
        drop.source.move(drop.at(0.5, 0.5));
        CHECK(QTest::qWaitFor([&] { return !ghost.isVisible(); }));
    }
    drop.source.leave();
}

TEST_CASE(
    "pane card: a tab dropped in the middle, or where the window cannot split, is not accepted (offscreen)",
    "[contour][gui][qml][panecard][drop]")
{
    // Unaccepted, so the drag ends IgnoreAction and the tab tears off into its own window, as before.
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);

    SECTION("the middle of the card")
    {
        drop.dragOver(0.5, 0.5);
        CHECK_FALSE(drop.part("dropRing").isVisible());
        CHECK_FALSE(drop.part("dropGhost").isVisible());
        CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::IgnoreAction);
    }

    SECTION("an edge of a window that cannot split")
    {
        drop.controller().split = contour::window::SplitSupport::Unsupported;
        drop.dragOver(0.95, 0.5);
        CHECK_FALSE(drop.part("dropRing").isVisible());
        CHECK_FALSE(drop.part("dropGhost").isVisible());
        CHECK(drop.source.drop(drop.at(0.95, 0.5)) == Qt::IgnoreAction);
    }

    CHECK(drop.controller().droppedZones.empty());
}

TEST_CASE("pane card: files and text drop into the pane's session, whatever part of the card (offscreen)",
          "[contour][gui][qml][panecard][drop]")
{
    auto drop = DropHost {};

    SECTION("files, as their URLs")
    {
        auto const urls = QList<QUrl> { QUrl { QStringLiteral("file:///tmp/a%20b.txt") },
                                        QUrl { QStringLiteral("file:///tmp/c.txt") } };
        drop.source.mime.setUrls(urls);
        drop.dragOver(0.5, 0.5);
        CHECK(QTest::qWaitFor([&] { return drop.part("dropRing").isVisible(); }));
        CHECK_FALSE(drop.part("dropGhost").isVisible()); // only a split has a ghost
        CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::CopyAction);
        REQUIRE(drop.session.droppedUrls.size() == 1);
        CHECK(drop.session.droppedUrls.front() == urls);
        CHECK(drop.session.droppedTexts.empty());
        CHECK(drop.controller().lastPayload == static_cast<int>(contour::window::DropPayload::Files));
    }

    SECTION("text")
    {
        drop.source.mime.setText(QStringLiteral("echo hi"));
        drop.dragOver(0.95, 0.5);
        CHECK(drop.source.drop(drop.at(0.95, 0.5)) == Qt::CopyAction);
        CHECK(drop.session.droppedTexts == std::vector<QString> { QStringLiteral("echo hi") });
        CHECK(drop.session.droppedUrls.empty());
        CHECK(drop.controller().lastPayload == static_cast<int>(contour::window::DropPayload::Text));
    }

    SECTION("a web link goes to the session's URL path, never typed as text")
    {
        auto const urls = QList<QUrl> { QUrl { QStringLiteral("https://example.org/") } };
        drop.source.mime.setUrls(urls);
        drop.dragOver(0.5, 0.5);
        CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::CopyAction);
        REQUIRE(drop.session.droppedUrls.size() == 1);
        CHECK(drop.session.droppedUrls.front() == urls);
        CHECK(drop.session.droppedTexts.empty());
    }

    SECTION("a mixed list is one insertDroppedUrls call, never joined into text")
    {
        // A page chooses its text/uri-list: joined with a line feed, the first line would run at the drop.
        auto const urls = QList<QUrl> { QUrl { QStringLiteral("https://a/$(touch${IFS}x)") },
                                        QUrl { QStringLiteral("file:///tmp/c.txt") },
                                        QUrl { QStringLiteral("https://b/") } };
        drop.source.mime.setUrls(urls);
        drop.dragOver(0.5, 0.5);
        CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::CopyAction);
        REQUIRE(drop.session.droppedUrls.size() == 1);
        CHECK(drop.session.droppedUrls.front() == urls);
        CHECK(drop.session.droppedTexts.empty());
    }

    CHECK(drop.controller().droppedZones.empty());
}

TEST_CASE(
    "pane card: the cursor refuses over the middle of the card and accepts again over an edge (offscreen)",
    "[contour][gui][qml][panecard][drop]")
{
    // Every move is answered with what a drop there would do, so the OS cursor shows it before release, and
    // the moves keep coming: leaving the middle for an edge accepts again.
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);

    drop.source.enter(drop.at(0.95, 0.5));
    drop.source.move(drop.at(0.95, 0.5));
    CHECK(drop.source.accepted);

    drop.source.move(drop.at(0.5, 0.5));
    CHECK_FALSE(drop.source.accepted); // the middle: forbidden, but the drag is still ours
    CHECK_FALSE(drop.part("dropRing").isVisible());

    drop.source.move(drop.at(0.05, 0.5));
    CHECK(drop.source.accepted);
    CHECK(QTest::qWaitFor([&] { return drop.part("dropGhost").isVisible(); }));

    // And a release over the middle ends as an ignored drop, so the tab tears off as it always did.
    drop.source.move(drop.at(0.5, 0.5));
    CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::IgnoreAction);
    CHECK(drop.controller().droppedZones.empty());
}

TEST_CASE("pane card: a zone the controller does not answer refuses the drag, it never accepts (offscreen)",
          "[contour][gui][qml][panecard][drop]")
{
    // `undefined` is what the engine gives for a call it could not make; `undefined !== none` would
    // accept. The card fails closed, and a zone from an earlier answer does not outlive it.
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);
    drop.dragOver(0.95, 0.5);
    REQUIRE(drop.source.accepted);
    REQUIRE(QTest::qWaitFor([&] { return drop.part("dropRing").isVisible(); }));

    drop.controller().answersUndefined = true;
    drop.source.move(drop.at(0.96, 0.5));
    CHECK(QTest::qWaitFor([&] { return !drop.part("dropRing").isVisible(); }));
    CHECK_FALSE(drop.part("dropGhost").isVisible());
    CHECK(drop.source.drop(drop.at(0.96, 0.5)) == Qt::IgnoreAction);
    CHECK(drop.controller().droppedZones.empty());

    // A new drag that is answered nothing at all is not accepted on entry either.
    drop.source.leave();
    drop.dragOver(0.95, 0.5);
    CHECK_FALSE(drop.source.accepted);
}

TEST_CASE("pane card: a drag the card cannot place is refused (offscreen)",
          "[contour][gui][qml][panecard][drop]")
{
    auto drop = DropHost {};
    drop.source.mime.setData("application/x-unknown", "x");
    drop.dragOver(0.5, 0.5);
    CHECK_FALSE(drop.source.accepted);
    CHECK_FALSE(drop.part("dropRing").isVisible());
    CHECK(drop.source.drop(drop.at(0.5, 0.5)) == Qt::IgnoreAction);
    CHECK(drop.session.droppedTexts.empty());
    CHECK(drop.session.droppedUrls.empty());
    CHECK(drop.controller().droppedZones.empty());
}

TEST_CASE("pane card: the ring and the ghost are gone once the drag leaves (offscreen)",
          "[contour][gui][qml][panecard][drop]")
{
    auto drop = DropHost {};
    drop.source.mime.setData(TabMime, TabJson);
    drop.dragOver(0.05, 0.5);
    CHECK(QTest::qWaitFor([&] { return drop.part("dropGhost").isVisible(); }));
    drop.source.leave();
    CHECK(QTest::qWaitFor(
        [&] { return !drop.part("dropRing").isVisible() && !drop.part("dropGhost").isVisible(); }));
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

TEST_CASE("The command palette's caret stands clear of its hint (offscreen)", "[contour][gui][qml][palette]")
{
    // The caret sits where the first typed letter will, and every style draws a placeholder from there:
    // the caret stood on the hint's first letter. Whatever style this process runs under.
    contour::test::QmlMessageCapture const warnings;
    QQmlEngine engine;
    MockPaletteController controller;
    auto const host = openPalette(engine, controller);
    REQUIRE(host.palette != nullptr);
    auto* filter = host.palette->findChild<QQuickItem*>(QStringLiteral("commandPaletteFilter"));
    REQUIRE(filter != nullptr);
    filter->forceActiveFocus();
    QCoreApplication::processEvents();

    auto const* hint = filter->findChild<QQuickItem*>(QStringLiteral("commandPaletteHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->isVisible());
    CHECK_FALSE(hint->property("text").toString().isEmpty());
    // No second hint from the style beneath it.
    CHECK(filter->property("placeholderText").toString().isEmpty());
    auto const caret = filter->property("cursorRectangle").toRectF();
    auto const* style =
        engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
    REQUIRE(style != nullptr);
    auto const hairline = style->property("borderWidth").toReal();
    CHECK(hint->mapToItem(filter, QPointF(0, 0)).x() >= caret.x() + caret.width() + hairline);

    // It steps aside for the first letter typed.
    filter->setProperty("text", QStringLiteral("split"));
    CHECK_FALSE(hint->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

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

    SECTION("the list is populated before the user types")
    {
        CHECK(list->property("count").toInt() == controller.model().rowCount());
        CHECK(list->property("count").toInt() > 0);
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
              == QString::fromStdString(contour::test::paneChordText(contour::actions::SplitVertical {})));
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
    waitForPopupClosed(*host.palette);

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

    // THE PIN: the command runs only once the popup has closed, never from inside its own closing, which
    // for this command would mean calling Popup.open() on top of a close that has not finished unwinding.
    REQUIRE_FALSE(host.palette->property("opened").toBool());
    CHECK(controller.ran.isEmpty());
    controller.observeOnRun = [&] {
        CHECK_FALSE(host.palette->property("visible").toBool());
    };

    // ... and with the popup at rest, it runs.
    REQUIRE(QTest::qWaitFor([&] { return !controller.ran.isEmpty(); }, contour::test::PopupSettleMs));

    // It ran, exactly once, and re-opened the palette — a fresh open rather than one stacked on a close.
    REQUIRE(controller.ran.size() == 1);
    CHECK(controller.ran.front() == QStringLiteral("OpenCommandPalette"));
    CHECK(QTest::qWaitFor([&] { return host.palette->property("opened").toBool(); },
                          contour::test::PopupSettleMs));
    controller.observeOnRun = {};

    // And one dismissal dismisses it.
    QMetaObject::invokeMethod(host.palette, "close");
    waitForPopupClosed(*host.palette);

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
    waitForPopupClosed(*host.palette);

    CHECK(controller.ran.isEmpty());
    CHECK_FALSE(host.palette->property("visible").toBool());
    CHECK(host.focusRestores() == 1);

    CHECK(warnings.count([](QString const& w) { return w.contains("TypeError"); }) == 0);
}
