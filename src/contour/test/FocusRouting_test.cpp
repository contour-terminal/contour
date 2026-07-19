// SPDX-License-Identifier: Apache-2.0
//
// End-to-end coverage of terminal (VT) focus routing: a tab or pane switch must send a focus-OUT to
// the session that had focus and a focus-IN to the one that gains it, so a shell running under
// DECSET 1004 (FocusTracking) receives CSI O / CSI I.
//
// Why this needs its own file. Qt fires NO focus event on a tab switch: PaneNode.qml's leaf Loader
// stays active, so the same TerminalDisplay item is reused and only its `session:` property rebinds.
// TerminalSessionManager::syncFocusForWindow is the compensating seam, and it is reached from the
// model's activeTabChanged / activePaneChanged. TerminalSession_test covers the setFocusedSession
// PRIMITIVE; these cases cover the CHAIN, down to the bytes in the PTY.
//
// These are also the regression pins for the focus-ownership refactor. syncFocusForWindow used to
// gate on `_activeDisplay != nullptr` — a raw Qt pointer that is null between a display teardown and
// the next Qt focus-in, and null in every headless test. A tab switch in that gap notified nobody,
// and the path was untestable. Ownership is now a modelled vtmux::WindowId, so the production path
// is the tested path. All cases here run headless: no [display] tag, nothing skips offscreen.

#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtbackend/primitives.h>

#include <vtpty/MockPty.h>

#include <crispy/escape.h>

#include <QtGui/QWindow>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

namespace
{

using contour::test::mockPtyOf;
using contour::test::MockPtySessionFactory;
using contour::test::ScopedController;
using contour::test::TestApp;

/// Turns on focus reporting (DECSET 1004) for @p session, as an application like vim would.
void enableFocusTracking(contour::TerminalSession& session)
{
    session.terminal().setMode(vtbackend::DECMode::FocusTracking, true);
}

/// Drops whatever the session has written so far, so a following assertion sees only the bytes the
/// operation under test produced.
void clearPtyInput(contour::TerminalSession& session)
{
    mockPtyOf(session).stdinBuffer().clear();
}

/// Escaped view of everything @p session wrote towards the shell (readable Catch2 diffs).
[[nodiscard]] std::string writtenBy(contour::TerminalSession& session)
{
    return crispy::escape(mockPtyOf(session).stdinBuffer());
}

/// The session backing the active pane of tab @p index in @p window.
[[nodiscard]] contour::TerminalSession* sessionOfTabAt(contour::TerminalSessionManager& manager,
                                                       vtmux::WindowId window,
                                                       int index)
{
    auto* modelWindow = manager.model().window(window);
    REQUIRE(modelWindow != nullptr);
    auto* tab = modelWindow->tabAt(index);
    REQUIRE(tab != nullptr);
    REQUIRE(tab->activePane() != nullptr);
    return manager.sessionForId(tab->activePane()->session());
}

/// Closes every tab of @p controller, mirroring how the multi-window tests wind sessions down.
void closeAllTabs(contour::WindowController& controller)
{
    for (int row = controller.count() - 1; row >= 0; --row)
        controller.closeTabAtIndex(row);
}

} // namespace

TEST_CASE("focus: switching tabs focuses out the old tab and focuses in the new one", "[contour][focus][tab]")
{
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();
    ScopedController window { manager };

    // A freshly spawned window owns focus, so a tab switch moves VT focus even before the window has
    // ever been clicked (no Qt focus event has been delivered in this headless run).
    REQUIRE(manager.focusedWindow() == window.id);

    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->count() == 2);
    REQUIRE(window->activeTabIndex() == 1);

    auto* a = sessionOfTabAt(manager, window.id, 0);
    auto* b = sessionOfTabAt(manager, window.id, 1);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);

    // Creating the second tab already moved focus off the first: exactly one session is ever focused.
    CHECK(b->terminal().focused());
    CHECK_FALSE(a->terminal().focused());

    window->activateTab(0);
    CHECK(a->terminal().focused());
    CHECK_FALSE(b->terminal().focused());

    window->activateTab(1);
    CHECK(b->terminal().focused());
    CHECK_FALSE(a->terminal().focused());

    closeAllTabs(*window);
}

TEST_CASE("focus: a tab switch writes CSI O and CSI I to the two tabs' PTYs under DECMode 1004",
          "[contour][focus][tab][modes]")
{
    // The point of the whole chain: the bytes must actually reach the shell, not merely flip an
    // internal flag. Terminal::flushInput() is what writes them into the PTY.
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->count() == 2);

    auto* a = sessionOfTabAt(manager, window.id, 0);
    auto* b = sessionOfTabAt(manager, window.id, 1);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    SECTION("with focus reporting enabled both ends of the switch are reported")
    {
        enableFocusTracking(*a);
        enableFocusTracking(*b);
        clearPtyInput(*a);
        clearPtyInput(*b);

        // Tab 1 (b) is active; switching to tab 0 must tell b it lost focus and a that it gained it.
        window->activateTab(0);
        CHECK(writtenBy(*b) == crispy::escape("\033[O"));
        CHECK(writtenBy(*a) == crispy::escape("\033[I"));

        clearPtyInput(*a);
        clearPtyInput(*b);

        // ...and symmetrically on the way back.
        window->activateTab(1);
        CHECK(writtenBy(*a) == crispy::escape("\033[O"));
        CHECK(writtenBy(*b) == crispy::escape("\033[I"));
    }

    SECTION("with focus reporting disabled nothing reaches either PTY")
    {
        // Focus still moves internally (the renderer needs it); only the notification is gated.
        clearPtyInput(*a);
        clearPtyInput(*b);

        window->activateTab(0);
        CHECK(mockPtyOf(*a).stdinBuffer().empty());
        CHECK(mockPtyOf(*b).stdinBuffer().empty());
        CHECK(a->terminal().focused());
        CHECK_FALSE(b->terminal().focused());
    }

    closeAllTabs(*window);
}

TEST_CASE("focus: switching panes within a tab moves focus between the panes' sessions",
          "[contour][focus][pane]")
{
    // The same seam, reached through activePaneChanged instead of activeTabChanged.
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();
    ScopedController window { manager };

    window->createNewTab();
    REQUIRE(window->count() == 1);

    auto* modelWindow = manager.model().window(window.id);
    REQUIRE(modelWindow != nullptr);
    auto* tab = modelWindow->tabAt(0);
    REQUIRE(tab != nullptr);

    auto const firstPaneId = tab->activePane()->id();
    auto* first = window->activeSession();
    REQUIRE(first != nullptr);

    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    auto const secondPaneId = tab->activePane()->id();
    REQUIRE(firstPaneId != secondPaneId);

    auto* second = window->activeSession();
    REQUIRE(second != nullptr);
    REQUIRE(first != second);

    // A split makes the new pane active from birth, so focus has already moved onto it.
    CHECK(second->terminal().focused());
    CHECK_FALSE(first->terminal().focused());

    enableFocusTracking(*first);
    enableFocusTracking(*second);
    clearPtyInput(*first);
    clearPtyInput(*second);

    // Move focus back to the original pane by id (orientation-independent, unlike a direction move).
    manager.activatePane(tab->id(), firstPaneId);
    CHECK(first->terminal().focused());
    CHECK_FALSE(second->terminal().focused());
    CHECK(writtenBy(*second) == crispy::escape("\033[O"));
    CHECK(writtenBy(*first) == crispy::escape("\033[I"));

    closeAllTabs(*window);
}

TEST_CASE("focus: moving a tab to another window does not carry VT focus into the unfocused window",
          "[contour][focus][multiwindow]")
{
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };

    windowA->createNewTab();
    windowA->createNewTab();
    windowB->createNewTab();
    REQUIRE(windowA->count() == 2);
    REQUIRE(windowB->count() == 1);

    // Spawning B took ownership; state the intent explicitly so the test does not lean on creation order.
    manager.setFocusedWindow(windowA.id);
    REQUIRE(manager.focusedWindow() == windowA.id);

    auto* moved = sessionOfTabAt(manager, windowA.id, 0);
    auto* stayed = sessionOfTabAt(manager, windowA.id, 1);
    REQUIRE(moved != nullptr);
    REQUIRE(stayed != nullptr);

    // The transplant routes through activeTabChanged on the DESTINATION window, which does not own
    // focus — so the moved tab must not steal it.
    windowB->moveTabIntoThisWindow(windowA.id.value, 0, 0);
    CHECK_FALSE(moved->terminal().focused());
    CHECK(stayed->terminal().focused());

    // Handing ownership to B focuses its active leaf and releases A's.
    manager.setFocusedWindow(windowB.id);
    CHECK(moved->terminal().focused());
    CHECK_FALSE(stayed->terminal().focused());

    closeAllTabs(*windowA);
    closeAllTabs(*windowB);
}

TEST_CASE("focus: bindWindow wires OS window activation to focus ownership", "[contour][focus][window]")
{
    // Alt-tabbing away from Contour must notify a DECSET 1004 application even when no display holds Qt
    // item focus, so bindWindow connects QWindow::activeChanged to the manager's grant/revoke. The
    // connection is the whole mechanism: without it the feature is silently absent, and no assertion
    // about focus behaviour would notice. Asserted via disconnect(), which reports whether a matching
    // connection existed -- deterministic, and independent of whether a window manager will activate a
    // test window.
    TestApp app;
    auto& manager = app.manager();
    ScopedController window { manager };

    auto osWindow = std::make_unique<QQuickWindow>();
    window->bindWindow(osWindow.get());

    // NB: this consumes the connection it verifies; nothing below depends on it.
    CHECK(QObject::disconnect(osWindow.get(), &QWindow::activeChanged, window.controller, nullptr));

    // And the two ends of what that signal drives: a grant focuses the window's active leaf, a revoke
    // leaves nothing focused.
    manager.clearFocusedWindow(window.id);
    CHECK_FALSE(manager.focusedWindow().has_value());
    manager.setFocusedWindow(window.id);
    CHECK(manager.focusedWindow() == window.id);
}

TEST_CASE("focus: closing the focused window drops focus ownership and leaves no terminal focused",
          "[contour][focus][multiwindow]")
{
    // Ownership must not outlive its window: a stale WindowId would let a later model event in an
    // unrelated window re-grant focus through syncFocusForWindow.
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();

    ScopedController window { manager };
    window->createNewTab();
    REQUIRE(manager.focusedWindow() == window.id);

    closeAllTabs(*window);
    manager.removeWindowController(window.id);

    CHECK_FALSE(manager.focusedWindow().has_value());
}

TEST_CASE("focus: a background window's tab activation cannot steal focus", "[contour][focus][multiwindow]")
{
    // The anti-steal property the ownership gate exists for: only the focus-owning window may move VT
    // focus, so a background window applying a layout or activating a tab leaves the user's terminal
    // focused.
    TestApp app { std::make_unique<MockPtySessionFactory>() };
    auto& manager = app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };

    windowA->createNewTab();
    windowB->createNewTab();
    windowB->createNewTab();

    manager.setFocusedWindow(windowA.id);
    REQUIRE(manager.focusedWindow() == windowA.id);

    auto* foreground = windowA->activeSession();
    REQUIRE(foreground != nullptr);
    auto* background = sessionOfTabAt(manager, windowB.id, 0);
    REQUIRE(background != nullptr);
    REQUIRE(foreground != background);

    enableFocusTracking(*foreground);
    clearPtyInput(*foreground);

    windowB->activateTab(0);

    CHECK(manager.focusedWindow() == windowA.id);
    CHECK(foreground->terminal().focused());
    CHECK_FALSE(background->terminal().focused());
    // The foreground shell was not told anything happened.
    CHECK(mockPtyOf(*foreground).stdinBuffer().empty());

    closeAllTabs(*windowA);
    closeAllTabs(*windowB);
}
