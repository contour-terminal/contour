// SPDX-License-Identifier: Apache-2.0
//
// Model-level coverage for the "how many OS windows exist" decision that TerminalSessionManager's
// teardown relies on. The manager itself is not headless-constructible (it needs ContourGuiApp / PTY /
// QQuickWindow), and the per-display session bookkeeping it used to carry (DisplayState /
// detachSessionFromState / isLastActiveDisplay) is gone: session->display ownership now lives on the
// pane tree, and window identity is the manager's WindowController registry ("last window" ==
// controllers.size() == 1). The identity that decision derives from is the vtmux::SessionModel's window
// set, so the testable invariant — window count as windows are created and removed — is exercised here
// against the Qt-free model. (Per-window tab/pane isolation is covered in SessionModel_test.cpp.)

#include <catch2/catch_test_macros.hpp>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace vtmux;

namespace
{
/// A no-op ModelEvents sink: these tests assert model state (window set), not event ordering.
struct SilentEvents: ModelEvents
{
    void tabAboutToBeAdded(WindowId, int) override {}
    void tabAdded(WindowId, TabId, int) override {}
    void tabAboutToBeRemoved(WindowId, int) override {}
    void tabClosed(WindowId, TabId, int) override {}
    void tabAboutToBeMoved(WindowId, int, int) override {}
    void tabMoved(WindowId, TabId, int, int) override {}
    void activeTabChanged(WindowId, TabId, int) override {}
    void paneSplit(TabId, PaneId, PaneId) override {}
    void paneClosed(TabId, PaneId, PaneId) override {}
    void activePaneChanged(TabId, PaneId) override {}
    void paneRatioChanged(TabId, PaneId, double) override {}
    void tabTitleChanged(TabId) override {}
    void tabColorChanged(TabId) override {}
};

/// Counts the live windows the way the manager's teardown does ("last window" == one window remaining).
/// The model has no public window count, so tests track it via the create/remove calls they make; this
/// helper documents the intent at each assertion.
} // namespace

TEST_CASE("SessionModel: the last-window decision tracks the live window set", "[contour][manager][window]")
{
    SilentEvents events;
    uint64_t nextSession = 1;
    SessionModel model { events, [&] { return SessionId { nextSession++ }; } };

    // One window: it IS the last window (closing it would be a full teardown).
    auto* a = model.createWindow();
    REQUIRE(a != nullptr);
    REQUIRE(model.window(a->id()) == a);

    // A second in-process window: now closing either is NOT the last window (the sibling must survive).
    auto* b = model.createWindow();
    REQUIRE(b != nullptr);
    CHECK(a->id().value != b->id().value);
    CHECK(model.window(a->id()) == a);
    CHECK(model.window(b->id()) == b);

    // Capture the ids BEFORE removal: removeWindow() destroys the Window, so a->id()/b->id() afterwards
    // would read freed memory (caught by ASan).
    auto const aId = a->id();
    auto const bId = b->id();

    // Closing A leaves exactly B — so B is now the last window and its state is untouched.
    model.removeWindow(aId);
    CHECK(model.window(aId) == nullptr); // A is gone
    CHECK(model.window(bId) == b);       // B survives intact (would-be full teardown target)

    // Closing B (the last window) removes the final window.
    model.removeWindow(bId);
    CHECK(model.window(bId) == nullptr);
}

TEST_CASE("SessionModel: removing a window with tabs closes only that window's tabs",
          "[contour][manager][window]")
{
    // The manager terminates a closing window's sessions and removes its vtmux::Window; a sibling window's
    // tabs/sessions must be untouched. This is the model half of the per-window teardown invariant.
    SilentEvents events;
    uint64_t nextSession = 1;
    SessionModel model { events, [&] { return SessionId { nextSession++ }; } };

    auto* a = model.createWindow();
    auto* b = model.createWindow();
    model.createTab(a->id());
    model.createTab(a->id());
    auto* bTab = model.createTab(b->id());

    REQUIRE(a->tabCount() == 2);
    REQUIRE(b->tabCount() == 1);

    // Capture before removal: removeWindow() destroys A, so a->id() afterwards reads freed memory.
    auto const aId = a->id();
    model.removeWindow(aId);

    CHECK(model.window(aId) == nullptr);
    REQUIRE(model.window(b->id()) == b);
    CHECK(b->tabCount() == 1); // B's tab survived A's teardown
    CHECK(model.findTab(bTab->id()) == bTab);
}
