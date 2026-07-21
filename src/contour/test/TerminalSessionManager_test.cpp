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
    contour::test::ScopedController const win { app.manager() };

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
// and asserts BOTH that layouts.yml was written next to the loaded config file AND that re-parsing
// it through the production config loader reproduces the saved tabs' commands.
TEST_CASE("TerminalSessionManager: saveWindowLayout writes layouts.yml", "[manager][layout]")
{
    // The save writes NEXT TO the loaded config file (where loadConfigFromFile merges it back
    // from); pointing configFile into an isolated temp dir keeps the test off the real user
    // config without mutating process-global environment (XDG_CONFIG_HOME).
    QTemporaryDir const configDir;
    REQUIRE(configDir.isValid());

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdString()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    // Inject an inline layout (as if hand-written in contour.yml) BEFORE saving a DIFFERENT
    // layout. SaveLayout must not freeze this inline-only layout into layouts.yml: since
    // layouts.yml wins name collisions on load, writing it there would permanently shadow the
    // user's hand-written layout on every future load.
    contour::config::Layout inlineOnly;
    contour::config::LayoutTab inlineTab;
    inlineTab.root.command = "inline-only-shell";
    inlineOnly.tabs = { inlineTab };
    app.app().config().layouts.value()["inline_only"] = inlineOnly;

    // Build a 2-leaf-tab layout and realize it into real mock-backed sessions.
    contour::config::Layout layout;
    contour::config::LayoutTab t0;
    t0.root.command = "echo a";
    contour::config::LayoutTab t1;
    t1.root.command = "echo b";
    layout.tabs = { t0, t1 };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(app.manager().saveWindowLayout(win.id, "saved"));

    auto const path = std::filesystem::path(configDir.path().toStdString()) / "layouts.yml";
    REQUIRE(std::filesystem::exists(path));

    // Sanity: the written file textually contains the saved layout's name.
    {
        std::ifstream const in(path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        CHECK(contents.str().contains("saved:"));
    }

    // Re-parse the written file through the production config loader to confirm it is valid
    // `layouts:` YAML that survives a round trip, not just text that happens to contain "saved:".
    auto parsed = contour::config::Config {};
    contour::config::loadConfigFromFile(parsed, path);
    REQUIRE(parsed.layouts.value().contains("saved"));
    auto const& savedLayout = parsed.layouts.value().at("saved");
    REQUIRE(savedLayout.tabs.size() == 2);
    // Commands come from TerminalSession::launchedCommand(), which the mock PTY factory does set;
    // workingDirectory() may be empty for a plain MockPty session, so we don't assert
    // on directory here.
    REQUIRE(savedLayout.tabs[0].root.command.has_value());
    CHECK(*savedLayout.tabs[0].root.command == "echo a");
    REQUIRE(savedLayout.tabs[1].root.command.has_value());
    CHECK(*savedLayout.tabs[1].root.command == "echo b");

    // The inline-only layout must NOT have been frozen into layouts.yml: it never came from that
    // file, so it must not be written there even though it was present in the merged in-memory view.
    CHECK_FALSE(parsed.layouts.value().contains("inline_only"));

    // No pane carries an explicit profile override, so none may be pinned into the file: the
    // saved layout must keep following the user's default profile, not freeze today's default.
    CHECK_FALSE(savedLayout.tabs[0].root.profile.has_value());
    CHECK_FALSE(savedLayout.tabs[1].root.profile.has_value());
}

TEST_CASE("TerminalSessionManager: saveWindowLayout refuses to overwrite an unreadable layouts.yml",
          "[manager][layout]")
{
    // The save is a read-modify-write of layouts.yml. If the read fails to PARSE, treating the
    // file as empty and rewriting it would permanently destroy every layout it still contains —
    // refusing (and telling the user) is the only safe move.
    QTemporaryDir const configDir;
    REQUIRE(configDir.isValid());

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdString()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    auto const layoutsPath = std::filesystem::path(configDir.path().toStdString()) / "layouts.yml";
    auto const garbage = std::string { "layouts:\n  broken: [unterminated\n" };
    // Binary mode on BOTH sides: a text-mode write translates '\n' to "\r\n" on Windows, which the
    // binary read below would then report as a byte-for-byte difference the save never made.
    std::ofstream(layoutsPath, std::ios::binary) << garbage;

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo a";
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    CHECK_FALSE(app.manager().saveWindowLayout(win.id, "saved"));

    // The unreadable file is left byte-for-byte untouched for the user to fix or remove.
    std::ifstream const in(layoutsPath, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    CHECK(contents.str() == garbage);
}

TEST_CASE("TerminalSessionManager: SaveLayout persists through the injected LayoutStore", "[manager][layout]")
{
    // The manager owns no filesystem knowledge: it serializes the window and hands the result to
    // the injected store. Driving that seam with an in-memory store exercises SaveLayout end to end
    // (including the read-modify-write against what the store already holds) without any disk I/O.
    QTemporaryDir const configDir;
    REQUIRE(configDir.isValid());

    auto storeOwned = std::make_unique<contour::test::InMemoryLayoutStore>();
    auto* store = storeOwned.get();

    // The store already holds an unrelated layout: SaveLayout must PRESERVE it, not replace the
    // store's contents with just the new entry.
    contour::config::Layout existing;
    contour::config::LayoutTab existingTab;
    existingTab.root.command = "already-here";
    existing.tabs = { existingTab };
    store->layouts["existing"] = existing;

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), std::move(storeOwned) };
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdString()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo a";
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(app.manager().saveWindowLayout(win.id, "saved"));

    // Persisted next to the LOADED CONFIG FILE — the very path loadConfigFromFile merges back from,
    // so a custom `--config` can never strand saves somewhere the loader will not look.
    REQUIRE(store->savedPaths.size() == 1);
    CHECK(store->savedPaths.front() == std::filesystem::path(configDir.path().toStdString()) / "layouts.yml");

    REQUIRE(store->layouts.contains("saved"));
    REQUIRE(store->layouts.at("saved").tabs.size() == 1);
    CHECK(*store->layouts.at("saved").tabs[0].root.command == "echo a");
    CHECK(store->layouts.contains("existing")); // the pre-existing layout survived

    // The live config sees the save too, so a LaunchLayout later in this run finds it.
    CHECK(app.app().config().layouts.value().contains("saved"));
}

TEST_CASE("TerminalSessionManager: SaveLayout reports why a save failed", "[manager][layout]")
{
    auto storeOwned = std::make_unique<contour::test::InMemoryLayoutStore>();
    auto* store = storeOwned.get();
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), std::move(storeOwned) };
    contour::test::ScopedController const win { app.manager() };

    // Each failure is a distinct, reportable cause — not a bare "false".
    CHECK(app.manager().saveWindowLayout(win.id, "").error() == contour::LayoutSaveError::EmptyName);
    CHECK(app.manager().saveWindowLayout(vtmux::WindowId { 4711 }, "x").error()
          == contour::LayoutSaveError::UnknownWindow);

    store->loadError = "corrupt";
    CHECK(app.manager().saveWindowLayout(win.id, "x").error() == contour::LayoutSaveError::StoreUnreadable);
    CHECK(store->savedPaths.empty()); // an unreadable store is never written over

    store->loadError.reset();
    store->saveError = "disk full";
    CHECK(app.manager().saveWindowLayout(win.id, "x").error() == contour::LayoutSaveError::WriteFailed);
    // A failed write must NOT leave the in-memory config claiming a layout that was never saved.
    CHECK_FALSE(app.app().config().layouts.value().contains("x"));
}

TEST_CASE("TerminalSessionManager: a directory-only layout pane does not override the shell",
          "[manager][layout]")
{
    // A pane that only picks a directory still runs the profile's shell: it must NOT engage a
    // command override (an override with an empty program would tell the factory "this session
    // replaces the shell", which skips an SSH profile's SshSession and opens a LOCAL shell).
    // The directory travels through `cwd`, the same channel every other new tab/split uses.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.directory = std::filesystem::path { "/tmp" };
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(factory->requestedCommandOverrides.size() == 1);
    CHECK_FALSE(factory->requestedCommandOverrides[0].has_value()); // shell NOT overridden
    REQUIRE(factory->requestedCwds.size() == 1);
    REQUIRE(factory->requestedCwds[0].has_value());
    CHECK(*factory->requestedCwds[0] == "/tmp"); // ...but the directory still applies
}

TEST_CASE("TerminalSessionManager: consumeDefaultLayout applies the startup layout exactly once",
          "[manager][layout]")
{
    // main.qml calls consumeDefaultLayout for EVERY window it loads; only the process's first
    // window may consume the startup layout, or NewTerminalWindow would re-run all its commands.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo startup";
    layout.tabs = { tab };
    app.app().config().layouts.value()["auto"] = layout;
    app.app().config().defaultLayoutName.value() = "auto";

    contour::test::ScopedController const first { app.manager() };
    CHECK(app.manager().consumeDefaultLayout(first.controller));
    auto* firstWindow = app.manager().model().window(first.id);
    REQUIRE(firstWindow != nullptr);
    CHECK(firstWindow->tabCount() == 1);

    // A later window must NOT re-apply the layout (it gets its usual single default tab instead).
    contour::test::ScopedController const second { app.manager() };
    CHECK_FALSE(app.manager().consumeDefaultLayout(second.controller));
    auto* secondWindow = app.manager().model().window(second.id);
    REQUIRE(secondWindow != nullptr);
    CHECK(secondWindow->tabCount() == 0);
}

TEST_CASE("TerminalSessionManager: launchLayout births panes at the live window page size",
          "[manager][layout]")
{
    // LaunchLayout appends tabs to a LIVE window: their grids/PTYs must be born at its running
    // page size (like CreateNewTab/splits), not at the profile's configured default — or layout
    // commands that read the terminal size at startup would see 80x25 in a maximized window.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    auto const liveSize = acting->terminal().totalPageSize();

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo a";
    layout.tabs = { tab };
    app.app().config().layouts.value()["work"] = layout;

    auto const callsBefore = factory->requestedPageSizes.size();
    app.manager().launchLayout("work", acting);

    REQUIRE(factory->requestedPageSizes.size() == callsBefore + 1);
    REQUIRE(factory->requestedPageSizes.back().has_value());
    CHECK(*factory->requestedPageSizes.back() == liveSize);
}

TEST_CASE("TerminalSessionManager: createSession launches under the named profile (new-tab dropdown)",
          "[manager][profile]")
{
    // The new-tab profile dropdown threads a profile name through createNewTab -> createSession ->
    // createSessionInBackground -> createBackingSession. Without a name the session follows the app
    // default (no override); with one it records that profile as its explicit override.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    // A second profile to launch (cloned from the default so it resolves against the config).
    auto& profiles = app.app().config().profiles.value();
    profiles["work"] = profiles.at(app.app().profileName());

    auto* def = app.manager().createSession(win.id);
    REQUIRE(def != nullptr);
    CHECK_FALSE(def->profileOverride().has_value()); // no argument -> follows the default profile

    auto* work = app.manager().createSession(win.id, "work");
    REQUIRE(work != nullptr);
    REQUIRE(work->profileOverride().has_value());
    CHECK(*work->profileOverride() == "work");
}

// ============================================================================================
// Command palette: the full round trip a user actually performs — press Ctrl+Shift+P, pick a command,
// see it run, and find it under "recently used" on the next start.
// ============================================================================================

TEST_CASE("TerminalSessionManager: running a palette command records and persists it", "[manager][palette]")
{
    // The DI seam again: the manager owns no filesystem knowledge, it hands the MRU to the injected
    // store. Driving that with an in-memory store exercises record -> persist end to end, with no disk.
    QTemporaryDir const configDir;
    REQUIRE(configDir.isValid());

    auto historyOwned = std::make_unique<contour::test::InMemoryCommandHistoryStore>();
    auto* history = historyOwned.get();

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), nullptr, std::move(historyOwned) };
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdString()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    app.manager().recordCommand("SplitVertical");
    app.manager().recordCommand("CreateNewTab");

    // Persisted next to the LOADED CONFIG FILE, exactly like layouts.yml — so a custom `--config` moves
    // the history with it instead of stranding it in the default home.
    REQUIRE_FALSE(history->savedPaths.empty());
    CHECK(history->savedPaths.back()
          == std::filesystem::path(configDir.path().toStdString()) / "command-history.yml");

    // Newest first.
    CHECK(history->ids == std::vector<std::string> { "CreateNewTab", "SplitVertical" });

    // And the live history agrees, so a palette opened right now already shows them.
    auto const recent = app.manager().commandHistory().recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0] == "CreateNewTab");
}

TEST_CASE("TerminalSessionManager: the recent-command list survives a restart", "[manager][palette]")
{
    // The whole point of persisting it. A store seeded as if by a previous run must surface in the
    // palette of a fresh one.
    auto historyOwned = std::make_unique<contour::test::InMemoryCommandHistoryStore>();
    auto* history = historyOwned.get();
    history->ids = { "TogglePaneZoom", "SplitVertical" }; // what "last time" left behind

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), nullptr, std::move(historyOwned) };
    contour::test::ScopedController const win { app.manager() };

    // The history is loaded lazily, on the first palette open — the configured capacity is not known at
    // construction time, and seeding early would truncate a longer stored list to the DEFAULT capacity.
    app.manager().openCommandPalette(nullptr); // no acting session: exercises the guard, loads nothing
    CHECK(app.manager().commandHistory().recent().empty());

    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    app.manager().openCommandPalette(session);

    auto const recent = app.manager().commandHistory().recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0] == "TogglePaneZoom");
    CHECK(recent[1] == "SplitVertical");
}

TEST_CASE("TerminalSessionManager: an unreadable command history leaves the palette usable",
          "[manager][palette]")
{
    // A corrupt file must degrade to "no recent commands", never to a failed open: the MRU is a
    // convenience, and losing it is not worth refusing to show the palette.
    auto historyOwned = std::make_unique<contour::test::InMemoryCommandHistoryStore>();
    auto* history = historyOwned.get();
    history->loadError = "command-history.yml is not valid YAML";

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), nullptr, std::move(historyOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    app.manager().openCommandPalette(session);

    CHECK(app.manager().commandHistory().recent().empty());

    // And it still records going forward, rather than staying wedged on the read failure.
    app.manager().recordCommand("SplitVertical");
    CHECK(app.manager().commandHistory().recent().size() == 1);
}

TEST_CASE("TerminalSessionManager: recent_count bounds what is remembered", "[manager][palette]")
{
    auto historyOwned = std::make_unique<contour::test::InMemoryCommandHistoryStore>();
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned), nullptr, std::move(historyOwned) };
    contour::test::ScopedController const win { app.manager() };

    app.app().config().commandPaletteRecentCount.value() = 2;

    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    app.manager().openCommandPalette(session); // applies the configured capacity

    app.manager().recordCommand("A");
    app.manager().recordCommand("B");
    app.manager().recordCommand("C");

    auto const recent = app.manager().commandHistory().recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0] == "C");
    CHECK(recent[1] == "B");
}
