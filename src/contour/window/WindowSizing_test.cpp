// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests for the WindowController's window-geometry authority: the WM-hint and show-mode
// paths that mutate a real (offscreen) QQuickWindow, and the displayless no-op guards.
//
// The pure hints MATH (min/base/increment values from cell size, margins, scale and chrome) is
// covered by WindowGeometry_test; these tests cover the controller WIRING on top of it: the declared
// chrome property, the fullscreen/maximize increment-zeroing (WMs must not cell-snap a fullscreen
// window), the displayless early-outs, and that tab switching never touches window geometry.

#include <contour/ContourGuiApp.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/screen/GutterSettings.hpp>

#include <vtpty/MockPty.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <fstream>
#include <memory>
#include <string_view>

#include <QtTest/QSignalSpy>
#include <vtworkspace/SessionModel.hpp>

namespace
{

using contour::test::ScopedController;
using contour::test::TestApp;

} // namespace

TEST_CASE("chromeHeight is a plain declared property with change notification", "[contour][sizing]")
{
    TestApp app;
    ScopedController controller { app.manager() };
    QSignalSpy const spy(controller.controller, &contour::window::WindowController::chromeHeightChanged);

    // With no active display the setter is a pure property write + signal (no hint refresh) — the
    // declared-chrome contract Main.qml relies on when it binds chromeHeight before any display
    // exists.
    controller->setChromeHeight(34);
    CHECK(controller->chromeHeight() == 34);
    CHECK(spy.count() == 1);

    // Same-value writes are deduped (no signal storm from a re-evaluating QML binding).
    controller->setChromeHeight(34);
    CHECK(spy.count() == 1);

    controller->setChromeHeight(0);
    CHECK(controller->chromeHeight() == 0);
    CHECK(spy.count() == 2);
}

TEST_CASE("bindWindow(nullptr) and displayless showInitial are safe no-ops", "[contour][sizing]")
{
    TestApp app;
    ScopedController controller { app.manager() };

    // No window bound at all: everything early-outs.
    controller->bindWindow(nullptr);
    controller->showInitial();

    // Window bound but no session/display: showInitial must still SHOW the window (the "never leave
    // the user windowless" fallback) at Qt's default size rather than crash or stay unmapped.
    QQuickWindow window;
    controller->bindWindow(&window);
    REQUIRE_FALSE(window.isVisible());
    controller->showInitial();
    CHECK(window.isVisible());
}

TEST_CASE("first window restores its location and saves normal moves", "[contour][sizing][window-position]")
{
    TestApp app;
    app.app().config().rememberWindowPosition = true;
    app.app().config().configFile = "test-config/contour.yml";
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    auto const available = QGuiApplication::primaryScreen()->availableGeometry();
    auto const initial = available.topLeft() + QPoint(20, 20);
    store.value = contour::window::WindowPosition {
        .position = { 20, 20 },
        .monitor = static_cast<int>(QGuiApplication::screens().indexOf(QGuiApplication::primaryScreen()))
    };
    ScopedController controller { app.manager() };
    QQuickWindow window;
    window.resize(200, 100);
    controller->bindWindow(&window);
    controller->showInitial();
    CHECK(window.position() == initial);
    CHECK(store.loads == 1);
    ScopedController additionalWindow { app.manager() };
    CHECK(store.loads == 1);

    auto const moved = initial + QPoint(20, 30);
    window.setPosition(moved);
    QCoreApplication::processEvents();
    window.showMaximized();
    QCoreApplication::processEvents();
    SECTION("enabled at close")
    {
        controller->closeWindow();
        REQUIRE(store.value);
        CHECK(store.value->position == moved - available.topLeft());
        CHECK(store.saves == 1);
    }
    SECTION("disabled after startup leaves existing state alone")
    {
        app.app().config().rememberWindowPosition = false;
        controller->closeWindow();
        REQUIRE(store.value);
        CHECK(store.value->position == QPoint(20, 20));
        CHECK(store.saves == 0);
    }
}

TEST_CASE("unavailable saved monitor falls back to an available monitor", "[contour][window-position]")
{
    TestApp app;
    app.app().config().configFile = "test-config/contour.yml";
    app.app().config().rememberWindowPosition = true;
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    store.value =
        contour::window::WindowPosition { .position = { 20, 20 },
                                          .monitor = static_cast<int>(QGuiApplication::screens().size()) };
    ScopedController controller { app.manager() };
    QQuickWindow window;
    window.resize(200, 100);
    controller->bindWindow(&window);
    controller->showInitial();
    REQUIRE(window.screen());
    CHECK(window.position() == window.screen()->availableGeometry().topLeft() + QPoint(20, 20));
    controller->closeWindow();
    REQUIRE(store.value);
    CHECK(store.value->monitor == QGuiApplication::screens().indexOf(window.screen()));
}

TEST_CASE("disabled window position persistence neither loads nor saves", "[contour][window-position]")
{
    TestApp app;
    app.app().config().configFile = "test-config/contour.yml";
    app.app().config().rememberWindowPosition = false;
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    store.value = contour::window::WindowPosition { .position = { 25, 30 }, .monitor = 0 };
    ScopedController controller { app.manager() };
    QQuickWindow window;
    window.resize(200, 100);
    controller->bindWindow(&window);
    controller->showInitial();
    window.setPosition(80, 90);
    controller->closeWindow();
    CHECK(store.loads == 0);
    CHECK(store.saves == 0);
    REQUIRE(store.value);
    CHECK(store.value->position == QPoint(25, 30));
}

TEST_CASE("first startup in every presentation mode saves a normal placement without prior state",
          "[contour][sizing][window-position][native]")
{
    auto const initialState = GENERATE(Qt::WindowNoState, Qt::WindowMaximized, Qt::WindowFullScreen);
    CAPTURE(initialState);

    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    app.app().config().rememberWindowPosition = true;
    app.app().config().configFile = "test-config/contour.yml";
    auto& profile = app.app().config().profiles.value().at("main");
    profile.showTitleBar = true;
    profile.terminalSize = vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(30) };
    profile.maximized = initialState == Qt::WindowMaximized;
    profile.fullscreen = initialState == Qt::WindowFullScreen;
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    REQUIRE_FALSE(store.value);

    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    controller->seedTitleBarVisible(true);
    REQUIRE(window.screen());
    window.setPosition(window.screen()->availableGeometry().topLeft() + QPoint(80, 80));
    auto* session = app.manager().createSession(controller->windowId());
    REQUIRE(session);
    contour::test::FakeDisplaySurface surface;
    surface.attachedSession = session;
    session->attachDisplay(surface);
    controller->showInitial();
    REQUIRE(window.screen());
    CHECK(window.windowState() == initialState);

    // Stop borrowing the stack display before closeWindow terminates the backing session.
    session->detachDisplay(surface);
    surface.attachedSession = nullptr;
    controller->closeWindow();
    REQUIRE(store.value);
    CHECK(store.loads == 1);
    CHECK(store.saves == 1);
    CHECK(store.value->position == QPoint(80, 80));
    CHECK(store.value->monitor == QGuiApplication::screens().indexOf(window.screen()));
    auto const available = window.screen()->availableGeometry();
    CHECK(store.value->position.x() >= 0);
    CHECK(store.value->position.y() >= 0);
    CHECK(store.value->position.x() < available.width());
    CHECK(store.value->position.y() < available.height());
}

#ifdef _WIN32
TEST_CASE("native startup clamps the decorated window inside its monitor",
          "[contour][sizing][window-position][native]")
{
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        SKIP("Requires the native Windows platform.");

    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    app.app().config().rememberWindowPosition = true;
    app.app().config().configFile = "test-config/contour.yml";
    auto& profile = app.app().config().profiles.value().at("main");
    profile.showTitleBar = true;
    profile.terminalSize = vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(30) };
    auto& store = static_cast<contour::test::InMemoryWindowPositionStore&>(app.app().windowPositionStore());
    auto const monitor =
        GENERATE(Catch::Generators::range(0, static_cast<int>(QGuiApplication::screens().size())));
    auto const savedPosition = GENERATE(QPoint(-100000, 100000), QPoint(100000, -100000));
    CAPTURE(monitor, savedPosition);
    store.value = contour::window::WindowPosition { .position = savedPosition, .monitor = monitor };

    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    controller->seedTitleBarVisible(true);
    auto* session = app.manager().createSession(controller->windowId());
    REQUIRE(session);
    contour::test::FakeDisplaySurface surface;
    surface.attachedSession = session;
    session->attachDisplay(surface);
    controller->showInitial();
    QCoreApplication::processEvents();
    REQUIRE(window.screen());
    CHECK(window.screen() == QGuiApplication::screens()[monitor]);
    CHECK(window.frameMargins().top() > 0);
    CHECK(window.screen()->availableGeometry().contains(window.frameGeometry()));
    auto const expectedPosition = window.position() - window.screen()->availableGeometry().topLeft();
    session->detachDisplay(surface);
    surface.attachedSession = nullptr;
    controller->closeWindow();
    REQUIRE(store.value);
    CHECK(store.value->position == expectedPosition);
    CHECK(store.saves == 1);
}
#endif

TEST_CASE("startup row snapping respects an off-grid minimum before focus", "[contour][sizing][row-resize]")
{
    TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& appConfig = app.app().config();
    appConfig.profiles.value().at("main").margins =
        contour::config::WindowMargins { .horizontal = contour::config::HorizontalMargin { 0 },
                                         .vertical = contour::config::VerticalMargin { 0 } };
    appConfig.snapToRows = true;

    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    controller->setChromeHeight(38);
    auto* session = app.manager().createSession(controller->windowId());
    REQUIRE(session != nullptr);
    contour::test::FakeDisplaySurface surface;
    surface.cellSizeValue = { vtbackend::Width(11), vtbackend::Height(21) };
    surface.attachedSession = session;
    session->attachDisplay(surface);

    // A size constraint can override the otherwise row-aligned profile size. No focusDisplay()
    // call: startup must resolve the session's display before the first native focus-in event.
    window.setMinimumHeight(673);
    controller->showInitial();
    CHECK(window.height() == 689); // 31 rows of 21 px, plus 38 px of chrome
    session->detachDisplay(surface);
    surface.attachedSession = nullptr;
}

TEST_CASE("changing row snapping refreshes live size hints", "[contour][sizing][row-resize]")
{
    contour::test::ConfiguredTestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const writeConfig = [&](std::string_view value) {
        std::ofstream(app.configFile()) << "default_profile: main\nsnap_to_rows: " << value << '\n';
    };
    writeConfig("false");
    REQUIRE(app.manager().reloadAllSessions().has_value());
    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    window.resize(800, 600);
    auto* session = app.manager().createSession(controller->windowId());
    REQUIRE(session != nullptr);
    contour::test::FakeDisplaySurface surface;
    surface.attachedSession = session;
    session->attachDisplay(surface);
    auto const size = window.size();

    // No font or margin change: toggling this global option must still refresh the hints, including
    // on disable so X11's physical override is replaced with Qt's ordinary hints.
    for (auto const value: { "true", "false" })
    {
        window.setMinimumSize(QSize(1, 1));
        writeConfig(value);
        CHECK(app.manager().reloadAllSessions().has_value());
        CHECK(window.minimumHeight() > 1);
        CHECK(window.size() == size);
    }
    session->detachDisplay(surface);
    surface.attachedSession = nullptr;
}

TEST_CASE("fullscreen and maximize zero the WM size increment", "[contour][sizing]")
{
    // WM_NORMAL_HINTS cell-snapping (PResizeInc) must be suspended while fullscreen/maximized:
    // the window has to fill the screen exactly, not the nearest cell multiple. The show-mode
    // paths route through showWithoutSizeIncrements(); assert the actual QWindow property writes.
    TestApp app;
    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);

    // A displayless TerminalDisplay resolves to the bound window via osWindowFor()'s fallback.
    contour::display::TerminalDisplay requester;

    window.setSizeIncrement(QSize(10, 20)); // as if hints had been applied
    controller->setWindowFullScreen(requester);
    CHECK(window.sizeIncrement() == QSize(0, 0));

    window.setSizeIncrement(QSize(10, 20));
    controller->setWindowMaximized(requester);
    CHECK(window.sizeIncrement() == QSize(0, 0));

    // setWindowNormal without a session shows normal and leaves hint refresh to the display
    // focus-in (updateSizeHintsFor early-outs on !hasSession) — it must not crash displayless.
    controller->setWindowNormal(requester);
    CHECK(window.visibility() != QWindow::FullScreen);
}

TEST_CASE("updateSizeHintsFor is a displayless no-op and leaves the window untouched", "[contour][sizing]")
{
    TestApp app;
    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    contour::display::TerminalDisplay requester; // hasSession() == false

    auto const before = window.sizeIncrement();
    controller->updateSizeHintsFor(requester,
                                   contour::window::WindowController::HintApplyMode::RespectWindowState);
    CHECK(window.sizeIncrement() == before);
    CHECK(window.minimumSize() == QSize(0, 0));
}

TEST_CASE("the size hints reserve the gutter the configuration lays out", "[contour][sizing][gutter]")
{
    // Line numbers six columns wide beside the block column: seven cells, where the fold markers alone
    // took one. A minimum computed from the fold switch would let the window shrink below a page the
    // terminal still needs those six columns for.
    TestApp app;
    app.app().config().gutter.value().lineNumbers = vtbackend::LineNumberMode::Absolute;
    auto session = contour::test::makeTestSession(&app.app().sessionsManager(),
                                                  std::make_unique<vtpty::MockPty>(vtbackend::PageSize {
                                                      vtbackend::LineCount(25), vtbackend::ColumnCount(80) }),
                                                  app.app());
    contour::test::FakeDisplaySurface requester;
    requester.attachedSession = session.get();

    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window); // the requester has no window of its own, so the hints land here
    controller->updateSizeHintsFor(requester,
                                   contour::window::WindowController::HintApplyMode::RespectWindowState);

    // The minimum is the one hint every platform applies (geometry::sizeHintPolicyFor). Eight cells -- six
    // of line numbers, two of block column -- behind the blank lead-in every reserved gutter begins with.
    auto const gutterDevicePx = contour::geometry::gutterLeadIn(requester.devicePixelRatioValue)
                                + (8 * unbox<int>(requester.cellSizeValue.width));
    auto const expected = contour::geometry::sizeHintsFor(
        requester.cellSizeValue,
        contour::session::toGeometryMargins(session->profile().margins.value()),
        requester.devicePixelRatioValue,
        contour::geometry::Chrome {},
        gutterDevicePx);
    CHECK(window.minimumSize() == QSize(expected.minimum.width, expected.minimum.height));
}

TEST_CASE("tab switching never touches window geometry", "[contour][sizing]")
{
    // The sizing rework's core invariant: window size changes flow ONLY through the explicit
    // choke points (showInitial / resizeWindowForPage / DPR settlement). A tab switch re-binds
    // content but must leave size, minimum size, and increments exactly as they are.
    TestApp app;
    auto& manager = app.manager();
    ScopedController controller { manager };
    QQuickWindow window;
    controller->bindWindow(&window);

    REQUIRE(manager.model().createTab(controller->windowId()) != nullptr);
    REQUIRE(manager.model().createTab(controller->windowId()) != nullptr);

    window.resize(640, 480);
    window.setSizeIncrement(QSize(10, 20));
    window.setMinimumSize(QSize(104, 138));
    auto const size = window.size();
    auto const increment = window.sizeIncrement();
    auto const minimum = window.minimumSize();

    controller->activateTab(1);
    controller->activateTab(0);
    QCoreApplication::processEvents();

    CHECK(window.size() == size);
    CHECK(window.sizeIncrement() == increment);
    CHECK(window.minimumSize() == minimum);
}

TEST_CASE("a maximized window survives a tab switch", "[contour][sizing]")
{
    // Same invariant as splitting: content re-binding (here a tab switch) must never drop the window's
    // show-state. Whatever the offscreen platform settles the visibility to after setWindowMaximized(),
    // activating another tab and switching back must leave it unchanged — the tab path must not route
    // through any show-mode call.
    TestApp app;
    auto& manager = app.manager();
    ScopedController controller { manager };
    QQuickWindow window;
    controller->bindWindow(&window);
    window.show();
    QCoreApplication::processEvents();

    REQUIRE(manager.model().createTab(controller->windowId()) != nullptr);
    REQUIRE(manager.model().createTab(controller->windowId()) != nullptr);

    contour::display::TerminalDisplay requester;
    controller->setWindowMaximized(requester);
    QCoreApplication::processEvents();
    auto const visibility = window.visibility();

    controller->activateTab(1);
    controller->activateTab(0);
    QCoreApplication::processEvents();

    CHECK(window.visibility() == visibility);
}

TEST_CASE("toggleMaximized routes both directions through the show-mode protocol", "[contour][sizing]")
{
    // The QML window controls / title-bar double-click entry point: maximizing must clear the WM
    // size increments (showWithoutSizeIncrements), restoring must show normal again. Displayless
    // fallbacks are exercised here (no _activeDisplay); the display-full path is covered by the
    // display-gated harness.
    TestApp app;
    ScopedController controller { app.manager() };

    // Without a bound window the toggle is a safe no-op.
    controller->toggleMaximized();
    controller->minimizeWindow();

    QQuickWindow window;
    controller->bindWindow(&window);
    window.show();
    QCoreApplication::processEvents();

    window.setSizeIncrement(QSize(10, 20));
    controller->toggleMaximized(); // -> maximized (increments zeroed)
    QCoreApplication::processEvents();
    CHECK(window.sizeIncrement() == QSize(0, 0));

    controller->toggleMaximized(); // -> back to normal
    QCoreApplication::processEvents();
    CHECK(window.visibility() != QWindow::Maximized);

    controller->minimizeWindow();
    QCoreApplication::processEvents();
    CHECK_NOTHROW(window.visibility());
}

TEST_CASE("content-driven resizes refuse displayless/sessionless requesters", "[contour][sizing]")
{
    // resizeWindowForPage/ForContentPixels are the grid->window choke point; without a session they
    // must refuse (return false) and never touch the window.
    TestApp app;
    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);
    window.resize(640, 480);

    contour::display::TerminalDisplay requester; // hasSession() == false
    auto const before = window.size();
    CHECK_FALSE(controller->resizeWindowForPage(
        requester, vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) }));
    CHECK_FALSE(controller->resizeWindowForContentPixels(
        requester, vtbackend::ImageSize { vtbackend::Width(800), vtbackend::Height(600) }));
    CHECK(window.size() == before);
}

TEST_CASE("a DevicePixelRatioChange event reaches the scale-settlement handler", "[contour][sizing]")
{
    // bindWindow installs the controller as an event filter for QEvent::DevicePixelRatioChange (no
    // QWindow signal exists for it). Synthesize the event: with no active display the settlement
    // handler early-outs — the point is that the filter route itself is wired and safe.
    TestApp app;
    ScopedController controller { app.manager() };
    QQuickWindow window;
    controller->bindWindow(&window);

    auto event = QEvent(QEvent::DevicePixelRatioChange);
    CHECK_NOTHROW(QCoreApplication::sendEvent(&window, &event));
}

TEST_CASE("tabWorkingDirectory answers safely for rows that do not exist", "[contour][tabs]")
{
    // The tab hover tooltip calls this by row. A row can vanish between the hover starting and the
    // tooltip opening (the tab closes underneath it), so an out-of-range row must be an empty answer
    // rather than a walk off the model.
    TestApp app;
    ScopedController const controller { app.manager() };

    CHECK(controller.controller->tabWorkingDirectory(-1).isEmpty());
    CHECK(controller.controller->tabWorkingDirectory(0).isEmpty());
    CHECK(controller.controller->tabWorkingDirectory(9999).isEmpty());
}
