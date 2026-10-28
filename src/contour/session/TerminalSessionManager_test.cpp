// SPDX-License-Identifier: Apache-2.0
//
// Model-level coverage for the "how many OS windows exist" decision that TerminalSessionManager's
// teardown relies on. The manager itself is not headless-constructible (it needs ContourGuiApp / PTY /
// QQuickWindow), and the per-display session bookkeeping it used to carry (DisplayState /
// detachSessionFromState / isLastActiveDisplay) is gone: session->display ownership now lives on the
// pane tree, and window identity is the manager's WindowController registry ("last window" ==
// controllers.size() == 1). The identity that decision derives from is the vtworkspace::SessionModel's window
// set, so the testable invariant — window count as windows are created and removed — is exercised here
// against the Qt-free model. (Per-window tab/pane isolation is covered in SessionModel_test.cpp.)

#include <contour/config/Config.hpp>
#include <contour/config/SecureInputMode.hpp>
#include <contour/platform/FreeDesktopNotifier.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/SecureInputFixtures.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/SecureInputPolicy.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/test/NonAsciiSample.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/FileUri.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPalette>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using namespace vtworkspace;

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
    // The manager terminates a closing window's sessions and removes its vtworkspace::Window; a sibling
    // window's tabs/sessions must be untouched. This is the model half of the per-window teardown invariant.
    SilentEvents events;
    uint64_t nextSession = 1;
    SessionModel model { events, [&] { return SessionId { nextSession++ }; } };

    auto* a = model.createWindow();
    auto* b = model.createWindow();
    (void) model.createTab(a->id());
    (void) model.createTab(a->id());
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
// contour::Layout by hand and asserts the resulting real, PTY-backed tabs/colors on the
// manager's authoritative vtworkspace::SessionModel.
TEST_CASE("TerminalSessionManager: applyLayoutToWindow builds tabs with colors", "[manager][layout]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto const profileColor = vtbackend::RGBColor { "#2E3440" };
    app.app().config().profile().tabColor.value() = profileColor;
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
    t2.root.orientation = vtworkspace::SplitState::Vertical;
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
    CHECK(window->tabAt(0)->color() == profileColor);
    auto* colored = window->tabAt(1);
    REQUIRE(colored != nullptr);
    CHECK(colored->color(vtworkspace::TabColorSource::User).has_value()); // second tab got its color
    CHECK(colored->color() == t1.color);
    CHECK(colored->color(vtworkspace::TabColorSource::Profile) == profileColor);
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

// B2: a daemon LayoutState, once converted by vthost::client::wireToLayout,
// realizes through the SAME applyLayoutToWindow path the config-layout feature
// uses — proving the daemon's tab/split tree rebuilds in the GUI's model. (The
// remote-session binding of each realized pane is the AF_UNIX-fixture layer; here
// the MockPty factory backs the panes so the STRUCTURE is verified on Windows.)
TEST_CASE("TerminalSessionManager: applyLayoutToWindow realizes a daemon wire layout", "[manager][layout]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    // Tab 0: a single pane. Tab 1: a vertical split (60/40) of two panes.
    auto state = vthost::proto::LayoutState {};
    state.tabs.push_back(
        vthost::proto::WireTab { .root = vthost::proto::WirePane { .split = 0, .session = 1 } });
    state.tabs.push_back(vthost::proto::WireTab {
        .root = vthost::proto::WirePane {
            .split = 2,
            .ratio = 6000,
            .children = { vthost::proto::WirePane { .split = 0, .session = 2 },
                          vthost::proto::WirePane { .split = 0, .session = 3 } } } });

    auto const wl = vthost::client::wireToLayout(state);

    // The beforeLeafSeed hook fires once per leaf, right before its backing session
    // is created — resolving each leaf to its remote session exactly as the attach
    // binding will (leafSession keyed by the realized leaf's address).
    auto boundSessions = std::vector<uint64_t> {};
    auto const seedHook = [&](contour::config::LayoutPane const& leaf) {
        boundSessions.push_back(wl.leafSession.at(&leaf));
    };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, wl.layout, std::nullopt, seedHook));

    // Every leaf (across both tabs) was bound to its remote session.
    std::ranges::sort(boundSessions);
    CHECK(boundSessions == std::vector<uint64_t> { 1, 2, 3 });

    auto* window = app.manager().model().window(win.id);
    REQUIRE(window != nullptr);
    CHECK(window->tabCount() == 2);

    auto* single = window->tabAt(0);
    REQUIRE(single != nullptr);
    CHECK(single->paneCount() == 1);

    auto* splitTab = window->tabAt(1);
    REQUIRE(splitTab != nullptr);
    CHECK(splitTab->paneCount() == 2);
    REQUIRE_FALSE(splitTab->rootPane()->isLeaf());
    CHECK(splitTab->rootPane()->splitState() == vtworkspace::SplitState::Vertical);
    CHECK(std::lround(splitTab->rootPane()->ratio() * 10000.0) == 6000); // first child's 60% share
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
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdU16String()) / "contour.yml";
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

    REQUIRE(app.manager().saveWindowLayout(win.id, "saved", contour::window::LayoutReplace::Replace));

    auto const path = std::filesystem::path(configDir.path().toStdU16String()) / "layouts.yml";
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
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdU16String()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    auto const layoutsPath = std::filesystem::path(configDir.path().toStdU16String()) / "layouts.yml";
    auto const garbage = std::string { "layouts:\n  broken: [unterminated\n" };
    // Binary mode on BOTH sides: a text-mode write translates '\n' to "\r\n" on Windows, which the
    // binary read below would then report as a byte-for-byte difference the save never made.
    std::ofstream(layoutsPath, std::ios::binary) << garbage;

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo a";
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    CHECK_FALSE(app.manager().saveWindowLayout(win.id, "saved", contour::window::LayoutReplace::Replace));

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
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdU16String()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.command = "echo a";
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(app.manager().saveWindowLayout(win.id, "saved", contour::window::LayoutReplace::Replace));

    // Persisted next to the LOADED CONFIG FILE — the very path loadConfigFromFile merges back from,
    // so a custom `--config` can never strand saves somewhere the loader will not look.
    REQUIRE(store->savedPaths.size() == 1);
    CHECK(store->savedPaths.front()
          == std::filesystem::path(configDir.path().toStdU16String()) / "layouts.yml");

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
    CHECK(app.manager().saveWindowLayout(win.id, "", contour::window::LayoutReplace::Replace).error()
          == contour::config::LayoutSaveError::EmptyName);
    CHECK(app.manager()
              .saveWindowLayout(vtworkspace::WindowId { 4711 }, "x", contour::window::LayoutReplace::Replace)
              .error()
          == contour::config::LayoutSaveError::UnknownWindow);

    store->loadError = "corrupt";
    CHECK(app.manager().saveWindowLayout(win.id, "x", contour::window::LayoutReplace::Replace).error()
          == contour::config::LayoutSaveError::StoreUnreadable);
    CHECK(store->savedPaths.empty()); // an unreadable store is never written over

    store->loadError.reset();
    store->saveError = "disk full";
    CHECK(app.manager().saveWindowLayout(win.id, "x", contour::window::LayoutReplace::Replace).error()
          == contour::config::LayoutSaveError::WriteFailed);
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
    tab.root.directory = "/tmp";
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(factory->requestedCommandOverrides.size() == 1);
    CHECK_FALSE(factory->requestedCommandOverrides[0].has_value()); // shell NOT overridden
    REQUIRE(factory->requestedCwds.size() == 1);
    REQUIRE(factory->requestedCwds[0].has_value());
    CHECK(*factory->requestedCwds[0] == "/tmp"); // ...but the directory still applies
}

TEST_CASE("TerminalSessionManager: a layout pane's directory beyond ASCII reaches its session in UTF-8",
          "[manager][layout]")
{
    // Every new session reads its cwd as UTF-8. Narrowed with path::string(), Windows spelled the directory
    // in the ANSI code page, or threw for one beyond it.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    contour::config::Layout layout;
    contour::config::LayoutTab tab;
    tab.root.directory = std::filesystem::path { u8"/tmp/" } / vtpty::testing::NonAsciiName;
    layout.tabs = { tab };
    REQUIRE(app.manager().applyLayoutToWindow(win.id, layout));

    REQUIRE(factory->requestedCwds.size() == 1);
    CHECK(factory->requestedCwds[0] == std::format("/tmp/{}", vtpty::testing::NonAsciiNameUtf8));
}

TEST_CASE("TerminalSessionManager: consumeDefaultLayout applies the startup layout exactly once",
          "[manager][layout]")
{
    // Main.qml calls consumeDefaultLayout for EVERY window it loads; only the process's first
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
    app.app().config().configFile = std::filesystem::path(configDir.path().toStdU16String()) / "contour.yml";
    contour::test::ScopedController const win { app.manager() };

    app.manager().recordCommand("SplitVertical");
    app.manager().recordCommand("CreateNewTab");

    // Persisted next to the LOADED CONFIG FILE, exactly like layouts.yml — so a custom `--config` moves
    // the history with it instead of stranding it in the default home.
    REQUIRE_FALSE(history->savedPaths.empty());
    CHECK(history->savedPaths.back()
          == std::filesystem::path(configDir.path().toStdU16String()) / "command-history.yml");

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

// ============================================================================================
// A pane running a given command (OpenCommandOutput's pager): a split beside the acting pane, or a tab.
// ============================================================================================

TEST_CASE("TerminalSessionManager: openCommandPane runs a command beside the acting pane or in a new tab",
          "[manager][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    REQUIRE(win->count() == 1);

    auto pager = vtpty::Process::ExecInfo {};
    pager.program = "less";
    pager.arguments = { "-R", "/run/out.txt" };

    SECTION("a split joins the acting pane's tab, stacked below it")
    {
        auto const pane =
            app.manager().openCommandPane(acting, contour::session::CommandPanePlacement::Split, pager);
        REQUIRE(pane.has_value());
        CHECK(*pane != acting);
        CHECK(win->count() == 1);

        // Output reads top to bottom, so the pager keeps the acting pane's full width.
        auto const* const root = app.manager().model().window(win.id)->activeTab()->rootPane();
        CHECK(root->splitState() == SplitState::Horizontal);
        REQUIRE(root->second() != nullptr);
        CHECK(root->second()->session() == (*pane)->modelSessionId());
    }

    SECTION("a tab is a tab of its own")
    {
        auto const pane =
            app.manager().openCommandPane(acting, contour::session::CommandPanePlacement::Tab, pager);
        REQUIRE(pane.has_value());
        CHECK(*pane != acting);
        CHECK(win->count() == 2);
    }

    REQUIRE(factory->requestedCommandOverrides.back().has_value());
    CHECK(factory->requestedCommandOverrides.back()->program == "less");
    CHECK(factory->requestedCommandOverrides.back()->arguments
          == std::vector<std::string> { "-R", "/run/out.txt" });
}

TEST_CASE("TerminalSessionManager: openCommandPane needs a tab to act from", "[manager][semanticblocks]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const win { app.manager() };

    auto pager = vtpty::Process::ExecInfo {};
    pager.program = "less";

    for (auto const placement:
         { contour::session::CommandPanePlacement::Split, contour::session::CommandPanePlacement::Tab })
    {
        auto const pane = app.manager().openCommandPane(nullptr, placement, pager);
        REQUIRE_FALSE(pane.has_value());
        CHECK(pane.error() == contour::session::CommandPaneError::NoPane);
    }
    CHECK(win->count() == 0);
}

TEST_CASE("TerminalSessionManager: closing a pane frees its session", "[manager][session][close]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const survivor = window->activeSession();
    REQUIRE(survivor != nullptr);
    manager.splitActivePane(/*vertical*/ true, survivor);
    auto* const closing = window->activeSession();
    REQUIRE(closing != survivor);
    auto const watched = QPointer<contour::session::TerminalSession> { closing };

    // Closed as a user closes a pane: the pane leaves the tab, and the session ends once its exit watcher
    // sees the pty closed -- called here in the watcher's stead.
    manager.closeActivePane(closing, contour::session::SessionEnd::Destroy);
    closing->onClosed();

    // Not under the call that ended it, which still runs on after the manager lets the session go; and a
    // call the session queued for itself, as its threads do, is still waiting.
    REQUIRE(watched);
    auto landed = false;
    contour::platform::postToObject(closing, [&landed]() { landed = true; });

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(watched.isNull());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    CHECK_FALSE(landed);
    CHECK(window->activeSession() == survivor);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: the sessions a manager made do not outlive it",
          "[manager][session][close]")
{
    auto watched = QPointer<contour::session::TerminalSession> {};
    auto landed = false;
    {
        contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
        contour::test::ScopedController const window { app.manager() };
        window->createNewTab();
        watched = window->activeSession();
        REQUIRE(watched);

        // Left open, with a call queued for itself, as an app that quits leaves one.
        contour::platform::postToObject(watched.data(), [&landed]() { landed = true; });
    }
    CHECK(watched.isNull());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    CHECK_FALSE(landed);
}

// {{{ Finish notifications: where a session is, what its tab is called, and bringing it forward

TEST_CASE("TerminalSessionManager: a session's visibility follows focus, tabs, zoom and minimising",
          "[manager][notification]")
{
    using contour::platform::SessionVisibility;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedControllerWithOsWindow const window { manager };

    window->createNewTab();
    auto* const first = window->activeSession();
    REQUIRE(first != nullptr);
    manager.splitActivePane(/*vertical*/ true, first);
    auto* const second = window->activeSession();
    REQUIRE(second != nullptr);
    REQUIRE(first != second);

    SECTION("the focused pane is Focused, and its on-screen sibling VisibleUnfocused")
    {
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::VisibleUnfocused);
    }

    SECTION("a window that loses focus leaves its panes on screen")
    {
        manager.clearFocusedWindow(window.id);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::VisibleUnfocused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::VisibleUnfocused);
    }

    SECTION("a zoomed pane covers its sibling")
    {
        manager.toggleActivePaneZoom(second);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
    }

    SECTION("another tab hides both panes")
    {
        window->createNewTab();
        auto* const third = window->activeSession();
        REQUIRE(third != nullptr);
        CHECK(manager.sessionVisibility(*third) == SessionVisibility::Focused);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Hidden);
    }

    SECTION("the Settings window covers no pane")
    {
        // Settings is a window of its own, so opening it takes neither pane off screen. Which one holds
        // focus is the OS window's activation to decide, which this unbound window never sees.
        app.app().openSettings(nullptr);
        CHECK(manager.sessionVisibility(*second) != SessionVisibility::Hidden);
        CHECK(manager.sessionVisibility(*first) != SessionVisibility::Hidden);
    }

    SECTION("a minimised window hides even its focused pane")
    {
        window->bindWindow(window.osWindow.get());
        window.osWindow->setWindowStates(Qt::WindowMinimized);
        CHECK(manager.sessionVisibility(*second) == SessionVisibility::Hidden);
        CHECK(manager.sessionVisibility(*first) == SessionVisibility::Hidden);
    }

    SECTION("a session no tab hosts is hidden unless it holds focus")
    {
        auto const standalone =
            contour::test::makeTestSession(&manager,
                                           std::make_unique<vtpty::MockPty>(vtpty::PageSize {
                                               vtpty::LineCount(24), vtpty::ColumnCount(80) }),
                                           app.app(),
                                           std::string {});
        CHECK(manager.sessionVisibility(*standalone) == SessionVisibility::Hidden);
        manager.setFocusedSession(standalone.get());
        CHECK(manager.sessionVisibility(*standalone) == SessionVisibility::Focused);
        manager.setFocusedSession(nullptr);
    }

    contour::test::closeAllTabs(*window);
}

// {{{ The tab badge slot: unseen output, finished commands and OSC 7501, and the host chip

namespace
{

/// The badge tab @p row of @p window shows, as the strip reads it.
[[nodiscard]] contour::window::TabBadgeKind badgeAt(contour::window::WindowController& window, int row)
{
    return static_cast<contour::window::TabBadgeKind>(
        window.data(window.index(row), static_cast<int>(contour::window::WindowController::Roles::BadgeRole))
            .toInt());
}

/// Runs every queued call, as the GUI thread's next turn would: what a session posts from its parser.
void drainPosted()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

/// The session behind tab @p row's active pane.
[[nodiscard]] contour::session::TerminalSession* sessionAtRow(
    contour::session::TerminalSessionManager& manager, contour::window::WindowController& window, int row)
{
    auto const* tab = manager.model().window(window.windowId())->tabAt(row);
    return tab != nullptr ? manager.sessionForId(tab->activePane()->session()) : nullptr;
}

/// What a program waiting for the user reports (OSC 7501).
constexpr auto BlockedStatus = std::string_view { "\033]7501;state=blocked:msg=QXBwcm92ZT8\033\\" };

} // namespace

TEST_CASE("TerminalSessionManager: a background tab's badge follows its output, its commands and OSC 7501",
          "[manager][tabbadge]")
{
    using contour::window::TabBadgeKind;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedControllerWithOsWindow const window { manager };
    window->bindWindow(window.osWindow.get());

    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->activeTabIndex() == 1);
    auto* const background = sessionAtRow(manager, *window, 0);
    auto* const active = sessionAtRow(manager, *window, 1);
    REQUIRE(background != nullptr);
    REQUIRE(active != nullptr);
    drainPosted();
    REQUIRE(badgeAt(*window, 0) == TabBadgeKind::None);

    SECTION("output in a background tab shows the dot, and seeing it clears it")
    {
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);

        window->activateTab(0);
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);
    }

    SECTION("output in the active tab shows nothing")
    {
        active->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 1) == TabBadgeKind::None);
    }

    SECTION("a command that fails in a background tab shows its cross until seen")
    {
        background->terminal().writeToScreen("\033]133;C\007");
        background->terminal().writeToScreen("\033]133;D;1\007");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Error);

        window->activateTab(0);
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);
    }

    SECTION("a command that succeeds in a background tab shows its tick")
    {
        background->terminal().writeToScreen("\033]133;C\007");
        background->terminal().writeToScreen("\033]133;D;0\007");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Done);
    }

    SECTION("Blocked outranks output, and its text is the tooltip")
    {
        background->terminal().writeToScreen(BlockedStatus);
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Blocked);
        CHECK(window
                  ->data(window->index(0),
                         static_cast<int>(contour::window::WindowController::Roles::BadgeTextRole))
                  .toString()
              == QStringLiteral("Approve?"));
    }

    SECTION("refreshTabBadgeForSession repaints the program status and marks no output of its own")
    {
        background->terminal().writeToScreen("\033]7501;state=working:app=cargo\033\\");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Working);

        // Seen, then sent to the back again: the report's bytes, which were a write like any other, are
        // forgotten.
        window->activateTab(0);
        window->activateTab(1);
        REQUIRE(manager.tabAttention().of(background->modelSessionId())
                == contour::window::SessionAttention {});

        // The program-status path alone, as Step 2's pane properties call it.
        manager.refreshTabBadgeForSession(background->modelSessionId());
        CHECK(manager.tabAttention().of(background->modelSessionId())
              == contour::window::SessionAttention {});
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Working);
    }

    SECTION("a minimised window's active tab is not seen")
    {
        window.osWindow->setWindowStates(Qt::WindowMinimized);
        active->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 1) == TabBadgeKind::Output);

        window.osWindow->setWindowStates(Qt::WindowNoState);
        CHECK(badgeAt(*window, 1) == TabBadgeKind::None);
    }

    SECTION("unzooming clears a covered sibling's dot")
    {
        manager.splitActivePane(/*vertical*/ true, active);
        auto* const sibling = window->activeSession();
        REQUIRE(sibling != nullptr);
        REQUIRE(sibling != active);
        manager.toggleActivePaneZoom(sibling);

        active->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 1) == TabBadgeKind::Output);

        manager.toggleActivePaneZoom(sibling);
        CHECK(badgeAt(*window, 1) == TabBadgeKind::None);
    }

    SECTION("swapping a covered pane in a zoomed tab clears its dot")
    {
        manager.splitActivePane(/*vertical*/ true, active);
        auto* const sibling = window->activeSession();
        REQUIRE(sibling != nullptr);
        manager.toggleActivePaneZoom(sibling);

        active->terminal().writeToScreen("x");
        drainPosted();
        REQUIRE(badgeAt(*window, 1) == TabBadgeKind::Output);

        // The swap puts `active` in front (vtworkspace::Tab ends the zoom so both swapped panes show), and
        // whichever seam reports it -- the swap or the zoom change -- marks it seen.
        manager.swapPane(vtworkspace::FocusDirection::Left, sibling);
        CHECK(badgeAt(*window, 1) == TabBadgeKind::None);
    }

    SECTION("a finish posted after its session was removed marks nothing")
    {
        auto const id = background->modelSessionId();
        manager.removeSession(*background);
        // What a queued handlePendingBlockEvents() delivers between the removal and the deletion.
        manager.noteCommandFinished(*background, vtbackend::CommandEnding::Failed);
        CHECK(manager.tabAttention().of(id) == contour::window::SessionAttention {});
    }

    SECTION("a closed session's marks go with it")
    {
        background->terminal().writeToScreen("x");
        drainPosted();
        auto const id = background->modelSessionId();
        REQUIRE(manager.tabAttention().of(id).activity == contour::window::TabActivity::UnseenOutput);
        // Mock PTYs are not started: drive the sessionClosed endpoint rather than waiting for an exit
        // watcher.
        manager.removeSession(*background);
        CHECK(manager.tabAttention().of(id) == contour::window::SessionAttention {});
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: output written from the parser's thread reaches the badge once posted",
          "[manager][tabbadge][threading]")
{
    using contour::window::TabBadgeKind;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 0);
    REQUIRE(background != nullptr);
    drainPosted();

    // A stream of output, as the parser applies it chunk by chunk, off the GUI thread.
    auto writer = std::thread { [background] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 200))
            background->terminal().writeToScreen("chunk\r\n");
    } };
    writer.join();
    CHECK(badgeAt(*window, 0) == TabBadgeKind::None); // nothing lands before the GUI thread's turn

    drainPosted();
    CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a resync marks no unseen output", "[manager][tabbadge]")
{
    using contour::window::TabBadgeKind;
    using vthost::client::ConnectionState;

    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    contour::test::TestApp app { std::move(factory) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 0);
    REQUIRE(background != nullptr);
    drainPosted();

    SECTION("the mirror's repaint around a reconnect is not news")
    {
        manager.setHostConnection(ConnectionState::Reconnecting);
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

        manager.setHostConnection(ConnectionState::Connected);
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

        // Once the grace is over, output is news again.
        app.steadyClock().advance(contour::window::ResyncGracePeriod);
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);
    }

    SECTION("a Blocked status set before the drop still reads Blocked after it")
    {
        background->terminal().writeToScreen(BlockedStatus);
        drainPosted();
        REQUIRE(badgeAt(*window, 0) == TabBadgeKind::Blocked);

        manager.setHostConnection(ConnectionState::Reconnecting);
        manager.setHostConnection(ConnectionState::Connected);
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Blocked);
    }

    manager.setHostConnection(ConnectionState::Connected);
    contour::test::closeAllTabs(*window);
}

namespace
{

/// A wire row of the session terminal's width at @p stableId carrying @p text.
[[nodiscard]] vthost::proto::WireLine wireRow(int64_t stableId, std::string_view text)
{
    auto line = vthost::proto::WireLine {};
    line.stableId = stableId;
    line.columns = unbox<uint32_t>(contour::test::MockPtySessionFactory::DefaultPageSize.columns);
    for (auto const ch: text)
    {
        auto cell = vthost::proto::WireCell {};
        cell.codepoint = static_cast<char32_t>(ch);
        line.cells.push_back(cell);
    }
    return line;
}

/// A delta of @p row alone on the page's top row; a snapshot when @p snapshot says so.
[[nodiscard]] vthost::proto::Delta deltaOf(std::string_view row, bool snapshot)
{
    auto delta = vthost::proto::Delta {};
    delta.snapshot = snapshot ? 1 : 0;
    delta.stableViewportBase = 1;
    delta.stableFloor = 1;
    delta.lines = { wireRow(1, row) };
    return delta;
}

} // namespace

TEST_CASE("TerminalSessionManager: a remote pane's replays are not news", "[manager][tabbadge]")
{
    using contour::window::TabBadgeKind;

    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    contour::test::TestApp app { std::move(factory) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 0);
    REQUIRE(background != nullptr);
    drainPosted();

    SECTION("a remote session is born inside the resync grace")
    {
        background->terminal().writeToScreen("x");
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);
    }

    SECTION("the mirror classifies its replays, so the grace is only a fallback")
    {
        // Past the grace: what follows is told apart at its source alone.
        app.steadyClock().advance(contour::window::ResyncGracePeriod);
        auto screen = vthost::client::RemoteScreen {};
        screen.columns = unbox<int>(contour::test::MockPtySessionFactory::DefaultPageSize.columns);
        screen.lines = unbox<int>(contour::test::MockPtySessionFactory::DefaultPageSize.lines);
        auto mirror = vthost::client::ScreenMirror { background->terminal(), vthost::DaemonMachine::Same };

        // A fresh pane's first replay: the attach.
        auto const first = deltaOf("prompt", /*snapshot*/ true);
        screen.apply(first);
        mirror.apply(screen, first);
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

        // A server resync while connected: a snapshot delta, as a resize or ResyncRequired sends.
        auto const resync = deltaOf("prompt", /*snapshot*/ true);
        screen.apply(resync);
        mirror.apply(screen, resync);
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

        // An incremental delta is the program writing.
        auto const tick = deltaOf("output", /*snapshot*/ false);
        screen.apply(tick);
        mirror.apply(screen, tick);
        drainPosted();
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);
    }

    SECTION("a program entering the alternate screen is news, though a snapshot carries it")
    {
        app.steadyClock().advance(contour::window::ResyncGracePeriod);
        auto screen = vthost::client::RemoteScreen {};
        screen.columns = unbox<int>(contour::test::MockPtySessionFactory::DefaultPageSize.columns);
        screen.lines = unbox<int>(contour::test::MockPtySessionFactory::DefaultPageSize.lines);
        auto mirror = vthost::client::ScreenMirror { background->terminal(), vthost::DaemonMachine::Same };

        // The first replay finds the session on the alternate screen already: the attach, not news.
        screen.screenType = 1;
        auto const first = deltaOf("vim", /*snapshot*/ true);
        screen.apply(first);
        mirror.apply(screen, first);
        drainPosted();
        REQUIRE(background->terminal().isAlternateScreen());
        CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

        // The program leaves it: the page flip the host forces a snapshot for.
        screen.screenType = 0;
        auto const flip = deltaOf("prompt", /*snapshot*/ true);
        screen.apply(flip);
        mirror.apply(screen, flip);
        drainPosted();
        REQUIRE_FALSE(background->terminal().isAlternateScreen());
        CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a tmux pane's refill is not news", "[manager][tabbadge]")
{
    using contour::window::TabBadgeKind;

    auto channels = std::vector<vtpty::ChannelPty*> {};
    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    factory->makePty = [&channels](vtbackend::PageSize size) {
        auto pty = std::make_unique<vtpty::ChannelPty>(size);
        channels.push_back(pty.get());
        return pty;
    };
    contour::test::TestApp app { std::move(factory) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 0);
    REQUIRE(background != nullptr);
    REQUIRE(channels.size() == 2);
    drainPosted();
    // Past the grace: the refill is told apart at its source alone.
    app.steadyClock().advance(contour::window::ResyncGracePeriod);

    // capture-pane's replay, as the tmux pane feed hands it over, parsed as the session's parser would.
    channels[0]->feedReplay("history-line\r\n");
    REQUIRE(background->terminal().processInputOnce());
    drainPosted();
    CHECK(badgeAt(*window, 0) == TabBadgeKind::None);

    channels[0]->feed("live-line\r\n");
    REQUIRE(background->terminal().processInputOnce());
    drainPosted();
    CHECK(badgeAt(*window, 0) == TabBadgeKind::Output);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: the host chip follows the connection", "[manager][hostchip]")
{
    using contour::window::HostChipState;
    using vthost::client::ConnectionState;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    CHECK(manager.hostConnection() == ConnectionState::Connected);
    CHECK(window->hostChipState() == static_cast<int>(HostChipState::Connected));

    auto spy = QSignalSpy { &*window, &contour::window::WindowController::hostChipStateChanged };

    manager.setHostConnection(ConnectionState::Reconnecting);
    CHECK(window->hostChipState() == static_cast<int>(HostChipState::Reconnecting));
    CHECK(spy.count() == 1);

    // Connecting folds onto the same chip state, so the chip is not told twice.
    manager.setHostConnection(ConnectionState::Connecting);
    CHECK(spy.count() == 1);

    manager.setHostConnection(ConnectionState::Refused);
    CHECK(window->hostChipState() == static_cast<int>(HostChipState::Disconnected));
    CHECK(spy.count() == 2);

    manager.setHostConnection(ConnectionState::Connected);
    CHECK(window->hostChipState() == static_cast<int>(HostChipState::Connected));
    CHECK(spy.count() == 3);
}

// }}}

// {{{ Step 2: panes -- each pane's directory, program status and focus, and the window's visible panes

namespace
{

/// Reports @p url as the shell's working directory (OSC 7), as a shell's prompt does, and runs what the
/// session posted for it.
void reportWorkingDirectory(contour::session::TerminalSession& session, std::string_view url)
{
    session.terminal().writeToScreen(std::format("\033]7;{}\033\\", url));
    drainPosted();
}

/// The leaf proxy that shows @p session under @p node, or null.
[[nodiscard]] contour::session::PaneProxy* proxyShowing(contour::session::PaneProxy* node,
                                                        contour::session::TerminalSession const* session)
{
    if (node == nullptr)
        return nullptr;
    if (node->isLeaf())
        return node->session() == session ? node : nullptr;
    if (auto* found = proxyShowing(node->first(), session))
        return found;
    return proxyShowing(node->second(), session);
}

} // namespace

TEST_CASE("TerminalSessionManager: OSC 7 notifies the pane's working directory once", "[manager][panes]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    drainPosted();

    auto const changed =
        QSignalSpy { session, &contour::session::TerminalSession::displayWorkingDirectoryChanged };
    reportWorkingDirectory(*session, "file://localhost/tmp/x");
    CHECK(changed.count() == 1);
    CHECK(session->getDisplayWorkingDirectory() == QStringLiteral("/tmp/x"));

    // A prompt re-reporting the same directory is no news.
    reportWorkingDirectory(*session, "file://localhost/tmp/x");
    CHECK(changed.count() == 1);

    SECTION("OSC 3008 notifies too")
    {
        session->terminal().writeToScreen(
            "\033]3008;start=shell-1;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;cwd=/srv\033\\");
        drainPosted();
        CHECK(changed.count() == 2);
        CHECK(session->getDisplayWorkingDirectory() == QStringLiteral("/srv"));
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a pane's working directory is abbreviated and made safe to show",
          "[manager][panes]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    auto const& home = manager.homeDirectory();
    REQUIRE_FALSE(home.empty());
    auto const path = std::string { home.starts_with('/') ? "" : "/" } + home;

    SECTION("beneath the home")
    {
        reportWorkingDirectory(*session,
                               std::format("file://{}{}/repo", contour::test::TestApp::HostName, path));
        CHECK(session->getDisplayWorkingDirectory() == QStringLiteral("~/repo"));

        // The status line's {Cwd} reads the same home as the header above it, not the environment's.
        CHECK(session->terminal().homeDirectory() == home);
        CHECK(vtbackend::serializeToVT(session->terminal(),
                                       vtbackend::parseStatusLineSegment("{Cwd}"),
                                       vtbackend::StatusLineStyling::Disabled)
              == "~/repo");
    }

    SECTION("holding a bidi override")
    {
        // OSC 7 is percent-decoded: %E2%80%AE is U+202E RIGHT-TO-LEFT OVERRIDE.
        reportWorkingDirectory(*session, "file://localhost/tmp/a%E2%80%AEb");
        CHECK_FALSE(session->getDisplayWorkingDirectory().contains(QChar(0x202E)));
        CHECK(session->getDisplayWorkingDirectory().startsWith(QStringLiteral("/tmp/a")));
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a pane's program status is its own", "[manager][panes]")
{
    using contour::window::TabBadgeKind;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedControllerWithOsWindow const window { manager };
    window->bindWindow(window.osWindow.get());
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != left);
    drainPosted();

    auto const leftChanged =
        QSignalSpy { left, &contour::session::TerminalSession::paneProgramStatusChanged };
    auto const rightChanged =
        QSignalSpy { right, &contour::session::TerminalSession::paneProgramStatusChanged };

    left->terminal().writeToScreen("\033]7501;state=working:app=cargo\033\\");
    drainPosted();
    CHECK(left->getProgramStatus() == static_cast<int>(TabBadgeKind::Working));
    CHECK(left->getProgramStatusText() == QStringLiteral("cargo"));
    CHECK(right->getProgramStatus() == static_cast<int>(TabBadgeKind::None));
    CHECK(right->getProgramStatusText().isEmpty());
    CHECK(leftChanged.count() == 1);
    CHECK(rightChanged.count() == 0);
    // The tab still shows what any of its panes holds.
    CHECK(badgeAt(*window, 0) == TabBadgeKind::Working);

    left->terminal().writeToScreen("\033]7501;state=clear\033\\");
    drainPosted();
    CHECK(left->getProgramStatus() == static_cast<int>(TabBadgeKind::None));
    CHECK(leftChanged.count() == 2);
    CHECK(rightChanged.count() == 0);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a pane's focus says how it was reached", "[manager][panes]")
{
    using contour::window::PaneFocus;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != left);

    auto* const leftProxy = proxyShowing(window->activeTabRootPane(), left);
    auto* const rightProxy = proxyShowing(window->activeTabRootPane(), right);
    REQUIRE(leftProxy != nullptr);
    REQUIRE(rightProxy != nullptr);

    rightProxy->activate();
    CHECK(rightProxy->focus() == static_cast<int>(PaneFocus::Focused));
    CHECK(leftProxy->focus() == static_cast<int>(PaneFocus::Unfocused));

    manager.focusPane(vtworkspace::FocusDirection::Left, right);
    CHECK(leftProxy->focus() == static_cast<int>(PaneFocus::KeyboardFocused));
    CHECK(rightProxy->focus() == static_cast<int>(PaneFocus::Unfocused));

    // A click on the pane the keyboard reached moves nothing, yet its focus is the pointer's now, and QML
    // is told so.
    auto const leftActive = QSignalSpy { leftProxy, &contour::session::PaneProxy::activeChanged };
    leftProxy->activate();
    CHECK(leftProxy->focus() == static_cast<int>(PaneFocus::Focused));
    CHECK(leftActive.count() >= 1);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: only a FocusPane move leaves a pane keyboard-focused", "[manager][panes]")
{
    using contour::window::PaneFocus;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != left);
    auto const focusOf = [&](contour::session::TerminalSession const* session) {
        auto const* proxy = proxyShowing(window->activeTabRootPane(), session);
        REQUIRE(proxy != nullptr);
        return static_cast<PaneFocus>(proxy->focus());
    };

    SECTION("a move with no neighbour to move to")
    {
        manager.focusPane(vtworkspace::FocusDirection::Right, right);
        CHECK(focusOf(right) == PaneFocus::Focused);
    }

    SECTION("a split after a keyboard move")
    {
        manager.focusPane(vtworkspace::FocusDirection::Left, right);
        REQUIRE(focusOf(left) == PaneFocus::KeyboardFocused);
        manager.splitActivePane(/*vertical*/ false, left);
        auto* const added = window->activeSession();
        REQUIRE(added != left);
        CHECK(focusOf(added) == PaneFocus::Focused);
    }

    SECTION("a close after a keyboard move")
    {
        manager.focusPane(vtworkspace::FocusDirection::Left, right);
        REQUIRE(focusOf(left) == PaneFocus::KeyboardFocused);
        manager.closeActivePane(left, contour::session::SessionEnd::Destroy);
        left->onClosed();
        REQUIRE(window->activeSession() == right);
        CHECK(focusOf(right) == PaneFocus::Focused);
    }

    SECTION("a swap after a keyboard move")
    {
        manager.focusPane(vtworkspace::FocusDirection::Left, right);
        REQUIRE(focusOf(left) == PaneFocus::KeyboardFocused);
        manager.swapPane(vtworkspace::FocusDirection::Right, left);
        REQUIRE(window->activeSession() == left);
        CHECK(focusOf(left) == PaneFocus::Focused);
    }

    SECTION("a revealed session after a keyboard move")
    {
        manager.focusPane(vtworkspace::FocusDirection::Left, right);
        REQUIRE(focusOf(left) == PaneFocus::KeyboardFocused);
        manager.revealSession(*right);
        REQUIRE(window->activeSession() == right);
        CHECK(focusOf(right) == PaneFocus::Focused);
        CHECK(focusOf(left) == PaneFocus::Unfocused);
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: visiblePaneCount follows splits and zoom", "[manager][panes]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    CHECK(window->visiblePaneCount() == 0);

    window->createNewTab();
    CHECK(window->visiblePaneCount() == 1);
    auto const changed = QSignalSpy { &*window, &contour::window::WindowController::visiblePaneCountChanged };

    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    CHECK(window->visiblePaneCount() == 2);
    CHECK(changed.count() == 1);

    manager.splitActivePane(/*vertical*/ false, window->activeSession());
    CHECK(window->visiblePaneCount() == 3);
    CHECK(changed.count() == 2);

    manager.toggleActivePaneZoom(window->activeSession());
    CHECK(window->visiblePaneCount() == 1);
    CHECK(changed.count() == 3);

    manager.toggleActivePaneZoom(window->activeSession());
    CHECK(window->visiblePaneCount() == 3);
    CHECK(changed.count() == 4);

    contour::test::closeAllTabs(*window);
}

namespace
{

/// The MockPty behind @p session: what a test names the program in front through.
[[nodiscard]] vtpty::MockPty& mockPtyOf(contour::session::TerminalSession& session)
{
    return dynamic_cast<vtpty::MockPty&>(session.pty());
}

/// The sessions of tab @p row of @p window, in tree order.
[[nodiscard]] std::vector<contour::session::TerminalSession*> sessionsOfTab(
    contour::session::TerminalSessionManager& manager, contour::window::WindowController& window, int row)
{
    auto sessions = std::vector<contour::session::TerminalSession*> {};
    manager.model().window(window.windowId())->tabAt(row)->rootPane()->walkTree([&](vtworkspace::Pane& pane) {
        if (pane.isLeaf())
            sessions.push_back(manager.sessionForId(pane.session()));
    });
    return sessions;
}

/// Whether every one of @p sessions polls the program in front of it.
[[nodiscard]] bool allPoll(std::vector<contour::session::TerminalSession*> const& sessions)
{
    return std::ranges::all_of(sessions, &contour::session::TerminalSession::pollsForegroundProcess);
}

/// Whether none of @p sessions does.
[[nodiscard]] bool nonePoll(std::vector<contour::session::TerminalSession*> const& sessions)
{
    return std::ranges::none_of(sessions, &contour::session::TerminalSession::pollsForegroundProcess);
}

} // namespace

TEST_CASE("TerminalSessionManager: a single pane polls nothing", "[manager][processname]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { app.manager() };
    window->createNewTab();
    REQUIRE(window->activeSession() != nullptr);
    CHECK_FALSE(window->activeSession()->pollsForegroundProcess());
    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a split's visible panes poll, and only on change",
          "[manager][processname]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != left);
    CHECK(left->pollsForegroundProcess());
    CHECK(right->pollsForegroundProcess());

    auto const changed =
        QSignalSpy { left, &contour::session::TerminalSession::foregroundProcessNameChanged };
    mockPtyOf(*left).setForegroundProcessName("vim");
    left->pollForegroundProcess();
    CHECK(changed.count() == 1);
    CHECK(left->getForegroundProcessName() == QStringLiteral("vim"));
    CHECK(right->getForegroundProcessName().isEmpty());

    left->pollForegroundProcess();
    CHECK(changed.count() == 1);

    SECTION("closing back to one pane stops the poll")
    {
        manager.closeActivePane(right, contour::session::SessionEnd::Destroy);
        right->onClosed();
        CHECK(window->visiblePaneCount() == 1);
        CHECK_FALSE(left->pollsForegroundProcess());
    }

    SECTION("zooming one stops the poll, and unzooming starts it again")
    {
        manager.toggleActivePaneZoom(right);
        CHECK_FALSE(left->pollsForegroundProcess());
        CHECK_FALSE(right->pollsForegroundProcess());
        manager.toggleActivePaneZoom(right);
        CHECK(left->pollsForegroundProcess());
        CHECK(right->pollsForegroundProcess());
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: the first follower polls at once", "[manager][processname]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);

    // Named before the split shows its header: the header reads the name without waiting for a tick.
    mockPtyOf(*left).setForegroundProcessName("vim");
    manager.splitActivePane(/*vertical*/ true, left);
    CHECK(left->getForegroundProcessName() == QStringLiteral("vim"));

    // And a tab brought to the front reads its panes' names as it shows them.
    window->createNewTab();
    REQUIRE_FALSE(left->pollsForegroundProcess());
    mockPtyOf(*left).setForegroundProcessName("top");
    window->activateTab(0);
    CHECK(left->getForegroundProcessName() == QStringLiteral("top"));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: an extension and a header follow the foreground process together",
          "[manager][processname]")
{
    using contour::session::ProcessNameFollower;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    REQUIRE(left->pollsForegroundProcess()); // its header follows

    // A header's poll tells the extensions nothing they did not ask for.
    mockPtyOf(*left).setForegroundProcessName("vim");
    left->pollForegroundProcess();
    REQUIRE(left->getForegroundProcessName() == QStringLiteral("vim"));
    CHECK(left->foregroundProcessToldToExtensions().empty());

    left->startFollowingForegroundProcess(ProcessNameFollower::Extension);
#ifdef CONTOUR_EXTENSIONS
    // Told at once, as the extension starts following: the name was known.
    CHECK(left->foregroundProcessToldToExtensions() == "vim");
#endif

    SECTION("stopping the header keeps the extension's poll")
    {
        left->stopFollowingForegroundProcess(ProcessNameFollower::Header);
        CHECK(left->pollsForegroundProcess());
        left->stopFollowingForegroundProcess(ProcessNameFollower::Extension);
        CHECK_FALSE(left->pollsForegroundProcess());
    }

    SECTION("stopping the extension keeps the header's poll")
    {
        left->stopFollowingForegroundProcess(ProcessNameFollower::Extension);
        CHECK(left->pollsForegroundProcess());

        // And the extensions are no longer told.
        mockPtyOf(*left).setForegroundProcessName("less");
        left->pollForegroundProcess();
        CHECK(left->getForegroundProcessName() == QStringLiteral("less"));
#ifdef CONTOUR_EXTENSIONS
        CHECK(left->foregroundProcessToldToExtensions() == "vim");
#endif

        left->stopFollowingForegroundProcess(ProcessNameFollower::Header);
        CHECK_FALSE(left->pollsForegroundProcess());
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: only the active tab's pane headers poll", "[manager][processname]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 4))
    {
        window->createNewTab();
        manager.splitActivePane(/*vertical*/ true, window->activeSession());
        manager.splitActivePane(/*vertical*/ false, window->activeSession());
    }
    REQUIRE(window->activeTabIndex() == 3);
    for (auto const row: std::views::iota(0, 4))
        REQUIRE(sessionsOfTab(manager, *window, row).size() == 3);

    CHECK(allPoll(sessionsOfTab(manager, *window, 3)));
    for (auto const row: std::views::iota(0, 3))
        CHECK(nonePoll(sessionsOfTab(manager, *window, row)));

    window->activateTab(2);
    CHECK(allPoll(sessionsOfTab(manager, *window, 2)));
    CHECK(nonePoll(sessionsOfTab(manager, *window, 0)));
    CHECK(nonePoll(sessionsOfTab(manager, *window, 1)));
    CHECK(nonePoll(sessionsOfTab(manager, *window, 3)));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: under Minimal, no pane header polls", "[manager][processname]")
{
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = testApp.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(allPoll(sessionsOfTab(manager, *window, 0)));

    // A density change refreshes who follows: the held modifier that would reveal the headers is not held.
    contour::test::writeConfigFile(
        testApp.configFile(),
        contour::test::profileConfigYaml(testApp.app().profileName(), "chrome_density: minimal\n"));
    REQUIRE(manager.reloadAllSessions());
    REQUIRE(testApp.app().config().chromeDensity.value() == contour::config::ChromeDensity::Minimal);
    CHECK(nonePoll(sessionsOfTab(manager, *window, 0)));

    // And a split made under it starts none.
    manager.splitActivePane(/*vertical*/ false, window->activeSession());
    CHECK(nonePoll(sessionsOfTab(manager, *window, 0)));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a pane header's button acts on its own pane", "[manager][panes]")
{
    using contour::window::PaneHeaderAction;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    manager.splitActivePane(/*vertical*/ true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != left);
    REQUIRE(window->visiblePaneCount() == 2);

    // Out of range: nothing.
    auto* const leftProxy = proxyShowing(window->activeTabRootPane(), left);
    REQUIRE(leftProxy != nullptr);
    leftProxy->runHeaderAction(-1);
    leftProxy->runHeaderAction(static_cast<int>(contour::config::PaneHeaderActionCount));
    CHECK(window->visiblePaneCount() == 2);
    CHECK(window->activeSession() == right);

    // The inactive pane's "split right" splits that pane, which the pointer has made the active one.
    leftProxy->runHeaderAction(static_cast<int>(PaneHeaderAction::SplitRight));
    CHECK(window->visiblePaneCount() == 3);
    auto* const added = window->activeSession();
    REQUIRE(added != nullptr);
    CHECK(added != left);
    CHECK(added != right);
    auto const* const tab = manager.model().window(window->windowId())->tabAt(0);
    auto const* const addedLeaf = tab->rootPane()->findLeaf(added->modelSessionId());
    REQUIRE(addedLeaf != nullptr);
    CHECK(addedLeaf->parent() == tab->rootPane()->findLeaf(left->modelSessionId())->parent());
    CHECK(manager.lastPaneActivation(tab->id()) == contour::window::PaneActivation::Pointer);

    contour::test::closeAllTabs(*window);
}

namespace
{
/// An item that holds keyboard focus in its window and records the keys pressed into it.
class KeyRecorder: public QQuickItem
{
  public:
    std::vector<int> pressed; ///< Every key pressed into the item, in order.

  protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        pressed.push_back(event->key());
        QQuickItem::keyPressEvent(event);
    }
};

/// Whether @p window's headers are revealed by the held modifier.
[[nodiscard]] bool revealed(contour::window::WindowController const& window)
{
    return window.headerReveal() == static_cast<int>(contour::window::HeaderReveal::HeldModifier);
}

/// Longer than the reveal's 500 ms hold, for a wait that must see it.
constexpr auto RevealWaitMs = 1000;

/// How often a held key repeats, roughly as Windows repeats it.
constexpr auto RepeatIntervalMs = 50;

/// The reveal cases' window edge: room for a click at (10, 10).
constexpr auto RevealWindowSize = 200;

/// One notch of a mouse wheel, in eighths of a degree.
constexpr auto WheelStep = 120;
} // namespace

TEST_CASE("WindowController: holding the reveal modifier alone reveals the headers",
          "[contour][panes][reveal]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedControllerWithOsWindow const window { app.manager() };
    window->bindWindow(window.osWindow.get());
    window->createNewTab();
    auto* const osWindow = window.osWindow.get();
    // Room for the clicks below: an unsized window holds no point.
    osWindow->resize(RevealWindowSize, RevealWindowSize);
    auto recorder = KeyRecorder {};
    recorder.setParentItem(osWindow->contentItem());
    recorder.forceActiveFocus();
    REQUIRE_FALSE(revealed(*window));

    SECTION("a hold reveals, and other input hides")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        CHECK(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyClick(osWindow, Qt::Key_X);
        CHECK_FALSE(revealed(*window));
        QTest::keyRelease(osWindow, Qt::Key_Alt);

        // The filter consumes nothing: the terminal still received the press.
        CHECK(std::ranges::find(recorder.pressed, static_cast<int>(Qt::Key_Alt)) != recorder.pressed.end());
    }

    SECTION("a release before the delay never reveals")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        QTest::keyRelease(osWindow, Qt::Key_Alt);
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
    }

    SECTION("a press while revealed stays revealed")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        REQUIRE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::mouseClick(osWindow, Qt::LeftButton, Qt::AltModifier, QPoint { 10, 10 });
        CHECK(revealed(*window));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
    }

    SECTION("a press before the delay never reveals")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        QTest::mouseClick(osWindow, Qt::LeftButton, Qt::AltModifier, QPoint { 10, 10 });
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
    }

    SECTION("other input before the delay never reveals")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        QTest::keyClick(osWindow, Qt::Key_X, Qt::AltModifier);
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
    }

    SECTION("Alt with another modifier, or AltGr, never arms")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt, Qt::ControlModifier);
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_Alt, Qt::ControlModifier);

        QTest::keyPress(osWindow, Qt::Key_AltGr);
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_AltGr);
    }

    SECTION("an auto-repeated press neither re-arms nor disarms")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        auto const pressedAt = std::chrono::steady_clock::now();
        // Windows repeats a held Alt as further presses: they must leave the first press's time alone.
        auto repeat =
            QKeyEvent { QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier, QString {}, /*autorep*/ true };
        while (!revealed(*window)
               && std::chrono::steady_clock::now() - pressedAt < std::chrono::milliseconds { RevealWaitMs })
        {
            QCoreApplication::sendEvent(osWindow, &repeat);
            QTest::qWait(RepeatIntervalMs);
        }
        CHECK(revealed(*window));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
    }

    SECTION("a wheel turn while held never reveals")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        auto wheel =
            QWheelEvent { QPointF { 10, 10 }, QPointF { 10, 10 }, QPoint {},         QPoint { 0, WheelStep },
                          Qt::NoButton,       Qt::AltModifier,    Qt::NoScrollPhase,
                          /*inverted*/ false };
        QCoreApplication::sendEvent(osWindow, &wheel);
        CHECK_FALSE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
    }

    SECTION("the window losing focus counts as the release")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        REQUIRE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        // The window was never activated, so its activeChanged reads as a deactivation.
        REQUIRE_FALSE(osWindow->isActive());
        REQUIRE(QMetaObject::invokeMethod(osWindow, "activeChanged"));
        CHECK_FALSE(revealed(*window));
    }

    SECTION("a release hides")
    {
        QTest::keyPress(osWindow, Qt::Key_Alt);
        REQUIRE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
        QTest::keyRelease(osWindow, Qt::Key_Alt);
        CHECK_FALSE(revealed(*window));
    }

    recorder.setParentItem(nullptr);
    contour::test::closeAllTabs(*window);
}

TEST_CASE("WindowController: under Minimal, the reveal makes the headers poll", "[contour][panes][reveal]")
{
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = testApp.manager();
    contour::test::writeConfigFile(
        testApp.configFile(),
        contour::test::profileConfigYaml(testApp.app().profileName(), "chrome_density: minimal\n"));
    REQUIRE(manager.reloadAllSessions());
    REQUIRE(testApp.app().config().chromeDensity.value() == contour::config::ChromeDensity::Minimal);

    contour::test::ScopedControllerWithOsWindow const window { manager };
    window->bindWindow(window.osWindow.get());
    window->createNewTab();
    manager.splitActivePane(/*vertical*/ true, window->activeSession());
    REQUIRE(nonePoll(sessionsOfTab(manager, *window, 0)));

    auto* const osWindow = window.osWindow.get();
    QTest::keyPress(osWindow, Qt::Key_Alt);
    REQUIRE(QTest::qWaitFor([&] { return revealed(*window); }, RevealWaitMs));
    CHECK(allPoll(sessionsOfTab(manager, *window, 0)));

    QTest::keyRelease(osWindow, Qt::Key_Alt);
    CHECK(nonePoll(sessionsOfTab(manager, *window, 0)));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a remote-hosted pane has no process name", "[manager][processname]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    factoryOwned->reportedHosting = contour::session::SessionHosting::Remote;
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical*/ true, left);
    REQUIRE(sessionsOfTab(manager, *window, 0).size() == 2);

    // Its program runs on the host, which names it there: this machine's device is asked nothing.
    mockPtyOf(*left).setForegroundProcessName("vim");
    auto const changed =
        QSignalSpy { left, &contour::session::TerminalSession::foregroundProcessNameChanged };
    CHECK(nonePoll(sessionsOfTab(manager, *window, 0)));
    left->pollForegroundProcess();
    CHECK(changed.count() == 0);
    CHECK(left->getForegroundProcessName().isEmpty());
    CHECK(left->foregroundProcessName().empty());

    contour::test::closeAllTabs(*window);
}

// }}}

TEST_CASE("TerminalSessionManager: revealing a session brings its tab and pane forward",
          "[manager][notification]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const first = window->activeSession();
    REQUIRE(first != nullptr);
    manager.splitActivePane(/*vertical*/ true, first); // the new pane takes focus
    auto* const second = window->activeSession();
    REQUIRE(second != first);
    window->createNewTab(); // and a second tab hides both
    REQUIRE(window->activeTabIndex() == 1);

    // The tab's previously active pane reports focus changes, as vim would ask it to.
    second->terminal().setMode(vtbackend::DECMode::FocusTracking, true);
    contour::test::mockPtyOf(*second).stdinBuffer().clear();

    SECTION("from another tab")
    {
        manager.revealSession(*first);
    }

    CHECK(window->activeTabIndex() == 0);
    CHECK(window->activeSession() == first);
    CHECK(manager.sessionVisibility(*first) == contour::platform::SessionVisibility::Focused);

    // The pane moves before the tab does, so the pane that was active in the hidden tab never receives
    // focus on the way: no CSI I, and no CSI O after it.
    CHECK(contour::test::mockPtyOf(*second).stdinBuffer().empty());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a session's tab title and attention requests reach its window",
          "[manager][notification]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    // The label the strip shows, read back through it rather than re-derived.
    manager.setTabTitleForSession(session->modelSessionId(), "build");
    CHECK(manager.tabTitleForSession(*session) == "build");

    // Each session is named by its own tab's row, whichever tab is shown.
    window->createNewTab();
    auto* const other = window->activeSession();
    REQUIRE(other != nullptr);
    manager.setTabTitleForSession(other->modelSessionId(), "logs");
    CHECK(manager.tabTitleForSession(*session) == "build");
    CHECK(manager.tabTitleForSession(*other) == "logs");

    // Attention goes to the window hosting the session and to no other. What each signal carries is the
    // window's own business, pinned by its tests.
    contour::test::ScopedController const elsewhere { manager };
    struct Spies
    {
        QSignalSpy notifications;
        QSignalSpy bells;
        QSignalSpy alerts;

        explicit Spies(contour::window::WindowController* controller):
            notifications(controller, &contour::window::WindowController::notificationRequested),
            bells(controller, &contour::window::WindowController::bellRequested),
            alerts(controller, &contour::window::WindowController::alertRequested)
        {
        }
    };
    Spies const hosting { window.controller };
    Spies const bystander { elsewhere.controller };

    manager.requestWindowNotification(
        *session, QStringLiteral("make finished"), QStringLiteral("took 12s"), {});
    manager.requestWindowBell(*session, QStringLiteral("qrc:/contour/bell.wav"), 0.5);
    manager.requestWindowAlert(*session);

    CHECK(hosting.notifications.count() == 1);
    CHECK(hosting.bells.count() == 1);
    CHECK(hosting.alerts.count() == 1);
    CHECK(bystander.notifications.count() == 0);
    CHECK(bystander.bells.count() == 0);
    CHECK(bystander.alerts.count() == 0);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: naming a session's tab locks only that tab's terminal",
          "[manager][notification][threading]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    manager.setTabTitleForSession(session->modelSessionId(), "build");
    window->createNewTab();
    auto* const busy = window->activeSession();
    REQUIRE(busy != nullptr);

    // Another tab's parser holds its terminal across a whole write, as `cat huge.log` would.
    auto title = std::string {};
    CHECK(
        contour::test::runWhileHeld(busy->terminal(), [&]() { title = manager.tabTitleForSession(*session); })
        == contour::test::UnderLock::Finished);
    CHECK(title == "build");

    contour::test::closeAllTabs(*window);
}

#ifdef __linux__
TEST_CASE("TerminalSessionManager: a session the test harness makes reaches no desktop notification service",
          "[manager][notification]")
{
    // The platform's own notifier on Linux is a FreeDesktopNotifier talking to the machine's session bus,
    // over D-Bus or its portal: a suite that hands every session it makes such a one runs a D-Bus
    // connection thread, subscribes to the desktop's signals and tears it down again, per session. The
    // harness stands in for the desktop, so what it makes must not reach it.
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { app.manager() };

    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    CHECK(dynamic_cast<contour::platform::FreeDesktopNotifier const*>(&session->notifier()) == nullptr);

    contour::test::closeAllTabs(*window);
}
#endif

TEST_CASE("TerminalSessionManager: the tab tooltip shortens a directory against the injected home",
          "[manager][tabs]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    // OSC 7 names a directory beneath the home as file://HOST/PATH; on Windows the path is `C:/...`
    // and takes a slash before it.
    auto const& home = manager.homeDirectory();
    REQUIRE_FALSE(home.empty());
    auto const path = std::string { home.starts_with('/') ? "" : "/" } + home + "/proj";

    SECTION("on this machine")
    {
        session->terminal().setCurrentWorkingDirectory(
            "file://" + std::string { contour::test::TestApp::HostName } + path);
        CHECK(window->tabWorkingDirectory(0) == QStringLiteral("~/proj"));
    }

    SECTION("on another machine, where this machine's home means nothing")
    {
        session->terminal().setCurrentWorkingDirectory("file://some-remote-host" + path);
        CHECK(window->tabWorkingDirectory(0) == QString::fromStdString(home + "/proj"));
    }

    SECTION("on another machine, still made safe to show")
    {
        // OSC 7 is percent-decoded, so %0A arrives as a line feed.
        session->terminal().setCurrentWorkingDirectory("file://some-remote-host" + path + "%0Ax");
        CHECK_FALSE(window->tabWorkingDirectory(0).contains(QChar('\n')));
    }

    contour::test::closeAllTabs(*window);
}

// }}}

TEST_CASE("TerminalSessionManager: a configuration reload re-applies the GUI chrome theme",
          "[manager][theme]")
{
    // With `theme: terminal` the chrome follows the terminal background, so a colour change made by hand
    // in contour.yml must recolor the chrome on reload, not only after a restart.
    contour::test::ConfiguredTestApp testApp;
    auto const windowLightness = [] {
        return QGuiApplication::palette().color(QPalette::Window).lightness();
    };

    testApp.app().applyGuiTheme(contour::config::GuiTheme::Light, testApp.app().defaultProfileColors());
    auto const lightChrome = windowLightness();

    contour::test::writeConfigFile(
        testApp.configFile(), contour::test::profileConfigYaml(testApp.app().profileName(), "theme: dark\n"));
    REQUIRE(testApp.manager().reloadAllSessions());

    CHECK(testApp.app().config().theme.value() == contour::config::GuiTheme::Dark);
    CHECK(windowLightness() < lightChrome);
}

namespace
{
/// The lightness of the application palette's Window role: how dark or light the chrome is.
[[nodiscard]] int chromeWindowLightness()
{
    return QGuiApplication::palette().color(QPalette::Window).lightness();
}

/// Loads @p theme over the default (Classic, so simple and black) colours, which darkens the chrome
/// under both `terminal` and `dark`.
void loadTheme(contour::test::ConfiguredTestApp& testApp,
               std::string_view themeYaml,
               contour::config::GuiTheme expected)
{
    contour::test::writeConfigFile(testApp.configFile(),
                                   contour::test::profileConfigYaml(testApp.app().profileName(), themeYaml));
    REQUIRE(testApp.manager().reloadAllSessions());
    REQUIRE(testApp.app().config().theme.value() == expected);
    REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(testApp.app().defaultProfileColors()));
}

/// Loads `theme: terminal`. @see loadTheme.
void loadTerminalTheme(contour::test::ConfiguredTestApp& testApp)
{
    loadTheme(testApp, "theme: terminal\n", contour::config::GuiTheme::Terminal);
}

/// A palette no theme produces, so a test can tell whether the chrome theme was re-applied over it.
[[nodiscard]] QPalette sentinelPalette()
{
    auto palette = QGuiApplication::palette();
    palette.setColor(QPalette::Window, QColor::fromRgb(0x123456));
    return palette;
}

constexpr auto LightBackgroundSequence = std::string_view { "\033]11;rgb:fafa/fafa/f8f8\033\\" };
} // namespace

TEST_CASE("TerminalSessionManager: OSC 11 in the focused session recolours a terminal-themed chrome",
          "[manager][theme][chromefollow]")
{
    // Sessions over mock ptys, as in every case of this group: the app's own factory would spawn a shell
    // and hand each session the platform's notifier, which on Linux reaches the machine's session bus.
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    loadTerminalTheme(testApp);
    contour::test::ScopedController const win { testApp.manager() };

    auto* session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    testApp.manager().setFocusedSession(session);
    QCoreApplication::processEvents();
    auto const darkChrome = chromeWindowLightness();
    REQUIRE(darkChrome < 128);

    session->terminal().writeToScreen(LightBackgroundSequence);
    QCoreApplication::processEvents();

    CHECK(chromeWindowLightness() > darkChrome);
}

TEST_CASE("TerminalSessionManager: OSC 11 in an unfocused session leaves the chrome alone",
          "[manager][theme][chromefollow]")
{
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    loadTerminalTheme(testApp);
    contour::test::ScopedController const win { testApp.manager() };

    auto* first = testApp.manager().createSession(win.id);
    auto* second = testApp.manager().createSession(win.id);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->modelSessionId() != second->modelSessionId());
    testApp.manager().setFocusedSession(second);
    QCoreApplication::processEvents();
    auto const darkChrome = chromeWindowLightness();
    REQUIRE(darkChrome < 128);

    first->terminal().writeToScreen(LightBackgroundSequence);
    QCoreApplication::processEvents();
    CHECK(chromeWindowLightness() == darkChrome);

    testApp.manager().setFocusedSession(first);
    CHECK(chromeWindowLightness() > darkChrome);
}

TEST_CASE("TerminalSessionManager: OSC 11 under a forced theme leaves the chrome and its palette alone",
          "[manager][theme][chromefollow]")
{
    // Only `theme: terminal` follows the background: under `dark`, re-applying would repaint the whole
    // application for the scheme it already shows. Qt skips a palette equal to the current one, so a
    // sentinel palette stands in for the applied one: any re-apply overwrites it.
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    loadTheme(testApp, "theme: dark\n", contour::config::GuiTheme::Dark);
    contour::test::ScopedController const win { testApp.manager() };

    auto* session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    testApp.manager().setFocusedSession(session);
    QCoreApplication::processEvents();
    REQUIRE(chromeWindowLightness() < 128);
    auto const sentinel = sentinelPalette();
    QGuiApplication::setPalette(sentinel);

    session->terminal().writeToScreen(LightBackgroundSequence);
    QCoreApplication::processEvents();

    CHECK(QGuiApplication::palette().color(QPalette::Window) == sentinel.color(QPalette::Window));
}

TEST_CASE("TerminalSessionManager: an OSC 11 that keeps the chrome's scheme does not repaint it",
          "[manager][theme][chromefollow]")
{
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    loadTerminalTheme(testApp);
    contour::test::ScopedController const win { testApp.manager() };

    auto* session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    testApp.manager().setFocusedSession(session);
    QCoreApplication::processEvents();
    auto const sentinel = sentinelPalette();
    QGuiApplication::setPalette(sentinel);

    session->terminal().writeToScreen("\033]11;rgb:2020/2020/2020\033\\"); // still dark
    QCoreApplication::processEvents();

    CHECK(QGuiApplication::palette().color(QPalette::Window) == sentinel.color(QPalette::Window));
}

// {{{ An ended pane: Restart in the same pane, Close, and what a remote pane offers

namespace
{
/// Ends @p session the way a shell that died right after starting does: its device closes, and the exit
/// watcher reports it -- called here in the watcher's stead.
void endEarly(contour::session::TerminalSession& session)
{
    session.terminal().device().close();
    session.onClosed();
}

/// Presses Return in @p session.
void pressReturn(contour::session::TerminalSession& session)
{
    session.sendKeyEvent(vtbackend::Key::Enter,
                         vtbackend::Modifiers {},
                         vtbackend::KeyboardEventType::Press,
                         std::chrono::steady_clock::now());
}

/// The leaf of @p window's active tab that carries @p session.
[[nodiscard]] vtworkspace::Pane* leafOf(contour::session::TerminalSessionManager& manager,
                                        vtworkspace::WindowId window,
                                        contour::session::TerminalSession const& session)
{
    return manager.model().window(window)->activeTab()->rootPane()->findLeaf(session.modelSessionId());
}
} // namespace

TEST_CASE("TerminalSessionManager: Return restarts an ended pane in place", "[manager][paneend]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    auto& profiles = app.app().config().profiles.value();
    profiles["work"] = profiles.at(app.app().profileName());

    // A split whose left pane runs the "work" profile in a directory its shell reported, and is focused.
    auto* ended = manager.createSession(window.id, "work");
    REQUIRE(ended != nullptr);
    manager.splitActivePane(/*vertical*/ true, ended);
    auto* sibling = window->activeSession();
    REQUIRE(sibling != ended);
    manager.focusPane(vtworkspace::FocusDirection::Left, sibling);
    REQUIRE(window->activeSession() == ended);

    auto const temp = core::testing::ScopedTempDir { "contour-restart" };
    ended->terminal().setCurrentWorkingDirectory(
        core::platform::fileUri(core::platform::normalizePath(temp.path()), "localhost"));
    auto const spawnCwd = ended->resolveWorkingDirectory(vtbackend::CwdPurpose::Spawn);
    REQUIRE(spawnCwd.has_value());

    auto const oldId = ended->modelSessionId();
    auto const* leaf = leafOf(manager, window.id, *ended);
    REQUIRE(leaf != nullptr);
    auto const leafId = leaf->id();
    auto const watched = QPointer<contour::session::TerminalSession> { ended };

    endEarly(*ended);
    REQUIRE(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    REQUIRE(ended->endAction() == contour::session::FooterAction::Restart);
    auto const ptysBefore = factory->createdPtys.size();

    // The view half: the leaf's proxy is what QML's pane reads its `session` from, so it must re-announce.
    auto* proxy = window->getProxy(leafId);
    REQUIRE(proxy != nullptr);
    REQUIRE(proxy->session() == ended);
    auto proxyChanges = QSignalSpy(proxy, &contour::session::PaneProxy::changed);
    auto activeSessionChanges =
        QSignalSpy(&*window, &contour::window::WindowController::activeSessionChanged);
    auto rowChanges = QSignalSpy(&*window, &contour::window::WindowController::dataChanged);

    pressReturn(*ended);

    // One new session, under the ended one's profile and in its directory.
    REQUIRE(factory->createdPtys.size() == ptysBefore + 1);
    REQUIRE(factory->requestedProfileNames.back().has_value());
    CHECK(*factory->requestedProfileNames.back() == "work");
    CHECK(factory->requestedCwds.back() == std::optional { spawnCwd->path });

    // In the same pane: the leaf keeps its id and now carries a new session, which is live and active.
    auto* tab = manager.model().window(window.id)->activeTab();
    REQUIRE(tab->paneCount() == 2);
    auto const* sameLeaf = tab->rootPane()->findPane(leafId);
    REQUIRE(sameLeaf != nullptr);
    CHECK(sameLeaf->session() != oldId);
    auto* restarted = manager.sessionForId(sameLeaf->session());
    REQUIRE(restarted != nullptr);
    CHECK(restarted->getSessionEnd() == static_cast<int>(contour::session::EndReason::None));
    CHECK(tab->activePane()->id() == leafId);
    CHECK(window->activeSession() == restarted);
    CHECK(manager.sessionForId(oldId) == nullptr);

    // And the view rebinds: the pane's proxy re-announced once and now names the new session, and the
    // window's active session, which the restarted pane was, changed with it.
    CHECK(proxyChanges.count() == 1);
    CHECK(proxy->session() == restarted);
    CHECK(activeSessionChanges.count() == 1);

    // The tab strip re-reads every role its active session feeds, not only the id and the label.
    auto announced = QSet<int> {};
    for (auto const& change: rowChanges)
        for (auto const role: change.at(2).value<QList<int>>())
            announced.insert(role);
    using Roles = contour::window::WindowController::Roles;
    for (auto const role: { Roles::SessionIdRole,
                            Roles::TitleRole,
                            Roles::ProgressStateRole,
                            Roles::ProgressPercentageRole,
                            Roles::BadgeRole,
                            Roles::BadgeTextRole,
                            Roles::BadgeProgressRole })
        CHECK(announced.contains(static_cast<int>(role)));

    // The ended session is let go once the call that ended it has returned.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(watched.isNull());

    SECTION("the next split mints a fresh id rather than the restarted one")
    {
        manager.splitActivePane(/*vertical*/ false, restarted);
        auto* third = window->activeSession();
        REQUIRE(third != nullptr);
        CHECK(third != restarted);
        CHECK(third->modelSessionId() != restarted->modelSessionId());
        CHECK(tab->paneCount() == 3);
    }

    SECTION("a restarted pane closes normally: its end prunes the leaf")
    {
        restarted->terminate();
        restarted->onClosed();
        CHECK(tab->paneCount() == 1);
        CHECK(manager.sessionForId(restarted->modelSessionId()) == nullptr);
        CHECK(window->activeSession() == sibling);
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: an ended pane's header stops following its process",
          "[manager][paneend][processname]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const ended = window->activeSession();
    REQUIRE(ended != nullptr);
    manager.splitActivePane(/*vertical*/ true, ended);
    auto* const survivor = window->activeSession();
    REQUIRE(ended->pollsForegroundProcess());

    // Its footer stands where its program ran: there is no program in front to name.
    endEarly(*ended);
    REQUIRE(ended->getSessionEnd() != static_cast<int>(contour::session::EndReason::None));
    CHECK_FALSE(ended->pollsForegroundProcess());
    CHECK(survivor->pollsForegroundProcess());

    // Nor does a later refresh -- a zoom and back -- start it again.
    manager.toggleActivePaneZoom(survivor);
    manager.toggleActivePaneZoom(survivor);
    CHECK_FALSE(ended->pollsForegroundProcess());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a modified Return in an ended pane is left to its bindings",
          "[manager][paneend]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    auto* ended = manager.createSession(window.id);
    REQUIRE(ended != nullptr);
    auto const endedId = ended->modelSessionId();
    endEarly(*ended);
    REQUIRE(ended->endAction() == contour::session::FooterAction::Restart);
    auto const ptysBefore = factory->createdPtys.size();

    // Alt+Enter is the default ToggleFullscreen binding (a no-op here, with no display), and Shift+Enter
    // a chord the user may bind: neither is Restart.
    for (auto const chord: { vtbackend::Modifiers { vtbackend::Modifier::Alt },
                             vtbackend::Modifiers { vtbackend::Modifier::Shift } })
        ended->sendKeyEvent(vtbackend::Key::Enter,
                            chord,
                            vtbackend::KeyboardEventType::Press,
                            std::chrono::steady_clock::now());

    CHECK(factory->createdPtys.size() == ptysBefore);
    CHECK(manager.sessionForId(endedId) == ended);
    CHECK(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));

    // A bare Return still is.
    pressReturn(*ended);
    CHECK(factory->createdPtys.size() == ptysBefore + 1);
    CHECK(manager.sessionForId(endedId) == nullptr);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a pane restarted mid-command starts with no blocks", "[manager][paneend]")
{
    // Review Focus 3: the block that was running when the shell died stays with the ended session; the
    // restarted one has nothing to show as running, in its rail, its header or its chrome.
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    auto* const ended = manager.createSession(window.id);
    REQUIRE(ended != nullptr);
    ended->terminal().writeToScreen(vtbackend::test::commandStarted("sleep 100"));
    REQUIRE(ended->terminal().commandBlocks().size() == 1);
    endEarly(*ended);
    auto const endedId = ended->modelSessionId();

    pressReturn(*ended);
    auto* const restarted = window->activeSession();
    REQUIRE(restarted != nullptr);
    REQUIRE(manager.sessionForId(endedId) == nullptr);
    CHECK(restarted->terminal().commandBlocks().size() == 0);
    CHECK(restarted->stickyHeader().isEmpty());
    CHECK(restarted->blockActionsAnchor().isEmpty());
    CHECK(restarted->focusedCommandBlock() == 0);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: ClosePane closes an ended pane", "[manager][paneend]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    window->createNewTab();
    auto* const survivor = window->activeSession();
    REQUIRE(survivor != nullptr);
    manager.splitActivePane(/*vertical*/ true, survivor);
    auto* const ended = window->activeSession();
    REQUIRE(ended != survivor);
    auto const endedId = ended->modelSessionId();

    endEarly(*ended);
    REQUIRE(ended->getSessionEnd() != static_cast<int>(contour::session::EndReason::None));

    // The default ClosePane chord: the input mappings still run while the pane waits, though text does not.
    auto const ctrlShift = vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift };
    ended->sendCharEvent(U'X',
                         vtbackend::KeyIdentity { .unshiftedKey = U'X' },
                         ctrlShift,
                         vtbackend::KeyboardEventType::Press,
                         std::chrono::steady_clock::now());

    CHECK(manager.model().window(window.id)->activeTab()->paneCount() == 1);
    CHECK(manager.sessionForId(endedId) == nullptr);
    CHECK(window->activeSession() == survivor);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: a remote-hosted pane offers no Restart", "[manager][paneend]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };

    auto* ended = manager.createSession(window.id);
    REQUIRE(ended != nullptr);
    auto const endedId = ended->modelSessionId();
    endEarly(*ended);

    // The daemon owns the session: only it may start one again.
    CHECK(ended->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    CHECK(ended->endAction() == contour::session::FooterAction::None);

    auto const ptysBefore = factory->createdPtys.size();
    pressReturn(*ended);
    CHECK(factory->createdPtys.size() == ptysBefore);
    CHECK(manager.sessionForId(endedId) == ended);
    CHECK_FALSE(manager.restartSession(*ended).has_value());

    contour::test::closeAllTabs(*window);
}

namespace
{
/// A split of two panes of a remote-hosted manager, both lost with the connection: their ptys closed after
/// noteTransportLost(), so each shows the Disconnected footer.
struct LostSplit
{
    contour::test::MockPtySessionFactory* factory;
    contour::test::TestApp app;
    contour::test::ScopedController window;
    contour::session::TerminalSession* left = nullptr;
    contour::session::TerminalSession* right = nullptr;
    vtworkspace::PaneId leftLeaf;
    vtworkspace::PaneId rightLeaf;

    static std::unique_ptr<contour::test::MockPtySessionFactory> makeFactory()
    {
        auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
        factory->reportedHosting = contour::session::SessionHosting::Remote;
        factory->reportedReconnectSupport = contour::session::ReconnectSupport::Supported;
        return factory;
    }

    explicit LostSplit(std::unique_ptr<contour::test::MockPtySessionFactory> owned = makeFactory()):
        factory { owned.get() }, app { std::move(owned) }, window { app.manager() }
    {
        left = app.manager().createSession(window.id);
        REQUIRE(left != nullptr);
        app.manager().splitActivePane(/*vertical*/ true, left);
        right = window->activeSession();
        REQUIRE(right != left);
        leftLeaf = leafOf(app.manager(), window.id, *left)->id();
        rightLeaf = leafOf(app.manager(), window.id, *right)->id();
        for (auto* session: { left, right })
        {
            session->noteTransportLost();
            session->terminal().device().close();
            session->onClosed();
            REQUIRE(session->endReason() == contour::session::EndReason::Disconnected);
        }
        factory->namedForReconnect = { &left->terminal().device(), &right->terminal().device() };
    }

    ~LostSplit() { contour::test::closeAllTabs(*window); }

    LostSplit(LostSplit const&) = delete;
    LostSplit& operator=(LostSplit const&) = delete;
    LostSplit(LostSplit&&) = delete;
    LostSplit& operator=(LostSplit&&) = delete;
};

/// @return The session the leaf @p leaf of @p manager's window now carries; null when none.
[[nodiscard]] contour::session::TerminalSession* sessionOfLeaf(
    contour::session::TerminalSessionManager& manager, vtworkspace::WindowId window, vtworkspace::PaneId leaf)
{
    auto const* pane = manager.model().window(window)->activeTab()->rootPane()->findPane(leaf);
    return pane != nullptr ? manager.sessionForId(pane->session()) : nullptr;
}
} // namespace

TEST_CASE("TerminalSessionManager: a lost pane offers Reconnect only where the app is attached",
          "[manager][paneend][reconnect]")
{
    using contour::session::FooterAction;
    auto lost = LostSplit {};
    CHECK(lost.left->endAction() == FooterAction::Reconnect);
    CHECK(lost.left->getFooterActionLabel() == "Reconnect");
    CHECK(lost.left->getFooterMessage()
          == "Disconnected from the Contour daemon. Its sessions are still running.");

    SECTION("a detached app offers Close alone, and keeps the reattach hint")
    {
        auto factory = LostSplit::makeFactory();
        factory->reportedReconnectSupport = contour::session::ReconnectSupport::Unsupported;
        auto detached = LostSplit { std::move(factory) };
        CHECK(detached.left->endAction() == FooterAction::None);
        CHECK(detached.left->getFooterMessage().contains(QStringLiteral("reattach with: contour client")));
    }
}

TEST_CASE("TerminalSessionManager: Reconnect rebinds the panes in place", "[manager][paneend][reconnect]")
{
    using contour::session::EndReason;
    using contour::window::PaneState;
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    auto const windows = manager.model().windowCount();
    auto const leftBefore = lost.left->modelSessionId();
    auto const ptysBefore = lost.factory->createdPtys.size();

    SECTION("Return on the footer reconnects too")
    {
        pressReturn(*lost.left);
        CHECK(lost.factory->reconnects == 1);
    }

    lost.right->runFooterAction();
    REQUIRE(lost.factory->reconnects >= 1);
    // The press of one is the answer of both: every pane lost with the connection seeks it again.
    CHECK(lost.left->getPaneState() == static_cast<int>(PaneState::Reconnecting));
    CHECK(lost.right->getPaneState() == static_cast<int>(PaneState::Reconnecting));
    CHECK(lost.left->isReconnecting());

    // The daemon's layout is back and names both sessions.
    manager.rebindDisconnected();

    CHECK(lost.factory->createdPtys.size() == ptysBefore + 2);
    auto* const newLeft = sessionOfLeaf(manager, lost.window.id, lost.leftLeaf);
    auto* const newRight = sessionOfLeaf(manager, lost.window.id, lost.rightLeaf);
    REQUIRE(newLeft != nullptr);
    REQUIRE(newRight != nullptr);
    CHECK(newLeft->modelSessionId() != leftBefore);
    for (auto* session: { newLeft, newRight })
    {
        CHECK(session->endReason() == EndReason::None);
        CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::None));
    }
    CHECK(manager.model().windowCount() == windows);
    CHECK(manager.model().window(lost.window.id)->activeTab()->paneCount() == 2);

    // Connected again, no pane shows the pill.
    manager.setHostConnection(vthost::client::ConnectionState::Connected);
    CHECK(newLeft->getPaneState() == static_cast<int>(PaneState::Live));
}

TEST_CASE("TerminalSessionManager: Reconnect takes back the panes of every window, layout by layout",
          "[manager][paneend][reconnect]")
{
    using contour::session::EndReason;
    // Two daemon windows: the daemon pushes one layout per window, and each reaches the GUI in its own turn.
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    auto const second = contour::test::ScopedController { manager };
    auto* const other = manager.createSession(second.id);
    REQUIRE(other != nullptr);
    auto const otherLeaf = leafOf(manager, second.id, *other)->id();
    other->noteTransportLost();
    other->terminal().device().close();
    other->onClosed();
    REQUIRE(other->endReason() == EndReason::Disconnected);

    // The first window's layout is the only one that has arrived.
    lost.factory->namedForReconnect = { &lost.left->terminal().device(), &lost.right->terminal().device() };
    auto const otherDevice = &other->terminal().device();
    lost.left->runFooterAction();
    REQUIRE(lost.factory->reconnects == 1);

    manager.rebindDisconnected();
    CHECK(sessionOfLeaf(manager, lost.window.id, lost.leftLeaf)->endReason() == EndReason::None);
    CHECK(manager.sessionForId(other->modelSessionId()) == other);

    // Then the second window's layout arrives: its pane comes back in the same run.
    lost.factory->namedForReconnect = { otherDevice };
    manager.rebindDisconnected();
    auto* const newOther = sessionOfLeaf(manager, second.id, otherLeaf);
    REQUIRE(newOther != nullptr);
    CHECK(newOther->endReason() == EndReason::None);

    // Nothing is left to take back, so the run is over: a later, unrelated loss makes this pane Disconnected
    // and a failure then is nobody's to report on it.
    newOther->noteTransportLost();
    newOther->terminal().device().close();
    newOther->onClosed();
    REQUIRE(newOther->endReason() == EndReason::Disconnected);
    manager.reconnectFailed("the connection closed");
    CHECK(newOther->getFooterMessage()
          == "Disconnected from the Contour daemon. Its sessions are still running.");

    contour::test::closeAllTabs(*second);
}

TEST_CASE("TerminalSessionManager: a refused reconnect keeps the footer with the reason",
          "[manager][paneend][reconnect]")
{
    using contour::window::PaneState;
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    auto const windows = manager.model().windowCount();

    SECTION("the daemon refuses the handshake")
    {
        lost.left->runFooterAction();
        REQUIRE(lost.left->getPaneState() == static_cast<int>(PaneState::Reconnecting));

        // The supervised run ends for good: the connection says why, and settles in Refused.
        manager.setHostConnection(vthost::client::ConnectionState::Refused);
        manager.reconnectFailed("the daemon refused the handshake");

        for (auto* session: { lost.left, lost.right })
        {
            CHECK(session->getPaneState() == static_cast<int>(PaneState::Ended));
            CHECK(session->getFooterMessage()
                  == QStringLiteral("Could not reconnect: the daemon refused the handshake."));
            CHECK(session->getFooterActionLabel() == "Reconnect"); // and it can be tried again
        }
    }

    SECTION("no connection could be started at all")
    {
        lost.factory->reconnectResult = std::unexpected(contour::session::ReconnectError::NotAttached);
        lost.left->runFooterAction();
        for (auto* session: { lost.left, lost.right })
            CHECK(session->getFooterMessage().startsWith(QStringLiteral("Could not reconnect: ")));
        CHECK(lost.left->getPaneState() == static_cast<int>(PaneState::Ended));
    }

    SECTION("a failure with no reconnect running is nobody's to report")
    {
        manager.reconnectFailed("the connection closed");
        CHECK(lost.left->getFooterMessage()
              == "Disconnected from the Contour daemon. Its sessions are still running.");
    }

    // No window was added, and no pane lost.
    CHECK(manager.model().windowCount() == windows);
    CHECK(manager.model().window(lost.window.id)->activeTab()->paneCount() == 2);
    CHECK(manager.sessionForId(lost.left->modelSessionId()) == lost.left);
}

TEST_CASE(
    "TerminalSessionManager: a later connection loss is not blamed on a Reconnect that left a pane behind",
    "[manager][paneend][reconnect]")
{
    using contour::session::EndReason;
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    // The returned layout omits the right pane's session, so the run stays up for it.
    lost.factory->namedForReconnect = { &lost.left->terminal().device() };
    lost.left->runFooterAction();
    manager.rebindDisconnected();
    auto* const newLeft = sessionOfLeaf(manager, lost.window.id, lost.leftLeaf);
    REQUIRE(newLeft != nullptr);
    REQUIRE(newLeft->endReason() == EndReason::None);

    // The connection then drops for good: the Reconnect's pane says so, the pane this loss just ended does
    // not.
    manager.connectionClosed("the connection closed");
    newLeft->terminal().device().close();
    newLeft->onClosed();

    CHECK(lost.right->getFooterMessage() == "Could not reconnect: the connection closed.");
    CHECK(newLeft->getFooterMessage()
          == "Disconnected from the Contour daemon. Its sessions are still running.");

    // And that ended the run: a failure now is nobody's to report.
    manager.reconnectFailed("another");
    CHECK(newLeft->getFooterMessage()
          == "Disconnected from the Contour daemon. Its sessions are still running.");
}

TEST_CASE("TerminalSessionManager: Return pressed as the connection fails shows the connection's own reason",
          "[manager][paneend][reconnect]")
{
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    // The controller is Failed but its connectionClosed has not been handled yet.
    lost.factory->reconnectResult = std::unexpected(contour::session::ReconnectError::ConnectionEnding);
    lost.left->runFooterAction();

    manager.connectionClosed("the daemon refused the handshake");

    for (auto* session: { lost.left, lost.right })
        CHECK(session->getFooterMessage()
              == QStringLiteral("Could not reconnect: the daemon refused the handshake."));
}

TEST_CASE("TerminalSessionManager: a pane the daemon no longer has stays ended",
          "[manager][paneend][reconnect]")
{
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    // The returned layout omits the right pane's session.
    lost.factory->namedForReconnect = { &lost.left->terminal().device() };
    lost.left->runFooterAction();

    manager.rebindDisconnected();

    auto* const newLeft = sessionOfLeaf(manager, lost.window.id, lost.leftLeaf);
    REQUIRE(newLeft != nullptr);
    CHECK(newLeft->endReason() == contour::session::EndReason::None);
    CHECK(sessionOfLeaf(manager, lost.window.id, lost.rightLeaf) == lost.right);
    CHECK(lost.right->endReason() == contour::session::EndReason::Disconnected);
    CHECK(lost.right->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Ended));
}

TEST_CASE("TerminalSessionManager: a failed take-back leaves no binding for the next session",
          "[manager][paneend][reconnect]")
{
    auto lost = LostSplit {};
    auto& manager = lost.app.manager();
    lost.factory->namedForReconnect = { &lost.left->terminal().device() };
    // The factory binds the pane, then cannot back the session that would mirror it.
    lost.factory->creation = contour::test::MockPtySessionFactory::SessionCreation::Refused;
    lost.left->runFooterAction();

    manager.rebindDisconnected();

    CHECK_FALSE(lost.factory->boundForReconnect);
    CHECK(lost.left->endReason() == contour::session::EndReason::Disconnected);
}

TEST_CASE("TerminalSessionManager: a restarted pane starts at the size it has once the footer hides",
          "[manager][paneend]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const ended = window->activeSession();
    REQUIRE(ended != nullptr);
    auto const running = ended->terminal().totalPageSize();
    REQUIRE(running.lines > vtbackend::LineCount(4));

    // The footer shows below the cells and takes two rows of the grid; a later window resize narrows it.
    endEarly(*ended);
    auto const resize = [&](vtbackend::PageSize size) {
        auto const lock = std::scoped_lock { ended->terminal() };
        ended->terminal().resizeScreen(size, std::nullopt);
    };
    // A resize on another thread -- the parser's, finishing the output that ended -- is not the footer's:
    // it measures nothing, and the footer's own resize below still does.
    std::thread { [&] {
        resize(vtbackend::PageSize { .lines = running.lines - 1, .columns = running.columns });
    } }.join();
    resize(vtbackend::PageSize { .lines = running.lines - 2, .columns = running.columns });
    resize(vtbackend::PageSize { .lines = running.lines - 2, .columns = running.columns - 10 });

    // The new shell starts with the rows the footer gives back, at the width the pane has now: no
    // SIGWINCH the moment the footer hides.
    auto const restarted = manager.restartSession(*ended);
    REQUIRE(restarted.has_value());
    CHECK((*restarted)->terminal().totalPageSize()
          == vtbackend::PageSize { .lines = running.lines, .columns = running.columns - 10 });

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: restarting an ended pane repaints its tab's badge",
          "[manager][paneend][tabbadge]")
{
    using contour::window::TabBadgeKind;

    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedControllerWithOsWindow const window { manager };
    window->bindWindow(window.osWindow.get());

    window->createNewTab();
    window->createNewTab();
    REQUIRE(window->activeTabIndex() == 1);
    auto* const ended = sessionAtRow(manager, *window, 0);
    REQUIRE(ended != nullptr);
    drainPosted();

    // Unseen output in the background tab, and the strip has read the mark it shows.
    ended->terminal().writeToScreen("x");
    drainPosted();
    endEarly(*ended);
    drainPosted();
    REQUIRE(badgeAt(*window, 0) != TabBadgeKind::None);

    // The restart forgets the ended session's marks; the new one has none, and the strip is told so: the
    // badge's own role changes, or the delegate bound to it keeps the stale mark.
    auto changed = QSignalSpy { &*window, &QAbstractItemModel::dataChanged };
    REQUIRE(manager.restartSession(*ended).has_value());
    drainPosted();
    CHECK(badgeAt(*window, 0) == TabBadgeKind::None);
    auto const badgeRole = static_cast<int>(contour::window::WindowController::Roles::BadgeRole);
    CHECK(std::ranges::any_of(changed, [&](QList<QVariant> const& arguments) {
        return arguments.at(0).toModelIndex().row() == 0
               && arguments.at(2).value<QList<int>>().contains(badgeRole);
    }));

    contour::test::closeAllTabs(*window);
}

// }}}

TEST_CASE("TerminalSessionManager: a configuration that fails to reload says so in every window, until one "
          "succeeds",
          "[contour][session][manager][notices]")
{
    // Real file I/O, as every reload case: reloadAllSessions() reads the app's configuration file from disk.
    contour::test::ConfiguredTestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const first { testApp.manager() };
    contour::test::ScopedController const second { testApp.manager() };
    auto const failure = QStringLiteral("Contour could not reload its configuration:");

    contour::test::writeConfigFile(testApp.configFile(), "profiles: [\n");
    REQUIRE_FALSE(testApp.manager().reloadAllSessions());
    CHECK(first->notices().bannerText().startsWith(failure));
    CHECK(second->notices().bannerText().startsWith(failure));
    CHECK(first->notices().bannerActionText() == "Show in Editor");

    // Its action opens the file it is about.
    first->notices().triggerBannerAction();
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.front()
          == QUrl::fromLocalFile(QString::fromStdString(core::pathToUtf8(testApp.configFile()))));

    contour::test::writeConfigFile(testApp.configFile(),
                                   contour::test::profileConfigYaml(testApp.app().profileName(), ""));
    REQUIRE(testApp.manager().reloadAllSessions());
    CHECK(first->notices().bannerText().isEmpty());
    CHECK(second->notices().bannerText().isEmpty());
}

// {{{ Secure input: a lock while the terminal hides typing, and the OS's secure keyboard entry

namespace
{

/// The doubles a secure-input app is built over. Declared before the app, so the counts can be read after it.
struct SecureInputDoubles
{
    contour::test::SecureInputRecord record {};
    contour::test::AccessibilitySwitch accessibility {};
    contour::test::AppearanceSwitch appearance {};
};

/// A test app over mock PTYs whose secure input counts into @p doubles, with `secure_input: auto`.
[[nodiscard]] std::unique_ptr<contour::test::TestApp> makeSecureInputApp(SecureInputDoubles& doubles)
{
    auto app = std::make_unique<contour::test::TestApp>(
        std::make_unique<contour::test::MockPtySessionFactory>(),
        nullptr,
        nullptr,
        nullptr,
        contour::test::TestApp::testHostIdentity(),
        nullptr,
        contour::test::TestEnvironment::Overrides {},
        nullptr,
        contour::test::appearanceServicesWith(doubles.accessibility, doubles.appearance, doubles.record));
    app->app().config().secureInput = contour::config::SecureInputMode::Auto;
    return app;
}

/// Sets what @p session's terminal does with typed text, then lets it print, as a prompt does.
void promptWith(contour::session::TerminalSession& session, vtpty::InputEcho echo)
{
    contour::test::mockPtyOf(session).setInputEcho(echo);
    session.terminal().writeToScreen("x");
    drainPosted();
}

/// @return The lock @p session shows.
[[nodiscard]] contour::window::SecureIndicator indicatorOf(contour::session::TerminalSession const& session)
{
    return static_cast<contour::window::SecureIndicator>(session.secureIndicator());
}

/// @return Whether tab @p row of @p window shows the lock.
[[nodiscard]] int secureRoleAt(contour::window::WindowController& window, int row)
{
    return window
        .data(window.index(row), static_cast<int>(contour::window::WindowController::Roles::SecureRole))
        .toInt();
}

/// Types one character into @p session, as a key press reaching the PTY.
void typeInto(contour::session::TerminalSession& session)
{
    session.sendCharEvent(U'a',
                          vtbackend::KeyIdentity {},
                          vtbackend::KeyboardModifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
}

} // namespace

TEST_CASE("TerminalSessionManager: a raw-mode program shows no lock", "[manager][secureinput]")
{
    // Review Focus 1: an editor or a shell's line editor clears echo too, and must not read as a prompt.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    promptWith(*session, vtpty::InputEcho::Raw);
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);
    CHECK(doubles.record.enables == 0);
}

TEST_CASE("TerminalSessionManager: a focused password prompt turns secure input on, once",
          "[manager][secureinput]")
{
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    promptWith(*session, vtpty::InputEcho::Hidden);
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::LockedSecured);
    CHECK(secureRoleAt(*window, 0) == static_cast<int>(contour::window::SecureIndicator::LockedSecured));
    CHECK(doubles.record.enables == 1);

    promptWith(*session, vtpty::InputEcho::Hidden);
    CHECK(doubles.record.enables == 1);
    CHECK(doubles.record.disables == 0);
}

TEST_CASE("TerminalSessionManager: a daemon's echo report locks the pane with no output after it",
          "[manager][secureinput]")
{
    // A daemon-hosted pane is told its echo state over the native protocol, from the reactor thread; the
    // program may turn echo off after its prompt, so nothing prints after the report. The session reads it
    // on its own thread when told (vtpty::Pty::setInputEchoObserver).
    auto doubles = SecureInputDoubles {};
    auto channels = std::vector<vtpty::ChannelPty*> {};
    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    factory->makePty = [&channels](vtbackend::PageSize size) {
        auto pty = std::make_unique<vtpty::ChannelPty>(size);
        channels.push_back(pty.get());
        return pty;
    };
    auto app = std::make_unique<contour::test::TestApp>(
        std::move(factory),
        nullptr,
        nullptr,
        nullptr,
        contour::test::TestApp::testHostIdentity(),
        nullptr,
        contour::test::TestEnvironment::Overrides {},
        nullptr,
        contour::test::appearanceServicesWith(doubles.accessibility, doubles.appearance, doubles.record));
    app->app().config().secureInput = contour::config::SecureInputMode::Auto;
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    REQUIRE(channels.size() == 1);
    drainPosted();
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);

    channels[0]->setInputEcho(vtpty::InputEcho::Hidden);
    drainPosted();
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::LockedSecured);
    CHECK(secureRoleAt(*window, 0) == static_cast<int>(contour::window::SecureIndicator::LockedSecured));
    CHECK(doubles.record.enables == 1);

    // Told again: no new lock, no new call.
    channels[0]->setInputEcho(vtpty::InputEcho::Hidden);
    drainPosted();
    CHECK(doubles.record.enables == 1);

    channels[0]->setInputEcho(vtpty::InputEcho::Echoed);
    drainPosted();
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);
    CHECK(doubles.record.disables == 1);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSessionManager: closing the prompt's pane, its window, or the manager turns it off",
          "[manager][secureinput]")
{
    // Review Focus 2: no way of the prompt's going away may leave secure input on.
    auto doubles = SecureInputDoubles {};

    SECTION("its pane closes")
    {
        auto app = makeSecureInputApp(doubles);
        auto& manager = app->manager();
        contour::test::ScopedController const window { manager };
        window->createNewTab();
        auto* const survivor = window->activeSession();
        REQUIRE(survivor != nullptr);
        manager.splitActivePane(/*vertical*/ true, survivor);
        manager.setFocusedWindow(window.id);
        auto* const prompt = window->activeSession();
        REQUIRE(prompt != survivor);
        promptWith(*prompt, vtpty::InputEcho::Hidden);
        REQUIRE(doubles.record.enables == 1);

        manager.closeActivePane(prompt, contour::session::SessionEnd::Destroy);
        prompt->onClosed();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(doubles.record.disables == 1);
    }

    SECTION("its window closes")
    {
        auto app = makeSecureInputApp(doubles);
        auto& manager = app->manager();
        contour::test::ScopedController const window { manager };
        window->createNewTab();
        manager.setFocusedWindow(window.id);
        auto* const prompt = window->activeSession();
        REQUIRE(prompt != nullptr);
        promptWith(*prompt, vtpty::InputEcho::Hidden);
        REQUIRE(doubles.record.enables == 1);

        manager.removeWindowController(window.id);
        CHECK(doubles.record.disables == 1);
    }

    SECTION("the manager goes, with the app")
    {
        {
            auto app = makeSecureInputApp(doubles);
            auto& manager = app->manager();
            // Not a ScopedController: the window must still be open when the manager goes, or closing it
            // would be what turned secure input off.
            auto* const window = manager.createWindowController();
            window->createNewTab();
            manager.setFocusedWindow(window->windowId());
            auto* const prompt = window->activeSession();
            REQUIRE(prompt != nullptr);
            promptWith(*prompt, vtpty::InputEcho::Hidden);
            REQUIRE(doubles.record.enables == 1);
            REQUIRE(doubles.record.disables == 0);
        }
        CHECK(doubles.record.disables == 1);
    }
}

TEST_CASE("TerminalSessionManager: the prompt's program ending turns secure input off and the lock away",
          "[manager][secureinput]")
{
    // Review M4: the program dies at its prompt, leaving the terminal's flags as they were; the pane stays
    // (its footer), but nothing is typing a password into it any more.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const prompt = window->activeSession();
    REQUIRE(prompt != nullptr);
    promptWith(*prompt, vtpty::InputEcho::Hidden);
    REQUIRE(doubles.record.enables == 1);
    REQUIRE(secureRoleAt(*window, 0) != 0);

    endEarly(*prompt);
    CHECK(doubles.record.disables == 1);
    CHECK(indicatorOf(*prompt) == contour::window::SecureIndicator::None);
    CHECK(secureRoleAt(*window, 0) == 0);
}

TEST_CASE("TerminalSessionManager: another application in front turns secure input off",
          "[manager][secureinput]")
{
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const prompt = window->activeSession();
    REQUIRE(prompt != nullptr);
    promptWith(*prompt, vtpty::InputEcho::Hidden);
    REQUIRE(doubles.record.enables == 1);

    manager.clearFocusedWindow(window.id);
    CHECK(doubles.record.disables == 1);
    // The lock stays: the prompt still waits.
    CHECK(indicatorOf(*prompt) == contour::window::SecureIndicator::Locked);

    manager.setFocusedWindow(window.id);
    CHECK(doubles.record.enables == 2);
}

TEST_CASE("TerminalSessionManager: only the pane the OS secures claims secure input",
          "[manager][secureinput]")
{
    // Review I3: secure input is process-wide and follows the one focused pane of the front application. A
    // pane at a prompt elsewhere, or in a background application, hides what is typed but is not secured.
    using contour::window::SecureIndicator;
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 1);
    REQUIRE(background != nullptr);
    window->activateTab(0);
    auto* const focused = window->activeSession();
    REQUIRE(focused != nullptr);
    manager.setFocusedWindow(window.id);
    promptWith(*background, vtpty::InputEcho::Hidden);
    promptWith(*focused, vtpty::InputEcho::Hidden);

    CHECK(indicatorOf(*focused) == SecureIndicator::LockedSecured);
    CHECK(indicatorOf(*background) == SecureIndicator::Locked);
    CHECK(secureRoleAt(*window, 0) == static_cast<int>(SecureIndicator::LockedSecured));
    CHECK(secureRoleAt(*window, 1) == static_cast<int>(SecureIndicator::Locked));

    // Focus moves to the other prompt: the claim moves with the OS's secure input.
    window->activateTab(1);
    CHECK(indicatorOf(*background) == SecureIndicator::LockedSecured);
    CHECK(indicatorOf(*focused) == SecureIndicator::Locked);
    CHECK(secureRoleAt(*window, 0) == static_cast<int>(SecureIndicator::Locked));
    CHECK(secureRoleAt(*window, 1) == static_cast<int>(SecureIndicator::LockedSecured));

    // Another application in front: no pane is secured.
    manager.clearFocusedWindow(window.id);
    CHECK(indicatorOf(*background) == SecureIndicator::Locked);
    CHECK(indicatorOf(*focused) == SecureIndicator::Locked);
}

TEST_CASE("TerminalSessionManager: focus between two prompts calls nothing", "[manager][secureinput]")
{
    // Review Focus 3: secure input is process-wide, so moving between two windows that both wait for a
    // password keeps it on without a call.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const first { manager };
    contour::test::ScopedController const second { manager };
    first->createNewTab();
    second->createNewTab();
    auto* const a = first->activeSession();
    auto* const b = second->activeSession();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    promptWith(*a, vtpty::InputEcho::Hidden);
    promptWith(*b, vtpty::InputEcho::Hidden);

    manager.setFocusedWindow(first.id);
    manager.setFocusedWindow(second.id);
    CHECK(doubles.record.enables == 1);
    CHECK(doubles.record.disables == 0);

    promptWith(*b, vtpty::InputEcho::Echoed);
    CHECK(doubles.record.disables == 1);
}

TEST_CASE("TerminalSessionManager: a background prompt locks its tab, not the OS", "[manager][secureinput]")
{
    // Review Focus 4: ssh asks for a passphrase in a tab the user is not typing into.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 1);
    REQUIRE(background != nullptr);
    window->activateTab(0);
    manager.setFocusedWindow(window.id);

    promptWith(*background, vtpty::InputEcho::Hidden);
    CHECK(secureRoleAt(*window, 1) == 1);
    CHECK(secureRoleAt(*window, 0) == 0);
    CHECK(doubles.record.enables == 0);

    window->activateTab(1);
    CHECK(doubles.record.enables == 1);
}

TEST_CASE("TerminalSessionManager: echo restored without output is noticed on the next key",
          "[manager][secureinput]")
{
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    promptWith(*session, vtpty::InputEcho::Hidden);
    REQUIRE(doubles.record.enables == 1);

    contour::test::mockPtyOf(*session).setInputEcho(vtpty::InputEcho::Echoed);
    typeInto(*session);
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);
    CHECK(doubles.record.disables == 1);
}

TEST_CASE("TerminalSessionManager: a key after a silent echo-off finds secure input already on",
          "[manager][secureinput]")
{
    // Review I2: `printf 'Password: '; stty -echo; read pw` turns echo off after its last output. The lock
    // is noticed before the first key is handed on; that key itself has already been seen by the OS, so
    // secure input protects the keys after it.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    auto enablesWhenKeyLeft = -1;
    auto& pty = contour::test::mockPtyOf(*session);
    pty.setWriteObserver([&] { enablesWhenKeyLeft = doubles.record.enables; });
    pty.setInputEcho(vtpty::InputEcho::Hidden); // no output follows
    typeInto(*session);

    CHECK(enablesWhenKeyLeft == 1);
}

TEST_CASE("TerminalSessionManager: a selection paste re-reads the echo and opens the poll window",
          "[manager][secureinput]")
{
    // Review M3: a middle-click paste answers a prompt like a key does, so it takes the same echo re-read
    // before it and the same quiet-window poll after it.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->inputEchoPollArmed());

    contour::test::mockPtyOf(*session).setInputEcho(vtpty::InputEcho::Hidden); // no output follows
    CHECK((*session)(contour::actions::PasteSelection {}));

    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::LockedSecured);
    CHECK(doubles.record.enables == 1);
    CHECK(session->inputEchoPollArmed());
}

TEST_CASE("TerminalSessionManager: a local pane polls its echo only within the window after activity",
          "[manager][secureinput]")
{
    // Review I2: no idle wakeups. The poll is armed by output or a key and stops once the pane has been
    // quiet for InputEchoPollWindow, all on the app's injected clock.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->inputEchoPollArmed());

    promptWith(*session, vtpty::InputEcho::Echoed); // output: the window opens
    CHECK(session->inputEchoPollArmed());

    // Echo goes off with no output and no key; a tick inside the window notices it.
    contour::test::mockPtyOf(*session).setInputEcho(vtpty::InputEcho::Hidden);
    app->steadyClock().advance(contour::window::InputEchoPollInterval);
    session->pollInputEcho();
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::LockedSecured);
    CHECK(doubles.record.enables == 1);
    CHECK(session->inputEchoPollArmed());

    // ... and it going back on, silently, releases secure input at the next tick.
    contour::test::mockPtyOf(*session).setInputEcho(vtpty::InputEcho::Echoed);
    app->steadyClock().advance(contour::window::InputEchoPollInterval);
    session->pollInputEcho();
    CHECK(doubles.record.disables == 1);

    // Quiet for the whole window: the tick that finds it over stops the poll.
    app->steadyClock().advance(contour::window::InputEchoPollWindow);
    session->pollInputEcho();
    CHECK_FALSE(session->inputEchoPollArmed());

    // A key opens it again.
    typeInto(*session);
    CHECK(session->inputEchoPollArmed());
}

TEST_CASE("TerminalSessionManager: a background pane does not poll its echo", "[manager][secureinput]")
{
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab();
    auto* const background = sessionAtRow(manager, *window, 1);
    REQUIRE(background != nullptr);
    window->activateTab(0);
    manager.setFocusedWindow(window.id);

    promptWith(*background, vtpty::InputEcho::Echoed);
    contour::test::mockPtyOf(*background).setInputEcho(vtpty::InputEcho::Hidden);
    background->pollInputEcho();
    CHECK(indicatorOf(*background) == contour::window::SecureIndicator::None);
}

TEST_CASE("TerminalSessionManager: secure_input: off never calls the OS", "[manager][secureinput]")
{
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    app->app().config().secureInput = contour::config::SecureInputMode::Off;
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);

    promptWith(*session, vtpty::InputEcho::Hidden);
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);
    CHECK(secureRoleAt(*window, 0) == 0);
    CHECK(doubles.record.enables == 0);
}

TEST_CASE("TerminalSessionManager: turning secure_input off at runtime releases it", "[manager][secureinput]")
{
    // A reload that turns the setting off must not leave the OS's secure input on until the prompt ends.
    auto doubles = SecureInputDoubles {};
    auto app = makeSecureInputApp(doubles);
    auto& manager = app->manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    manager.setFocusedWindow(window.id);
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    promptWith(*session, vtpty::InputEcho::Hidden);
    REQUIRE(doubles.record.enables == 1);

    app->app().config().secureInput = contour::config::SecureInputMode::Off;
    manager.secureInputModeChanged();
    CHECK(doubles.record.disables == 1);
    CHECK(indicatorOf(*session) == contour::window::SecureIndicator::None);
    CHECK(secureRoleAt(*window, 0) == 0);
}
// }}}

TEST_CASE("TerminalSessionManager: the restore summary waits for a window, and tells one",
          "[manager][notices][restore]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();

    // Nothing to post it in yet.
    manager.noteWorkspaceRestored(vthost::RestoredCounts { .windows = 2, .tabs = 7 });

    contour::test::ScopedController const first { manager };
    contour::test::ScopedController const second { manager };
    CHECK(first->notices().toastText().isEmpty());
    CHECK(second->notices().toastText().isEmpty());

    manager.deliverRestoreSummaryTo(first->windowId());
    CHECK(first->notices().toastText() == "Restored 2 windows, 7 tabs");

    // Told once: a second window shows nothing.
    manager.deliverRestoreSummaryTo(second->windowId());
    CHECK(second->notices().toastText().isEmpty());
}

TEST_CASE("TerminalSessionManager: the restore summary reads in the singular", "[manager][notices][restore]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { app.manager() };
    app.manager().noteWorkspaceRestored(vthost::RestoredCounts { .windows = 1, .tabs = 1 });
    app.manager().deliverRestoreSummaryTo(window->windowId());
    CHECK(window->notices().toastText() == "Restored 1 window, 1 tab");
}

TEST_CASE("TerminalSessionManager: a summary no window of its layout took is not posted to a later window",
          "[manager][notices][restore]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    manager.noteWorkspaceRestored(vthost::RestoredCounts { .windows = 2, .tabs = 7 });
    manager.expireRestoreSummary();

    contour::test::ScopedController const later { manager };
    manager.deliverRestoreSummaryTo(later->windowId());
    CHECK(later->notices().toastText().isEmpty());
}

TEST_CASE("TerminalSessionManager: a newer restore summary replaces one still held",
          "[manager][notices][restore]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    manager.noteWorkspaceRestored(vthost::RestoredCounts { .windows = 5, .tabs = 5 });
    manager.noteWorkspaceRestored(vthost::RestoredCounts { .windows = 2, .tabs = 7 });
    contour::test::ScopedController const window { manager };
    manager.deliverRestoreSummaryTo(window->windowId());
    CHECK(window->notices().toastText() == "Restored 2 windows, 7 tabs");
}

TEST_CASE("TerminalSessionManager: a preview session is in no window and a reload leaves it alone",
          "[manager][settingspreview]")
{
    contour::test::ConfiguredTestApp testApp;
    contour::test::ScopedController const win { testApp.manager() };
    auto* window = testApp.manager().model().window(win.id);
    REQUIRE(window != nullptr);
    auto const tabsBefore = window->tabCount();

    auto previewed = testApp.app().config();
    auto const previewedSize = text::FontSize { 31.0 };
    previewed.profile().fonts.value().size = previewedSize;
    auto const preview = testApp.manager().createPreviewSession(previewed, testApp.app().profileName());

    REQUIRE(preview != nullptr);
    CHECK(preview->role() == contour::session::SessionRole::Preview);
    CHECK(preview->profile().fonts.value().size.pt == previewedSize.pt);
    CHECK(window->tabCount() == tabsBefore);
    CHECK(testApp.manager().sessionForId(preview->modelSessionId()) == nullptr);

    // The reload's loop over the manager's sessions does not reach it: the owner applies what it previews.
    contour::test::writeConfigFile(
        testApp.configFile(), contour::test::profileConfigYaml(testApp.app().profileName(), "theme: dark\n"));
    REQUIRE(testApp.manager().reloadAllSessions());
    CHECK(preview->profile().fonts.value().size.pt == previewedSize.pt);
    CHECK(window->tabCount() == tabsBefore);
}

TEST_CASE("TerminalSessionManager: starting a preview session spawns no process and opens no pty",
          "[manager][settingspreview]")
{
    contour::test::ConfiguredTestApp testApp;
    auto const preview =
        testApp.manager().createPreviewSession(testApp.app().config(), testApp.app().profileName());
    REQUIRE(preview != nullptr);
    // The device is the mock the session was built with, not one a factory made.
    auto* const mock = dynamic_cast<vtpty::MockPty*>(&preview->terminal().device());
    REQUIRE(mock != nullptr);
    REQUIRE_FALSE(mock->isClosed());

    // What a display that shows the preview does (TerminalDisplay::setSession).
    preview->start();

    // Still the mock, with no program on it: no process was spawned, and nothing in its foreground.
    CHECK(dynamic_cast<vtpty::MockPty*>(&preview->terminal().device()) == mock);
    CHECK_FALSE(mock->foregroundProcessName().has_value());
    // Destroyed with its threads started: the end path runs without a real device to close.
}

// {{{ mergeTabIntoSplit

TEST_CASE("TerminalSessionManager: a window's only tab merged into another window's pane closes the source",
          "[manager][merge]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const windowA { manager };
    contour::test::ScopedController const windowB { manager };
    windowA->createNewTab(); // A: [a0]
    windowB->createNewTab(); // B: [b0]
    auto* const movedSession = windowA->activeSession();
    auto* const targetSession = windowB->activeSession();
    REQUIRE(movedSession != nullptr);
    REQUIRE(targetSession != nullptr);
    auto const movedId = movedSession->modelSessionId();
    auto const targetId = targetSession->modelSessionId();
    auto* const targetTab = manager.model().window(windowB.id)->activeTab();
    REQUIRE(targetTab != nullptr);
    auto const targetTabId = targetTab->id();
    auto const targetLeaf = targetTab->rootPane()->id();
    auto const ptyCountBefore = factory->createdPtys.size();

    manager.mergeTabIntoSplit(
        windowA.id, 0, windowB.id, targetTabId, targetLeaf, vtworkspace::SplitSide::Right);
    QCoreApplication::processEvents();

    // The source window held only that tab: it is gone, model and controller both.
    CHECK(manager.model().window(windowA.id) == nullptr);
    CHECK(manager.controllerFor(windowA.id) == nullptr);

    // Both sessions live in B's tab, which has one pane more; nothing was re-created.
    CHECK(factory->createdPtys.size() == ptyCountBefore);
    CHECK(manager.sessionForId(movedId) == movedSession);
    CHECK(manager.sessionForId(targetId) == targetSession);
    auto* const merged = manager.model().findTab(targetTabId);
    REQUIRE(merged != nullptr);
    CHECK(merged->paneCount() == 2);
    CHECK(std::get<1>(manager.model().findSessionLeaf(movedId)) == merged);
    CHECK(std::get<1>(manager.model().findSessionLeaf(targetId)) == merged);

    // B's window renders both: the proxy tree now has a split whose halves carry the two sessions, the
    // grafted one on the right and in focus.
    auto* const root = windowB->activeTabRootPane();
    REQUIRE(root != nullptr);
    REQUIRE_FALSE(root->isLeaf());
    CHECK(root->orientation() == static_cast<int>(vtworkspace::SplitState::Vertical));
    REQUIRE(root->first() != nullptr);
    REQUIRE(root->second() != nullptr);
    CHECK(root->first()->session() == targetSession);
    CHECK(root->second()->session() == movedSession);
    CHECK(windowB->activeSession() == movedSession);

    // The session index follows the graft: a rename aimed at the moved session lands on its new tab.
    manager.setTabTitleForSession(movedId, "merged");
    REQUIRE(merged->runtimeTitle().has_value());
    CHECK(*merged->runtimeTitle() == "merged");

    windowB->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager: a background tab merged into the active tab's pane splits it",
          "[manager][merge]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const backgroundSession = window->activeSession();
    window->createNewTab(); // [t0, t1], t1 active
    auto* const activeSession = window->activeSession();
    REQUIRE(backgroundSession != nullptr);
    REQUIRE(activeSession != nullptr);
    REQUIRE(backgroundSession != activeSession);
    auto* const activeTab = manager.model().window(window.id)->activeTab();
    REQUIRE(activeTab != nullptr);
    auto const activeTabId = activeTab->id();
    auto const leaf = activeTab->activePane()->id();

    manager.mergeTabIntoSplit(window.id, 0, window.id, activeTabId, leaf, vtworkspace::SplitSide::Bottom);

    CHECK(window->count() == 1);
    auto* const merged = manager.model().window(window.id)->activeTab();
    REQUIRE(merged != nullptr);
    CHECK(merged->id() == activeTabId);
    CHECK(merged->rootPane()->splitState() == vtworkspace::SplitState::Horizontal);
    CHECK(merged->rootPane()->first()->session() == activeSession->modelSessionId());
    CHECK(merged->rootPane()->second()->session() == backgroundSession->modelSessionId());
    CHECK(window->activeSession() == backgroundSession);

    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager: a refused merge leaves every session where it was", "[manager][merge]")
{
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab(); // [t0, t1]
    auto* const model = &manager.model();
    auto* const first = model->window(window.id)->tabAt(0);
    auto* const second = model->window(window.id)->tabAt(1);
    auto const firstId = first->id();
    auto const secondId = second->id();
    auto const firstSession = first->rootPane()->session();
    auto const unknownLeaf = vtworkspace::PaneId { 9999 };

    SECTION("an unknown pane")
    {
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, secondId, unknownLeaf, vtworkspace::SplitSide::Left);
    }

    SECTION("a tab onto its own pane")
    {
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, firstId, first->rootPane()->id(), vtworkspace::SplitSide::Left);
    }

    SECTION("a row that is no tab")
    {
        manager.mergeTabIntoSplit(
            window.id, 7, window.id, secondId, second->rootPane()->id(), vtworkspace::SplitSide::Left);
    }

    CHECK(window->count() == 2);
    CHECK(first->paneCount() == 1);
    CHECK(second->paneCount() == 1);
    // The session index still names the tab the session is in, so focus and routing keep working.
    CHECK(std::get<1>(model->findSessionLeaf(firstSession)) == first);
    CHECK(manager.sessionForId(firstSession) != nullptr);

    window->closeTabAtIndex(0);
    window->closeTabAtIndex(0);
}

TEST_CASE("TerminalSessionManager: a merge on a daemon-hosted window is asked of the daemon, not made here",
          "[manager][merge]")
{
    using contour::session::TabMergeSupport;
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    auto& manager = app.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    window->createNewTab(); // [t0 (dragged), t1]
    auto* const model = &manager.model();
    auto* const dragged = model->window(window.id)->tabAt(0);
    auto* const target = model->window(window.id)->tabAt(1);
    auto const draggedSession = dragged->rootPane()->session();
    auto const targetSession = target->rootPane()->session();
    factory->reportedHosting = contour::session::SessionHosting::Remote;

    SECTION("a daemon that serves the merge is asked, with both panes' ptys and the side")
    {
        factory->reportedTabMergeSupport = TabMergeSupport::Supported;
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, target->id(), target->rootPane()->id(), vtworkspace::SplitSide::Top);

        REQUIRE(factory->requestedTabMerges.size() == 1);
        auto const& request = factory->requestedTabMerges.front();
        CHECK(
            request.tabPtys
            == std::vector<vtpty::Pty const*> { &manager.sessionForId(draggedSession)->terminal().device() });
        CHECK(request.targetPty == &manager.sessionForId(targetSession)->terminal().device());
        CHECK(request.side == vtworkspace::SplitSide::Top);
        // The daemon's layout re-push is what changes the local tree: nothing moved here.
        CHECK(window->count() == 2);
        CHECK(target->paneCount() == 1);
        CHECK(std::get<1>(model->findSessionLeaf(draggedSession)) == dragged);
    }

    SECTION("a request the factory does not route, as the reconciler's own, is merged here")
    {
        factory->reportedTabMergeSupport = TabMergeSupport::Unsupported;
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, target->id(), target->rootPane()->id(), vtworkspace::SplitSide::Top);

        CHECK(window->count() == 1);
        CHECK(target->paneCount() == 2);
        CHECK(std::get<1>(model->findSessionLeaf(draggedSession)) == target);
    }

    SECTION("a tab of several panes names every one of them, so the daemon can spot a stale view")
    {
        factory->reportedTabMergeSupport = TabMergeSupport::Supported;
        auto* const draggedFirst = manager.sessionForId(draggedSession);
        manager.splitActivePane(/*vertical*/ true, draggedFirst);
        REQUIRE(dragged->paneCount() == 2);
        factory->requestedTabMerges.clear();
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, target->id(), target->rootPane()->id(), vtworkspace::SplitSide::Left);

        REQUIRE(factory->requestedTabMerges.size() == 1);
        auto const expected = [&] {
            auto ptys = std::vector<vtpty::Pty const*> {};
            for (auto const* const session: manager.sessionsOfTab(dragged))
                ptys.push_back(&session->terminal().device());
            return ptys;
        }();
        CHECK(expected.size() == 2);
        CHECK(factory->requestedTabMerges.front().tabPtys == expected);
    }

    SECTION("a daemon that cannot take it now refuses it: nothing is merged here, and the window says so")
    {
        // No binding yet, or the capability lost between hover and release: the answer is Refused.
        factory->reportedTabMergeSupport = TabMergeSupport::Supported;
        factory->reportedTabMergeRouting = contour::session::RemoteRouting::Refused;
        manager.mergeTabIntoSplit(
            window.id, 0, window.id, target->id(), target->rootPane()->id(), vtworkspace::SplitSide::Top);

        CHECK(factory->requestedTabMerges.size() == 1);
        CHECK(window->count() == 2); // no local merge, so this window never diverges from the daemon's
        CHECK(target->paneCount() == 1);
        CHECK(std::get<1>(model->findSessionLeaf(draggedSession)) == dragged);
        REQUIRE(window->notices().toastShown());
        CHECK(window->notices().toastText().toStdString()
              == std::string { contour::window::TabMergeRefusedText });
    }

    SECTION("a daemon's merge stays within one window")
    {
        factory->reportedTabMergeSupport = TabMergeSupport::Supported;
        CHECK(manager.tabMergeSupport(window.id, window.id) == TabMergeSupport::Supported);
        CHECK(manager.tabMergeSupport(window.id, vtworkspace::WindowId { window.id.value + 1 })
              == TabMergeSupport::Unsupported);
        factory->reportedHosting = contour::session::SessionHosting::Local;
        CHECK(manager.tabMergeSupport(window.id, vtworkspace::WindowId { window.id.value + 1 })
              == TabMergeSupport::Supported);
        factory->reportedHosting = contour::session::SessionHosting::Remote;
    }

    factory->reportedHosting = contour::session::SessionHosting::Local;
    window->closeTabAtIndex(0);
    window->closeTabAtIndex(0);
}

// }}}
