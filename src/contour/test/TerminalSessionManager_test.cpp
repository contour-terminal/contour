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

#include <contour/Config.h>
#include <contour/TerminalSessionManager.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtbackend/primitives.h>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

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

// Manager-level coverage for applyLayoutToWindow, exercised directly (rather than through
// launchLayout) so the test needs no config injection or acting-session setup: it builds a
// contour::config::Layout by hand and asserts the resulting real, PTY-backed tabs/colors on the
// manager's authoritative vtmux::SessionModel.
TEST_CASE("TerminalSessionManager: applyLayoutToWindow builds tabs with colors", "[manager][layout]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController win { app.manager() };

    // Build a 3-tab layout in C++ (no config needed). Panes set NO profile, so no config lookup
    // happens. t2 is a vertical split of two leaves, covering the multi-pane realization path.
    contour::config::Layout layout;
    contour::config::LayoutTab t0;
    t0.root.command = "echo a";
    contour::config::LayoutTab t1;
    t1.root.command = "echo b";
    t1.color = vtbackend::RGBColor { "#112233" };
    contour::config::LayoutTab t2;
    t2.root.orientation = vtmux::SplitState::Vertical;
    contour::config::LayoutPane left;
    left.command = "echo left";
    contour::config::LayoutPane right;
    right.command = "echo right";
    t2.root.children = { left, right };
    layout.tabs = { t0, t1, t2 };

    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    auto* window = app.manager().model().window(win.id);
    REQUIRE(window != nullptr);
    CHECK(window->tabCount() == 3); // three tabs created from the layout
    auto* colored = window->tabAt(1);
    REQUIRE(colored != nullptr);
    CHECK(colored->color(vtmux::TabColorSource::User).has_value()); // second tab got its color
    auto* split = window->tabAt(2);
    REQUIRE(split != nullptr);
    CHECK(split->paneCount() == 2); // the split tab realized both leaves

    // Every leaf pane is backed by a REAL session created through the mock PTY factory (not just a
    // model tab with no backing session): the factory recorded one createPty() call per leaf, in
    // order, carrying that leaf's command.
    REQUIRE(factory->requestedCommandOverrides.size() == 4);
    CHECK(factory->requestedCommandOverrides[0]->program == "echo a");
    CHECK(factory->requestedCommandOverrides[1]->program == "echo b");
    CHECK(factory->requestedCommandOverrides[2]->program == "echo left");
    CHECK(factory->requestedCommandOverrides[3]->program == "echo right");
}

// Manager-level coverage for saveWindowLayout: builds a real, PTY-backed 2-tab window via
// applyLayoutToWindow (so each leaf session has a genuine launchedCommand()), saves it under a name,
// and asserts BOTH that layouts.yml was written to the redirected config home AND that re-parsing it
// through the production config loader reproduces the saved tabs' commands.
TEST_CASE("TerminalSessionManager: saveWindowLayout writes layouts.yml", "[manager][layout]")
{
    // Redirect config home to an isolated temp dir BEFORE constructing anything that reads
    // XDG_CONFIG_HOME, so the save never touches the real user config.
    QTemporaryDir configDir;
    REQUIRE(configDir.isValid());
    qputenv("XDG_CONFIG_HOME", configDir.path().toUtf8());

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController win { app.manager() };

    // Build a 2-leaf-tab layout and realize it into real mock-backed sessions.
    contour::config::Layout layout;
    contour::config::LayoutTab t0;
    t0.root.command = "echo a";
    contour::config::LayoutTab t1;
    t1.root.command = "echo b";
    layout.tabs = { t0, t1 };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(app.manager().saveWindowLayout(win.id, "saved"));

    auto const path = contour::config::configHome() / "layouts.yml";
    REQUIRE(std::filesystem::exists(path));

    // Sanity: the written file textually contains the saved layout's name.
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        CHECK(contents.str().find("saved:") != std::string::npos);
    }

    // Re-parse the written file through the production config loader to confirm it is valid,
    // round-trippable `layouts:` YAML, not just text that happens to contain "saved:".
    auto parsed = contour::config::Config {};
    contour::config::loadConfigFromFile(parsed, path);
    REQUIRE(parsed.layouts.value().contains("saved"));
    auto const& savedLayout = parsed.layouts.value().at("saved");
    REQUIRE(savedLayout.tabs.size() == 2);
    // Commands come from TerminalSession::launchedCommand(), which the mock PTY factory does set;
    // workingDirectory() may be empty for a BlockingMockPty-less MockPty session, so we don't assert
    // on directory here.
    REQUIRE(savedLayout.tabs[0].root.command.has_value());
    CHECK(*savedLayout.tabs[0].root.command == "echo a");
    REQUIRE(savedLayout.tabs[1].root.command.has_value());
    CHECK(*savedLayout.tabs[1].root.command == "echo b");

    qunsetenv("XDG_CONFIG_HOME");
}
