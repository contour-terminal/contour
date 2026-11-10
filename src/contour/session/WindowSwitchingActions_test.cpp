// SPDX-License-Identifier: Apache-2.0
//
// The daemon and workspace switching actions as a window meets them: a bound action reaches the injected
// WindowSwitching with the right target and mode, a cycle steps through what the SwitchDirectory lists, and
// a window that runs its sessions in-process does nothing and says so in the log.

#include <contour/config/Actions.hpp>
#include <contour/session/FactoryBinding.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/WindowController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using contour::actions::CycleDirection;
using contour::actions::DaemonRef;
using contour::actions::DaemonRefKind;
using contour::session::SwitchMode;
using contour::session::SwitchTarget;
using contour::session::TerminalSession;
using contour::session::WorkspaceRole;
using contour::test::DaemonNamedBinding;
using contour::test::MockPtySessionFactory;
using contour::test::ScopedController;
using contour::test::TestApp;
using vthost::proto::SwitchDaemonKind;

namespace
{

SwitchTarget labelTarget(std::string name)
{
    return { .kind = SwitchDaemonKind::Label, .daemon = std::move(name), .workspace = {} };
}

SwitchTarget hostTarget(std::string name)
{
    return { .kind = SwitchDaemonKind::Host, .daemon = std::move(name), .workspace = {} };
}

/// A window with one session, bound to a daemon named @p daemon, or to none (in-process).
struct SwitchRig
{
    MockPtySessionFactory* factory = nullptr;
    TestApp app;
    DaemonNamedBinding binding;
    ScopedController window;
    TerminalSession* session = nullptr;

    explicit SwitchRig(std::optional<SwitchTarget> const& daemon):
        app { [this] {
            auto made = std::make_unique<MockPtySessionFactory>();
            factory = made.get();
            return made;
        }() },
        binding { *factory, daemon.value_or(SwitchTarget {}) },
        window { app.manager() }
    {
        if (daemon)
        {
            app.manager().bindWindow(window.id, binding);
            // Binding to a daemon warms the discovery only: no listing, so no connection to a saved host.
            REQUIRE(app.switchDirectory().discoveryRefreshes == 1);
            REQUIRE(app.switchDirectory().refreshes == 0);
        }
        window->createNewTab();
        auto* const modelWindow = app.manager().model().workspace(window.id);
        REQUIRE(modelWindow != nullptr);
        auto* const tab = modelWindow->tabAt(window->activeTabIndex());
        REQUIRE(tab != nullptr);
        session = app.manager().sessionForId(tab->activePane()->session());
        REQUIRE(session != nullptr);
    }

    /// Runs @p action in the session.
    void run(contour::actions::Action const& action) const { REQUIRE(session->executeAction(action)); }

    [[nodiscard]] auto& requests() { return app.switching().requests; }
};

} // namespace

TEST_CASE("switching actions reach the window switcher with their target and mode", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    using namespace contour::actions;

    rig.run(SwitchToDaemon { DaemonRef { DaemonRefKind::Label, "admin" } });
    rig.run(OpenDaemon { DaemonRef { DaemonRefKind::Host, "studio" } });
    rig.run(SwitchToWorkspace { 2 });
    rig.run(OpenWorkspace { 3 });

    REQUIRE(rig.requests().size() == 4);
    CHECK(rig.requests()[0].window == rig.window.id);
    CHECK(rig.requests()[0].target == labelTarget("admin"));
    CHECK(rig.requests()[0].mode == SwitchMode::Switch);
    CHECK(rig.requests()[1].target == hostTarget("studio"));
    CHECK(rig.requests()[1].mode == SwitchMode::Open);
    // The same daemon, named by a 1-based position the daemon resolves.
    CHECK(rig.requests()[2].target
          == SwitchTarget { .kind = SwitchDaemonKind::Same,
                            .daemon = {},
                            .workspace = contour::session::WorkspaceAt { .position = 2 } });
    CHECK(rig.requests()[2].mode == SwitchMode::Switch);
    CHECK(rig.requests()[3].target.workspace
          == contour::session::WorkspaceTarget { contour::session::WorkspaceAt { 3 } });
    CHECK(rig.requests()[3].mode == SwitchMode::Open);
}

TEST_CASE("a failed switch is logged and leaves nothing pending", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switching().failWith = "switching is not available yet";
    rig.run(contour::actions::SwitchToDaemon { DaemonRef { DaemonRefKind::Label, "admin" } });
    CHECK(rig.requests().size() == 1);
}

TEST_CASE("CycleDaemon steps through local labels, then shown hosts", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switchDirectory().daemons = { { .label = "default", .socket = {}, .workspaces = std::nullopt },
                                          { .label = "admin", .socket = {}, .workspaces = std::nullopt } };
    rig.app.switchDirectory().hosts = { "studio" };

    rig.run(contour::actions::CycleDaemon { CycleDirection::Next });
    rig.run(contour::actions::CycleDaemon { CycleDirection::Previous });

    REQUIRE(rig.requests().size() == 2);
    CHECK(rig.requests()[0].target == hostTarget("studio")); // admin, default, studio
    CHECK(rig.requests()[1].target == labelTarget("admin"));
    CHECK(rig.requests()[0].mode == SwitchMode::Switch);
    CHECK(rig.app.switchDirectory().refreshes == 2); // each press renews the cache for the next
}

TEST_CASE("CycleDaemon with no other daemon switches nothing", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switchDirectory().daemons = { { .label = "default", .socket = {}, .workspaces = std::nullopt } };
    rig.run(contour::actions::CycleDaemon {});
    CHECK(rig.requests().empty());
}

TEST_CASE("CycleWorkspace steps through the workspaces of the window's daemon, wrapping", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switchDirectory().workspaces = { { .position = 1, .name = "a", .role = WorkspaceRole::Other },
                                             { .position = 2, .name = "b", .role = WorkspaceRole::Current },
                                             { .position = 3, .name = "c", .role = WorkspaceRole::Other } };

    rig.run(contour::actions::CycleWorkspace { CycleDirection::Next });
    rig.app.switchDirectory().workspaces[1].role = WorkspaceRole::Other;
    rig.app.switchDirectory().workspaces[0].role = WorkspaceRole::Current;
    rig.run(contour::actions::CycleWorkspace { CycleDirection::Previous });

    REQUIRE(rig.requests().size() == 2);
    auto const third = contour::session::WorkspaceTarget { contour::session::WorkspaceAt { 3 } };
    CHECK(rig.requests()[0].target.workspace == third);
    CHECK(rig.requests()[1].target.workspace == third); // before 1 wraps to 3
    CHECK(rig.requests()[0].target.kind == SwitchDaemonKind::Same);
    CHECK(rig.app.switchDirectory().refreshes == 2); // each press renews the cache for the next
}

TEST_CASE("CycleWorkspace with a single workspace switches nothing", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switchDirectory().workspaces = { { .position = 1, .name = "a", .role = WorkspaceRole::Current } };
    rig.run(contour::actions::CycleWorkspace {});
    CHECK(rig.requests().empty());
}

TEST_CASE("ShowSwitcher refreshes what the switcher lists", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.run(contour::actions::ShowSwitcher {});
    CHECK(rig.app.switchDirectory().refreshes == 1);
}

TEST_CASE("the menu bar state lists the other daemons from the caches, and a menu opening refreshes them",
          "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.app.switchDirectory().daemons = { { .label = "default", .socket = {}, .workspaces = std::nullopt },
                                          { .label = "admin", .socket = {}, .workspaces = std::nullopt } };

    auto const state = rig.window->menuBarState();

    CHECK(state.switching == contour::command::DaemonSwitching::Available);
    // admin alone: the window's own daemon is not one to switch to, and the open rows are not these.
    REQUIRE(state.switchToDaemon.size() == 1);
    CHECK(std::holds_alternative<contour::actions::SwitchToDaemon>(state.switchToDaemon[0].action));
    // Reading the state is a focus change's cost: no daemon I/O.
    CHECK(rig.app.switchDirectory().refreshes == 0);

    rig.window->refreshSwitching();
    CHECK(rig.app.switchDirectory().refreshes == 1);
}

TEST_CASE("a tab with no title is named after the shell on this machine's daemon, the host elsewhere",
          "[switch][session][tablabel]")
{
    auto const titleOf = [](SwitchRig& rig) {
        return rig.window
            ->data(rig.window->index(0),
                   static_cast<int>(contour::window::WindowController::Roles::TitleRole))
            .toString();
    };

    SECTION("a daemon on this machine")
    {
        auto rig = SwitchRig { labelTarget("default") };
        REQUIRE(rig.session->resolvedWindowTitle().empty());
        auto const shell = QString::fromStdString(rig.session->shellProgramName());
        REQUIRE_FALSE(shell.isEmpty());
        CHECK(titleOf(rig) == shell);
    }

    SECTION("a saved host")
    {
        auto rig = SwitchRig { hostTarget("studio") };
        rig.binding.runElsewhere();
        REQUIRE(rig.session->resolvedWindowTitle().empty());
        CHECK(titleOf(rig) == QStringLiteral("studio"));
    }

    SECTION(
        "a `--connect-tcp` daemon on another machine has no name to switch by, so the chrome's label does")
    {
        auto rig =
            SwitchRig { SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} } };
        rig.binding.runElsewhere();
        rig.binding.renameTo(SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} },
                             "far-away");
        REQUIRE(rig.session->resolvedWindowTitle().empty());
        CHECK(titleOf(rig) == QStringLiteral("far-away"));
    }
}

TEST_CASE("the host chip's showSwitcher runs ShowSwitcher in the active session", "[switch][session]")
{
    auto rig = SwitchRig { labelTarget("default") };
    rig.window->showSwitcher();
    CHECK(rig.app.switchDirectory().refreshes == 1); // what ShowSwitcher does first
}

TEST_CASE("a window with no daemon has no switching rows and refreshes nothing", "[switch][session]")
{
    auto rig = SwitchRig { std::nullopt };
    rig.app.switchDirectory().daemons = { { .label = "admin", .socket = {}, .workspaces = std::nullopt } };

    auto const state = rig.window->menuBarState();
    CHECK(state.switching == contour::command::DaemonSwitching::Unavailable);
    CHECK(state.switchToDaemon.empty());
    rig.window->refreshSwitching();
    CHECK(rig.app.switchDirectory().refreshes == 0);
}

TEST_CASE("every switching action is a no-op for a window whose sessions run in-process", "[switch][session]")
{
    auto rig = SwitchRig { std::nullopt };
    rig.app.switchDirectory().workspaces = { { .position = 1, .name = "a", .role = WorkspaceRole::Current },
                                             { .position = 2, .name = "b", .role = WorkspaceRole::Other } };
    rig.app.switchDirectory().daemons = { { .label = "admin", .socket = {}, .workspaces = std::nullopt } };
    using namespace contour::actions;

    rig.run(SwitchToDaemon { DaemonRef { DaemonRefKind::Label, "admin" } });
    rig.run(OpenDaemon { DaemonRef { DaemonRefKind::Label, "admin" } });
    rig.run(SwitchToWorkspace { 2 });
    rig.run(OpenWorkspace { 2 });
    rig.run(CycleWorkspace {});
    rig.run(CycleDaemon {});
    rig.run(ShowSwitcher {});
    rig.run(NewDaemon {});

    CHECK(rig.requests().empty());
    CHECK(rig.app.switchDirectory().refreshes == 0);
}

// Review (PR 3, item 3): a tmux window is bound to a factory, which has no daemon a switch could reach; it
// was offered every switching row and action all the same, each ending in "no daemon behind it".
TEST_CASE("every switching action is a no-op for a window bound to a tmux session's factory",
          "[switch][session]")
{
    auto rig = SwitchRig { std::nullopt };
    auto tmux = contour::session::FactoryBinding { *rig.factory, "tmux-host" };
    rig.app.manager().bindWindow(rig.window.id, tmux);
    rig.app.switchDirectory().workspaces = { { .position = 1, .name = "a", .role = WorkspaceRole::Current },
                                             { .position = 2, .name = "b", .role = WorkspaceRole::Other } };
    rig.app.switchDirectory().daemons = { { .label = "admin", .socket = {}, .workspaces = std::nullopt } };
    using namespace contour::actions;

    CHECK_FALSE(rig.app.manager().daemonTargetOf(rig.window.id).has_value());
    CHECK(rig.window->switchingAvailability() == contour::command::DaemonSwitching::Unavailable);
    CHECK(rig.window->daemonRows().switching == contour::command::DaemonSwitching::Unavailable);

    rig.run(SwitchToDaemon { DaemonRef { DaemonRefKind::Label, "admin" } });
    rig.run(SwitchToWorkspace { 2 });
    rig.run(CycleWorkspace {});
    rig.run(CycleDaemon {});
    rig.run(ShowSwitcher {});
    rig.run(NewDaemon {});

    CHECK(rig.requests().empty());
    CHECK(rig.app.switchDirectory().refreshes == 0);
    rig.app.manager().releaseBinding(tmux);
}

TEST_CASE("UpgradeDaemon tells a window with no daemon why nothing happens", "[switch][session]")
{
    using namespace contour::actions;
    SECTION("sessions in-process")
    {
        auto rig = SwitchRig { std::nullopt };
        rig.run(UpgradeDaemon {});
        CHECK(rig.window->notices().toastText()
              == QStringLiteral("Could not upgrade the daemon: this window has no daemon to upgrade."));
    }
    SECTION("a binding with no daemon behind it")
    {
        auto rig = SwitchRig { std::nullopt };
        auto tmux = contour::session::FactoryBinding { *rig.factory, "tmux-host" };
        rig.app.manager().bindWindow(rig.window.id, tmux);
        rig.run(UpgradeDaemon {});
        CHECK(rig.window->notices().toastText()
              == QStringLiteral("Could not upgrade the daemon: this window has no daemon to upgrade."));
        rig.app.manager().releaseBinding(tmux);
    }
}

TEST_CASE("a window says which daemon it is on, and stops when it moves to the default one",
          "[switch][chrome]")
{
    auto factory = MockPtySessionFactory {};
    auto app = TestApp { [&] {
        return std::make_unique<MockPtySessionFactory>();
    }() };
    // Declared before the window, which tells its binding of its close.
    auto onAdmin = DaemonNamedBinding { factory, labelTarget("admin"), "admin" };
    auto onDefault = DaemonNamedBinding { factory, labelTarget("default"), "" };
    auto onHost = DaemonNamedBinding { factory, hostTarget("studio"), "studio" };
    auto const window = ScopedController { app.manager() };

    SECTION("a non-default local daemon: the title carries its label and the chip is shown")
    {
        app.manager().bindWindow(window.id, onAdmin);
        CHECK(window->hostLabel() == "admin");
        CHECK(window->windowTitle("vim") == "admin — vim");
        CHECK(contour::window::hostChipPresence(app.manager().chromeLabelOf(window.id))
              == contour::window::HostChipPresence::Shown);
    }

    SECTION("the default daemon: nothing extra")
    {
        app.manager().bindWindow(window.id, onDefault);
        CHECK(window->hostLabel().isEmpty());
        CHECK(window->windowTitle("vim") == "vim");
    }

    SECTION("a saved host keeps saying so")
    {
        app.manager().bindWindow(window.id, onHost);
        CHECK(window->windowTitle("vim") == "studio — vim");
    }

    SECTION("switching from admin to the default daemon takes the label away and says so")
    {
        app.manager().bindWindow(window.id, onAdmin);
        auto changed = QSignalSpy { window.controller, &contour::window::WindowController::hostLabelChanged };
        app.manager().bindWindow(window.id, onDefault);
        CHECK(changed.count() >= 1);
        CHECK(window->hostLabel().isEmpty());
        CHECK(window->windowTitle("vim") == "vim");
    }
}
