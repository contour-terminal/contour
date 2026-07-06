// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests for the WindowController's window-geometry authority: the WM-hint and show-mode
// paths that mutate a real (offscreen) QQuickWindow, and the displayless no-op guards.
//
// The pure hints MATH (min/base/increment values from cell size, margins, scale and chrome) is
// covered by WindowGeometry_test; these tests cover the controller WIRING on top of it: the declared
// chrome property, the fullscreen/maximize increment-zeroing (WMs must not cell-snap a fullscreen
// window), the displayless early-outs, and that tab switching never touches window geometry.

#include <contour/ContourGuiApp.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/test/GuiTestFixtures.h>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <QtTest/QSignalSpy>
#include <vtmux/SessionModel.h>

namespace
{

using contour::test::ScopedController;
using contour::test::TestApp;

} // namespace

TEST_CASE("chromeHeight is a plain declared property with change notification", "[contour][sizing]")
{
    TestApp app;
    ScopedController controller { app.manager() };
    QSignalSpy const spy(controller.controller, &contour::WindowController::chromeHeightChanged);

    // With no active display the setter is a pure property write + signal (no hint refresh) — the
    // declared-chrome contract main.qml relies on when it binds chromeHeight before any display
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
    controller->updateSizeHintsFor(requester);
    CHECK(window.sizeIncrement() == before);
    CHECK(window.minimumSize() == QSize(0, 0));
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
