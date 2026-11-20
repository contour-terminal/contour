// SPDX-License-Identifier: Apache-2.0
//
// The agent panes daemon-scoped extensions open (IOwnedPanes), on a SessionHost whose sessions run on devices
// that never end by themselves: where a pane opens, that it takes no focus, what it runs, when it is listed,
// whose it is, what status it shows, and what its registry file holds.

#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/DirectoryHandle.hpp>

#include <core/Environment.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/OwnedPaneStore.hpp>
#include <vthost/OwnedPanes.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/ServedTesting.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vthost::testing::FakeWindow;
using vtworkspace::SessionId;

namespace
{

constexpr auto Orchestrator = std::string_view { "orchestrator" };
constexpr auto Other = std::string_view { "other" };

/// How a bench's host spawns, as far as held directories go.
struct BenchSandbox
{
    /// Whether a spawn under a policy can enter a held directory; empty: under every one.
    std::function<vtpty::HeldDirectorySpawn(SandboxEscape)> rule {};
    SandboxEscape ptySandbox = SandboxEscape::Escapes; ///< The host's own policy.
    /// The default profile's policy, its sessions spawned through the profile factory; nullopt: no profiles.
    std::optional<SandboxEscape> profileSandbox {};
};

/// @return A rule under which only a contained spawn enters a held directory, as in a Flatpak.
[[nodiscard]] std::function<vtpty::HeldDirectorySpawn(SandboxEscape)> flatpakRule()
{
    return [](SandboxEscape escape) {
        return escape == SandboxEscape::Contained ? vtpty::HeldDirectorySpawn::Supported
                                                  : vtpty::HeldDirectorySpawn::Unsupported;
    };
}

/// A host whose sessions pump on devices that never end by themselves, recording what each spawn was asked to
/// run; the spawn of a program named `fails` fails. A fake daemon world journals the hints it is raised.
struct PanesBench
{
    explicit PanesBench(
        std::shared_ptr<IOwnedPaneStore> registry = std::make_shared<InMemoryOwnedPaneStore>(),
        BenchSandbox sandboxes = {}):
        store { std::move(registry) },
        sandbox { std::move(sandboxes) },
        host { loop,
               [this](vtbackend::PageSize size,
                      std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                   return spawnDevice(size, commandOverride);
               },
               vtbackend::Settings {},
               core::defaultEnvironment(),
               config() }
    {
        auto* const tab = host.createTab();
        REQUIRE(tab != nullptr);
        shellTab = tab->id();
        shell = tab->rootPane()->session();
        first = host.workspaceId();
        // The registry is reconciled once the constructor's posted job ran.
        REQUIRE(eventually([this] { return panes().list(Orchestrator).has_value(); }));
        // Asked before that, the extension hears Panes once reconciled: that hint is the bench's, not a
        // test's.
        REQUIRE(eventually(
            [this] { return std::ranges::contains(*journal, std::format("hint {} panes", Orchestrator)); }));
        journal->clear();
    }

    PanesBench(PanesBench const&) = delete;
    PanesBench(PanesBench&&) = delete;
    PanesBench& operator=(PanesBench const&) = delete;
    PanesBench& operator=(PanesBench&&) = delete;
    ~PanesBench() = default;

    [[nodiscard]] SessionHostConfig config()
    {
        auto config = SessionHostConfig {
            .pumping = SessionPumping::Threaded,
            .daemonWorld =
                [this](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring) {
                    world = std::make_shared<vthost::testing::FakeDaemonWorldHosting>(
                        journal, std::move(post), std::move(events));
                    world->rows = { ExtensionActionRows { .extension = std::string(Orchestrator),
                                                          .title = "Agents",
                                                          .state = DaemonExtensionState::Running,
                                                          .rows = { { "open", "Open Agents" } } } };
                    tabs = &wiring.tabs;
                    return world;
                },
            .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>(),
            .servedIds = std::make_shared<vthost::testing::ServedUuids>(),
            .ownedPaneStore = store,
        };
        if (sandbox.profileSandbox)
        {
            config.profiles = profiles();
            config.profilePtys = [this](vtbackend::PageSize size,
                                        vtpty::Process::ExecInfo const& shell,
                                        SandboxEscape /*escape*/) {
                return spawnDevice(size, shell);
            };
        }
        // Whose typing a reveal may stand on, and for how long, is judged by this clock.
        config.clocks = vtbackend::TerminalClocks { .steady = clock,
                                                    .wall = core::platform::defaultSystemWallClock(),
                                                    .localTime = &vtbackend::systemLocalTime };
        config.ptySandbox = sandbox.ptySandbox;
        config.heldDirectorySpawn = sandbox.rule;
        // Only a program named `not-installed` is missing.
        config.programFound = [](vtpty::Process::ExecInfo const& exec) {
            return exec.program != "not-installed";
        };
        return config;
    }

    [[nodiscard]] OwnedPanes& panes() { return host.ownedPanes(); }

    /// Records a spawn and makes its device; none for a program named `fails`. Tells onSpawn first.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> spawnDevice(vtbackend::PageSize size,
                                                          std::optional<vtpty::Process::ExecInfo> const& exec)
    {
        if (onSpawn && exec)
            onSpawn(*exec);
        auto const lock = std::lock_guard { spawnsMutex };
        spawns.push_back(exec);
        if (exec && exec->program == "fails")
            return nullptr;
        auto device = std::make_unique<vtpty::ChannelPty>(size);
        devices.push_back(device.get());
        return device;
    }

    /// @return A catalog whose one profile, the default, runs under the bench's profile policy.
    [[nodiscard]] std::shared_ptr<ProfileCatalog const> profiles() const
    {
        auto profile = vthost::testing::fakeProfile("/bin/sh", vtbackend::LineCount(100));
        profile.sandbox = *sandbox.profileSandbox;
        return std::make_shared<vthost::testing::FakeProfileCatalog>(
            std::map<std::string, ResolvedProfile> { { "main", std::move(profile) } }, "main");
    }

    /// @return A spawn of @p extension's agent, tagged @p tag, near @p nearTab, running @p program.
    [[nodiscard]] OwnedPaneSpawn spawn(std::string_view extension = Orchestrator,
                                       std::string_view tag = "run-1",
                                       std::optional<std::string> nearTab = std::nullopt,
                                       std::string program = "claude") const
    {
        return OwnedPaneSpawn {
            .extension = std::string(extension),
            .argv = { std::move(program), "--model", "opus" },
            .environment = { { "GIT_CONFIG_COUNT", "2" } },
            .removedEnvironment = { "GIT_DIR", "GH_REPO" },
            .directory = directory,
            .cwd = directory->resolvedPath(),
            .title = "run-1",
            .tag = std::string(tag),
            .nearTab = std::move(nearTab),
        };
    }

    /// Opens @p spawn and turns the loop until its creation ran.
    /// @return The pane's id.
    [[nodiscard]] std::string openNow(OwnedPaneSpawn spawn)
    {
        auto const extension = spawn.extension;
        auto opened = panes().open(std::move(spawn));
        REQUIRE(opened.has_value());
        REQUIRE(eventually([&] { return stateOf(extension, *opened) != OwnedPaneState::Pending; }));
        return *opened;
    }

    /// @return The listing of @p pane of @p extension; nullopt when it is not listed.
    [[nodiscard]] std::optional<OwnedPaneInfo> infoOf(std::string_view extension, std::string const& pane)
    {
        auto const listed = panes().list(extension);
        REQUIRE(listed.has_value());
        auto const found = std::ranges::find(*listed, pane, &OwnedPaneInfo::pane);
        return found != listed->end() ? std::optional { *found } : std::nullopt;
    }

    /// @return Where @p pane of @p extension stands; nullopt when it is not listed.
    [[nodiscard]] std::optional<OwnedPaneState> stateOf(std::string_view extension, std::string const& pane)
    {
        return infoOf(extension, pane).transform([](OwnedPaneInfo const& info) { return info.state; });
    }

    /// @return The session of the pane @p pane.
    [[nodiscard]] std::optional<SessionId> sessionOf(std::string const& pane) const
    {
        for (auto const session: host.sessionIds())
            if (auto const uuid = host.objects().uuidOf(session); uuid && vtcontrol::toString(*uuid) == pane)
                return session;
        return std::nullopt;
    }

    /// @return The workspace @p session's tab is in.
    [[nodiscard]] vtworkspace::WorkspaceId workspaceOf(SessionId session) const
    {
        auto const [workspace, tab, leaf] = host.model().findSessionLeaf(session);
        REQUIRE(workspace != nullptr);
        return workspace->id();
    }

    /// @return The tab @p session runs in.
    [[nodiscard]] vtworkspace::TabId tabOf(SessionId session) const
    {
        auto const [workspace, tab, leaf] = host.model().findSessionLeaf(session);
        REQUIRE(tab != nullptr);
        return tab->id();
    }

    /// @return The active tab of @p workspace.
    [[nodiscard]] vtworkspace::TabId activeTabIn(vtworkspace::WorkspaceId workspace) const
    {
        auto const* const found = host.model().workspace(workspace);
        REQUIRE(found != nullptr);
        REQUIRE(found->activeTab() != nullptr);
        return found->activeTab()->id();
    }

    /// Attaches @p window selecting @p workspace, focused, and mints its origin as invoking an action does.
    [[nodiscard]] Origin attach(FakeWindow& window, vtworkspace::WorkspaceId workspace)
    {
        window.selected = workspace.value;
        host.subscribeStream(&window);
        host.noteClientAttached(&window);
        host.noteFocus(window, proto::WindowFocus::Focused);
        REQUIRE(host.invokeExtensionAction(window.connectionId(), Orchestrator, "open")
                == InvocationEffect::Deliver);
        auto const taken = host.extensionActions().take(Orchestrator);
        REQUIRE(taken.size() == 1);
        return taken.front().second;
    }

    void detach(FakeWindow& window)
    {
        host.noteClientDetached(&window, DetachKind::Closed);
        host.forgetActionOrigin(window.connectionId());
        host.unsubscribeStream(&window);
    }

    /// @return How many times @p extension was raised ServedHint::Panes.
    [[nodiscard]] std::size_t paneHints(std::string_view extension = Orchestrator) const
    {
        return static_cast<std::size_t>(
            std::ranges::count(*journal, std::format("hint {} panes", extension)));
    }

    /// Turns the loop until @p condition holds, for at most ten seconds.
    /// @return Whether it held.
    [[nodiscard]] bool eventually(std::function<bool()> const& condition)
    {
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::ignore = loop.runOnce(10ms);
        }
        return true;
    }

    /// Turns the loop a few times, for what it has queued.
    void settle()
    {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
            std::ignore = loop.runOnce(1ms);
    }

    std::shared_ptr<IOwnedPaneStore> store;
    BenchSandbox sandbox;
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<vthost::testing::FakeDaemonWorldHosting> world;
    IServedTabs* tabs = nullptr;
    std::mutex spawnsMutex;
    std::vector<std::optional<vtpty::Process::ExecInfo>> spawns; ///< Every spawn's override, in order.
    std::vector<vtpty::ChannelPty*> devices;                     ///< Every session's device, in spawn order.
    /// Called on the loop as a session is spawned, before its device is made.
    std::function<void(vtpty::Process::ExecInfo const&)> onSpawn {};
    std::shared_ptr<vtpty::DirectoryHandle const> directory = std::make_shared<vtpty::DirectoryHandle const>(
        vtpty::DirectoryHandle::unbacked(std::filesystem::temp_directory_path().string()));
    core::platform::ManualClock clock; ///< The host's sessions' steady clock.
    core::net::PlatformLoop loop;
    SessionHost host;
    LoopDrain drain { loop }; ///< Declared after the host: destroyed first, as LoopDrain asks.
    vtworkspace::TabId shellTab {};
    SessionId shell {};
    vtworkspace::WorkspaceId first {};
};

/// A registry whose saves wait until released, as a disk that does not answer: its records reach the store
/// behind it only then.
struct BlockingStore final: IOwnedPaneStore
{
    [[nodiscard]] std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> loadAll() const override
    {
        return records.loadAll();
    }

    [[nodiscard]] std::expected<void, OwnedPaneStoreError> save(
        std::string_view extension, std::span<OwnedPaneRecord const> saved) override
    {
        {
            auto lock = std::unique_lock { mutex };
            ++waiting;
            changed.notify_all();
            changed.wait(lock, [this] { return released; });
        }
        return records.save(extension, saved);
    }

    void release()
    {
        {
            auto const lock = std::scoped_lock { mutex };
            released = true;
        }
        changed.notify_all();
    }

    /// @return Whether a save is waiting for the disk, or has.
    [[nodiscard]] bool entered()
    {
        auto const lock = std::scoped_lock { mutex };
        return waiting > 0;
    }

    /// @return The records saved for @p extension.
    [[nodiscard]] std::vector<OwnedPaneRecord> savedOf(std::string_view extension) const
    {
        for (auto const& [name, saved]: records.loadAll())
            if (name == extension)
                return saved;
        return {};
    }

    InMemoryOwnedPaneStore records;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t waiting = 0;
    bool released = false;
};

} // namespace

TEST_CASE("an agent pane opened by pane-open leaves the previous tab active", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto* const second = b.host.createTab(); // active: second; previous: the shell's
    REQUIRE(second != nullptr);
    auto const secondTab = second->id();
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    CHECK(b.workspaceOf(*session) == b.first);
    auto const* const workspace = b.host.model().workspace(b.first);
    REQUIRE(workspace != nullptr);
    CHECK(workspace->activeTab()->id() == secondTab);
    // The agent's tab was never active: "previous tab" still goes to the user's own previous one.
    REQUIRE(workspace->previousActiveTab() != nullptr);
    CHECK(workspace->previousActiveTab()->id() == b.shellTab);
}

TEST_CASE("pane-open opens near its dashboard", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto const secondId = second->id();
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, secondId);
    auto const dashboard = b.tabs->open(Orchestrator, "Agents", "", "agents", origin);
    REQUIRE(dashboard.has_value());
    // The window then looks at the first workspace: the pane still goes beside its dashboard.
    window.selected = b.first.value;
    b.host.noteSelected(window);

    auto const pane = b.openNow(b.spawn(Orchestrator, "run-1", *dashboard));
    CHECK(b.workspaceOf(*b.sessionOf(pane)) == secondId);
    b.detach(window);
}

TEST_CASE("pane-open near a closed dashboard opens in the focused workspace", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto const secondId = second->id();
    auto window = FakeWindow { 1 };
    std::ignore = b.attach(window, secondId);

    auto const pane = b.openNow(b.spawn(Orchestrator, "run-1", std::string { "no-such-tab" }));
    CHECK(b.workspaceOf(*b.sessionOf(pane)) == secondId);
    b.detach(window);

    // With no window at all: the host's first workspace.
    auto const alone = b.openNow(b.spawn(Orchestrator, "run-2", std::string { "no-such-tab" }));
    CHECK(b.workspaceOf(*b.sessionOf(alone)) == b.first);
}

TEST_CASE("pane-open near another extension's tab ignores it", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, second->id());
    auto const dashboard = b.tabs->open(Orchestrator, "Agents", "", "agents", origin);
    REQUIRE(dashboard.has_value());
    window.selected = b.first.value;
    b.host.noteSelected(window);

    // Another extension naming the orchestrator's dashboard opens where the focused window looks.
    auto const pane = b.openNow(b.spawn(Other, "run-1", *dashboard));
    CHECK(b.workspaceOf(*b.sessionOf(pane)) == b.first);
    b.detach(window);
}

TEST_CASE("the agent is the pane's program", "[vthost][panes]")
{
    // What SessionHost hands the PTY factory; the daemon's applies it with ShellOverride::Overlays, the
    // program replacing the shell's (ShellPtyFactory_test pins what the child then sees).
    auto b = PanesBench {};
    std::ignore = b.openNow(b.spawn());
    auto const lock = std::lock_guard { b.spawnsMutex };
    REQUIRE(b.spawns.size() == 2);
    auto const& spawned = b.spawns.back();
    REQUIRE(spawned.has_value());
    CHECK(spawned->program == "claude");
    CHECK(spawned->arguments == std::vector<std::string> { "--model", "opus" });
    CHECK(spawned->env.at("GIT_CONFIG_COUNT") == "2");
    CHECK(spawned->env.contains("CONTOUR_PANE"));
    CHECK(spawned->removedEnvironment == std::vector<std::string> { "GIT_DIR", "GH_REPO" });
    CHECK(spawned->directory == b.directory);
    CHECK(spawned->workingDirectory == b.directory->resolvedPath());
}

TEST_CASE("a pending pane is listed", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const opened = b.panes().open(b.spawn());
    REQUIRE(opened.has_value());
    // Before the loop turns: the creation is still posted.
    auto const listed = b.panes().list(Orchestrator);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK(listed->front()
          == OwnedPaneInfo { .pane = *opened,
                             .tag = "run-1",
                             .status = std::nullopt,
                             .program = PaneLiveness::Alive,
                             .state = OwnedPaneState::Pending });
    REQUIRE(b.eventually([&] { return b.stateOf(Orchestrator, *opened) == OwnedPaneState::Open; }));
}

TEST_CASE("closing a pending pane cancels its creation", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const opened = b.panes().open(b.spawn());
    REQUIRE(opened.has_value());
    REQUIRE(b.panes().close(Orchestrator, *opened).has_value());
    CHECK(b.stateOf(Orchestrator, *opened) == OwnedPaneState::Cancelled);

    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, *opened).has_value(); }));
    auto const lock = std::lock_guard { b.spawnsMutex };
    CHECK(b.spawns.size() == 1); // the shell's only: the agent was never spawned
    CHECK_FALSE(b.sessionOf(*opened).has_value());
}

TEST_CASE("a pane stays listed until its session ended", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    b.journal->clear();

    REQUIRE(b.panes().close(Orchestrator, pane).has_value());
    // Still listed, closing, until the loop ended its session.
    CHECK(b.stateOf(Orchestrator, pane) == OwnedPaneState::Closing);
    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, pane).has_value(); }));
    CHECK_FALSE(b.host.hostedSession(*session) != nullptr);
    CHECK(b.paneHints() == 1);
}

namespace
{

/// Captures the registry as a live upgrade does (SessionHost::flushOwnedPanes()) as an agent's session begins
/// to end, while it is still hosted.
class UpgradeCaptureOnClose final: public SessionStreamEvents
{
  public:
    UpgradeCaptureOnClose(SessionHost& host, IOwnedPaneStore const& store, SessionId shell):
        _host { host }, _store { store }, _shell { shell }
    {
    }

    void sessionClosing(SessionId session) override
    {
        if (session == _shell || captured)
            return;
        _host.flushOwnedPanes();
        captured = std::vector<OwnedPaneRecord> {};
        for (auto const& [extension, records]: _store.loadAll())
            if (extension == Orchestrator)
                captured = records;
    }

    std::optional<std::vector<OwnedPaneRecord>> captured; ///< The registry of Orchestrator, once captured.

  private:
    SessionHost& _host;
    IOwnedPaneStore const& _store;
    SessionId _shell;
};

} // namespace

TEST_CASE("a pane closed while it was created is carried by an upgrade until its session ended",
          "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const opened = b.panes().open(b.spawn());
    REQUIRE(opened.has_value());
    // Closed as its session is spawned: created, but cancelled, so its session ends at once.
    b.onSpawn = [&](vtpty::Process::ExecInfo const&) {
        CHECK(b.panes().close(Orchestrator, *opened).has_value());
    };
    auto capture = UpgradeCaptureOnClose { b.host, *b.store, b.shell };
    b.host.subscribeStream(&capture);
    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, *opened).has_value(); }));
    b.host.unsubscribeStream(&capture);
    b.onSpawn = {};

    // Captured while it ended, it was in the registry: the next image takes it for the agent pane it is.
    REQUIRE(capture.captured.has_value());
    REQUIRE(capture.captured->size() == 1);
    CHECK(vtcontrol::toString(capture.captured->front().pane) == *opened);
    // Its session ended, it is gone from the registry too.
    b.host.flushOwnedPanes();
    for (auto const& [extension, records]: b.store->loadAll())
        CHECK((extension != Orchestrator || records.empty()));
}

TEST_CASE("a failed creation removes the pane and raises Panes", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const opened = b.panes().open(b.spawn(Orchestrator, "run-1", std::nullopt, "fails"));
    REQUIRE(opened.has_value());
    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, *opened).has_value(); }));
    REQUIRE(b.eventually([&] { return b.paneHints() == 1; }));
    CHECK(b.activeTabIn(b.first) == b.shellTab);
}

TEST_CASE("an extension cannot close or list a pane it did not open", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn());

    CHECK(b.panes().close(Other, pane)
          == std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::NotOwned });
    auto const shellPane = b.host.objects().uuidOf(b.shell);
    REQUIRE(shellPane.has_value());
    CHECK(b.panes().close(Orchestrator, vtcontrol::toString(*shellPane))
          == std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::NotOwned });
    CHECK(b.panes().close(Orchestrator, "not-a-uuid")
          == std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::NotOwned });
    CHECK(b.panes().list(Other)
          == std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> { std::vector<OwnedPaneInfo> {} });
    b.settle();
    CHECK(b.sessionOf(pane).has_value());
    CHECK(b.host.hostedSession(b.shell) != nullptr);
}

// {{{ pane-reveal

TEST_CASE("pane-reveal in the origin's workspace activates", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, b.first);
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    REQUIRE(b.activeTabIn(b.first) == b.shellTab);

    REQUIRE(b.panes().reveal(Orchestrator, pane, origin).has_value());

    REQUIRE(b.eventually([&] { return window.reveals == std::vector { *session }; }));
    CHECK(b.activeTabIn(b.first) == b.tabOf(*session));
    CHECK(window.switches.empty());
    b.detach(window);
}

TEST_CASE("pane-reveal tells the origin's window, not the one focused last", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto origins = FakeWindow { 1 };
    auto focused = FakeWindow { 2 };
    auto const origin = b.attach(origins, b.first);
    std::ignore = b.attach(focused, b.first); // focused last
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());

    REQUIRE(b.panes().reveal(Orchestrator, pane, origin).has_value());

    REQUIRE(b.eventually([&] { return origins.reveals == std::vector { *session }; }));
    b.settle();
    CHECK(focused.reveals.empty());
    CHECK(focused.switches.empty());
    b.detach(focused);
    b.detach(origins);
}

TEST_CASE("pane-reveal with no origin, just after a key typed into its dashboard, goes to that window",
          "[vthost][panes]")
{
    auto b = PanesBench {};
    auto typing = FakeWindow { 1 };
    auto focused = FakeWindow { 2 };
    std::ignore = b.attach(typing, b.first);
    auto const origin = b.attach(focused, b.first); // focused last
    auto const dashboard = b.tabs->open(Orchestrator, "Agents", "", "agents", origin);
    REQUIRE(dashboard.has_value());
    REQUIRE(b.eventually([&] { return b.sessionOf(*dashboard).has_value(); }));
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    auto const gone = std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::Gone };

    // A guest restarted since: its dashboard knows no origin, but the Enter that asked was typed in it.
    b.host.writeInputFrom(*b.sessionOf(*dashboard), "\r", typing.connectionId(), proto::InputKind::Keyboard);
    REQUIRE(b.panes().reveal(Orchestrator, pane, Origin { 0 }).has_value());

    REQUIRE(b.eventually([&] { return typing.reveals == std::vector { *session }; }));
    CHECK(focused.reveals.empty());

    // Past the grace, no key stands behind it any more.
    b.clock.advance(std::chrono::seconds { 3 });
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 }) == gone);

    // A key typed into the orchestrator's dashboard asks nothing of another extension.
    auto const others = b.openNow(b.spawn(Other));
    b.host.writeInputFrom(*b.sessionOf(*dashboard), "\r", typing.connectionId(), proto::InputKind::Keyboard);
    CHECK(b.panes().reveal(Other, others, Origin { 0 }) == gone);
    // Nor does what a window sends by itself, such as a focus report.
    b.clock.advance(std::chrono::seconds { 3 });
    b.host.writeInputFrom(*b.sessionOf(*dashboard), "\x1b[I", typing.connectionId(), proto::InputKind::Other);
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 }) == gone);

    b.settle();
    CHECK(typing.reveals.size() == 1);
    CHECK(focused.reveals.empty());
    b.detach(focused);
    b.detach(typing);
}

TEST_CASE("pane-reveal with no origin, just after a click into its dashboard, goes to that window",
          "[vthost][panes]")
{
    auto b = PanesBench {};
    auto clicking = FakeWindow { 1 };
    auto focused = FakeWindow { 2 };
    std::ignore = b.attach(clicking, b.first);
    auto const origin = b.attach(focused, b.first); // focused last
    auto const dashboard = b.tabs->open(Orchestrator, "Agents", "", "agents", origin);
    REQUIRE(dashboard.has_value());
    REQUIRE(b.eventually([&] { return b.sessionOf(*dashboard).has_value(); }));
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    auto const tab = *b.sessionOf(*dashboard);
    auto const gone = std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::Gone };

    // What the window sends as the pointer merely moves, scrolls or the tab gains focus asks for nothing.
    b.host.writeInputFrom(tab, "\x1b[<35;4;2M", clicking.connectionId(), proto::InputKind::Other);
    b.host.writeInputFrom(tab, "\x1b[<64;4;2M", clicking.connectionId(), proto::InputKind::Other);
    b.host.writeInputFrom(tab, "\x1b[I", clicking.connectionId(), proto::InputKind::Other);
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 }) == gone);

    // A button the user pressed on the agent's row does.
    b.host.writeInputFrom(tab, "\x1b[<0;4;2M", clicking.connectionId(), proto::InputKind::ButtonPress);
    REQUIRE(b.panes().reveal(Orchestrator, pane, Origin { 0 }).has_value());
    REQUIRE(b.eventually([&] { return clicking.reveals == std::vector { *session }; }));

    // For as long as a key would.
    b.clock.advance(std::chrono::seconds { 3 });
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 }) == gone);

    b.settle();
    CHECK(clicking.reveals.size() == 1);
    CHECK(focused.reveals.empty());
    b.detach(focused);
    b.detach(clicking);
}

TEST_CASE("pane-reveal with no origin raises no window", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto window = FakeWindow { 1 };
    std::ignore = b.attach(window, b.first);
    auto const pane = b.openNow(b.spawn());

    // No user action behind it: refused, though a window is focused.
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 })
          == std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::Gone });

    b.settle();
    CHECK(window.reveals.empty());
    CHECK(window.switches.empty());
    CHECK(b.activeTabIn(b.first) == b.shellTab);
    b.detach(window);
}

TEST_CASE("pane-reveal into another workspace asks, and refused changes nothing", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn()); // in the first workspace: no window looks anywhere yet
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto const secondId = second->id();
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, secondId);

    REQUIRE(b.panes().reveal(Orchestrator, pane, origin).has_value());

    REQUIRE(b.eventually([&] { return window.switches.size() == 1; }));
    auto const request = window.switches.front();
    // The workspace being left is the window's own; the target names the pane's workspace.
    CHECK(request.workspace == secondId.value);
    CHECK(request.target.workspace == vtcontrol::toString(*b.host.objects().uuidOf(b.first)));
    CHECK(b.activeTabIn(b.first) == b.shellTab);

    auto const answer = [&](proto::SwitchOutcome outcome) {
        b.host.completeSwitch(window.connectionId(),
                              proto::SwitchReply { .request = request.request, .outcome = outcome });
        b.settle();
    };
    SECTION("refused: nothing is activated, and the window is not told")
    {
        answer(proto::SwitchOutcome::Refused);
        CHECK(b.activeTabIn(b.first) == b.shellTab);
        CHECK(window.reveals.empty());
    }
    SECTION("switched: activated, and the window is told")
    {
        answer(proto::SwitchOutcome::Switched);
        CHECK(b.activeTabIn(b.first) == b.tabOf(*session));
        CHECK(window.reveals == std::vector { *session });
    }
    b.detach(window);
}

TEST_CASE("pane-reveal with a detached origin reveals in the most recently focused window", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto const secondId = second->id();
    auto gone = FakeWindow { 1 };
    auto stays = FakeWindow { 2 };
    auto const origin = b.attach(gone, b.first);
    std::ignore = b.attach(stays, secondId); // focused last
    b.detach(gone);

    REQUIRE(b.panes().reveal(Orchestrator, pane, origin).has_value());

    // The window focused last is asked, as the origin's would have been: it shows another workspace.
    REQUIRE(b.eventually([&] { return stays.switches.size() == 1; }));
    CHECK(stays.switches.front().workspace == secondId.value);
    b.host.completeSwitch(stays.connectionId(),
                          proto::SwitchReply { .request = stays.switches.front().request,
                                               .outcome = proto::SwitchOutcome::Switched });
    b.settle();
    CHECK(stays.reveals == std::vector { *session });
    CHECK(b.activeTabIn(b.first) == b.tabOf(*session));
    CHECK(gone.switches.empty());
    CHECK(gone.reveals.empty());
    b.detach(stays);
}

TEST_CASE("pane-reveal with no window attached activates in place", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn());
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    REQUIRE(b.activeTabIn(b.first) == b.shellTab);
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, b.first);
    b.detach(window);

    // The origin's window went, and no other is attached: none to tell; the pane's own workspace shows it.
    REQUIRE(b.panes().reveal(Orchestrator, pane, origin).has_value());

    REQUIRE(b.eventually([&] { return b.activeTabIn(b.first) == b.tabOf(*session); }));
}

TEST_CASE("pane-reveal of a pending pane reveals it once created", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, b.first);
    auto const opened = b.panes().open(b.spawn());
    REQUIRE(opened.has_value());

    REQUIRE(b.panes().reveal(Orchestrator, *opened, origin).has_value());

    REQUIRE(b.eventually([&] { return window.reveals.size() == 1; }));
    CHECK(window.reveals.front() == b.sessionOf(*opened));
    b.detach(window);
}

TEST_CASE("pane-reveal of a pane it does not own is refused", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto window = FakeWindow { 1 };
    auto const origin = b.attach(window, b.first);
    auto const pane = b.openNow(b.spawn());
    auto const notOwned = std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::NotOwned };

    CHECK(b.panes().reveal(Other, pane, origin) == notOwned);
    auto const shellPane = b.host.objects().uuidOf(b.shell);
    REQUIRE(shellPane.has_value());
    CHECK(b.panes().reveal(Orchestrator, vtcontrol::toString(*shellPane), origin) == notOwned);
    CHECK(b.panes().reveal(Orchestrator, "not-a-uuid", origin) == notOwned);
    b.settle();
    CHECK(window.reveals.empty());
    CHECK(window.switches.empty());
    CHECK(b.activeTabIn(b.first) == b.shellTab);
    b.detach(window);
}

TEST_CASE("pane-reveal of a closed pane answers gone", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const gone = std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::Gone };

    // Closed while open: listed until its session ended, and gone to a reveal meanwhile.
    auto const pane = b.openNow(b.spawn());
    REQUIRE(b.panes().close(Orchestrator, pane).has_value());
    CHECK(b.panes().reveal(Orchestrator, pane, Origin { 0 }) == gone);

    // Closed while pending: never created.
    auto const pending = b.panes().open(b.spawn(Orchestrator, "run-2"));
    REQUIRE(pending.has_value());
    REQUIRE(b.panes().close(Orchestrator, *pending).has_value());
    CHECK(b.panes().reveal(Orchestrator, *pending, Origin { 0 }) == gone);

    REQUIRE(b.eventually([&] { return b.panes().list(Orchestrator)->empty(); }));
    CHECK(b.activeTabIn(b.first) == b.shellTab);
}

TEST_CASE("an origin handed to another extension reveals nothing", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn(Other)); // in the first workspace
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    auto* const second = b.host.createWorkspace();
    REQUIRE(second != nullptr);
    auto orchestrators = FakeWindow { 1 };
    auto focused = FakeWindow { 2 };
    // Handed to the orchestrator, for a window that shows another workspace.
    auto const foreign = b.attach(orchestrators, second->id());
    std::ignore = b.attach(focused, b.first); // focused last

    // Never handed to it: no action of its user is behind it, so neither window is raised -- not the
    // orchestrator's, nor the one focused last.
    CHECK(b.panes().reveal(Other, pane, foreign)
          == std::expected<void, OwnedPaneError> { std::unexpect, OwnedPaneError::Gone });

    b.settle();
    CHECK(focused.reveals.empty());
    CHECK(orchestrators.switches.empty());
    CHECK(orchestrators.reveals.empty());
    CHECK(b.activeTabIn(b.first) == b.shellTab);
    b.detach(focused);
    b.detach(orchestrators);
}

// }}}

TEST_CASE("the pane's status follows its named records", "[vthost][panes]")
{
    using vtbackend::ProgramState;
    using vtbackend::ProgramStatusOrigin;
    auto const record =
        [](std::string id, ProgramState state, ProgramStatusOrigin origin, std::chrono::seconds since) {
            auto made = vtbackend::ProgramStatusRecord {};
            made.id = std::move(id);
            made.state = state;
            made.origin = origin;
            made.since = std::chrono::system_clock::time_point { since };
            return made;
        };
    struct Row
    {
        std::string_view what;
        std::vector<vtbackend::ProgramStatusRecord> records;
        std::optional<ProgramState> status;
    };
    auto const row = GENERATE_COPY(
        Row { .what = "the hooks' blocked over the root's working",
              .records = { record("", ProgramState::Working, ProgramStatusOrigin::Program, 20s),
                           record(
                               "claude-code", ProgramState::Blocked, ProgramStatusOrigin::Integration, 10s) },
              .status = ProgramState::Blocked },
        Row { .what = "the root's done alone",
              .records = { record("", ProgramState::Done, ProgramStatusOrigin::Program, 10s) },
              .status = ProgramState::Done },
        Row { .what = "a stale hooks record beats a fresh root record",
              .records = { record("claude-code", ProgramState::Idle, ProgramStatusOrigin::Integration, 1s),
                           record("", ProgramState::Working, ProgramStatusOrigin::Program, 900s) },
              .status = ProgramState::Idle },
        Row { .what = "an unrelated record only",
              .records = { record("build/test", ProgramState::Error, ProgramStatusOrigin::Program, 10s) },
              .status = std::nullopt });
    INFO(row.what);
    CHECK(paneStatusOf(row.records) == row.status);
}

TEST_CASE("each status change of a pane raises Panes once", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const pane = b.openNow(b.spawn());
    REQUIRE(b.devices.size() == 2);
    auto* const device = b.devices.back();
    REQUIRE(b.eventually([&] { return b.paneHints() >= 1; })); // its creation's
    b.journal->clear();

    device->feed("\033]7501;state=working\033\\");
    REQUIRE(b.eventually(
        [&] { return b.infoOf(Orchestrator, pane)->status == vtbackend::ProgramState::Working; }));
    REQUIRE(b.eventually([&] { return b.paneHints() == 1; }));
    b.settle();
    CHECK(b.paneHints() == 1);

    // The hooks' record wins over the root's, whatever came later.
    device->feed("\033]7501;state=blocked:id=claude-code\033\\\033]7501;state=done\033\\");
    REQUIRE(b.eventually(
        [&] { return b.infoOf(Orchestrator, pane)->status == vtbackend::ProgramState::Blocked; }));
    REQUIRE(b.eventually([&] { return b.paneHints() == 2; }));
    b.settle();
    CHECK(b.paneHints() == 2);

    // A record no row names changes nothing: once the terminal holds it, the pane's status is as it was.
    device->feed("\033]7501;state=error:id=lint\033\\");
    auto const session = b.sessionOf(pane);
    REQUIRE(session.has_value());
    REQUIRE(b.eventually([&] {
        auto const& terminal = std::as_const(*b.host.terminal(*session));
        auto const guard = std::lock_guard { terminal };
        return std::ranges::contains(
            terminal.programStatus().records(), "lint", &vtbackend::ProgramStatusRecord::id);
    }));
    b.settle();
    CHECK(b.infoOf(Orchestrator, pane)->status == vtbackend::ProgramState::Blocked);
    CHECK(b.paneHints() == 2);
}

TEST_CASE("pane-open is refused while an upgrade runs", "[vthost][panes]")
{
    auto b = PanesBench {};
    b.host.closeServedSessions();
    CHECK(b.panes().open(b.spawn())
          == std::expected<std::string, OwnedPaneError> { std::unexpect, OwnedPaneError::Upgrading });
    b.host.admitServedSessions();
    CHECK(b.panes().open(b.spawn()).has_value());
}

TEST_CASE("pane-open accepted before an upgrade's quiesce creates nothing during it", "[vthost][panes]")
{
    auto b = PanesBench {};
    auto const opened = b.panes().open(b.spawn());
    REQUIRE(opened.has_value());
    // The upgrade closes the sessions before the loop ran the creation.
    b.host.closeServedSessions();
    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, *opened).has_value(); }));
    CHECK_FALSE(b.sessionOf(*opened).has_value());
    CHECK(b.host.model().workspace(b.first)->tabCount() == 1);
    REQUIRE(b.eventually([&] { return b.paneHints() == 1; }));
    b.host.admitServedSessions();
}

TEST_CASE("pane-open is refused before any pane exists where no held directory can be entered",
          "[vthost][panes]")
{
    auto const scratch = core::testing::ScopedTempDir { "contour-panes" };
    auto const worktree = scratch.path() / "worktree";
    std::filesystem::create_directory(worktree);
    auto opened = vtpty::nativeDirectoryOpener().open(worktree.string());
    REQUIRE(opened.has_value());
    auto const held = std::make_shared<vtpty::DirectoryHandle const>(std::move(*opened));

    auto const store = std::make_shared<InMemoryOwnedPaneStore>();
    auto refusal = OwnedPaneError {};
    auto sandbox = BenchSandbox {};
    SECTION("the program runs through flatpak-spawn, or on Windows")
    {
        sandbox.rule = [](SandboxEscape) {
            return vtpty::HeldDirectorySpawn::Unsupported;
        };
        refusal = OwnedPaneError::NotSupported;
    }
    SECTION("the worktree was removed and made again since it was opened")
    {
        std::filesystem::remove(worktree);
        std::filesystem::create_directory(worktree);
        refusal = OwnedPaneError::DirectoryGone;
    }
    auto b = PanesBench { store, sandbox };
    auto spawn = b.spawn();
    spawn.directory = held;
    spawn.cwd = held->resolvedPath();

    auto const answer = b.panes().open(std::move(spawn));
    CHECK(answer == std::expected<std::string, OwnedPaneError> { std::unexpect, refusal });
    b.settle();
    CHECK(b.panes().list(Orchestrator)->empty());
    CHECK(b.host.model().workspace(b.first)->tabCount() == 1);
    CHECK(b.spawns.size() == 1); // the shell's
    b.host.flushOwnedPanes();
    CHECK(std::ranges::all_of(store->loadAll(), [](auto const& file) { return file.second.empty(); }));
}

TEST_CASE("pane-open decides held directories by the policy the agent spawns under", "[vthost][panes]")
{
    struct Row
    {
        BenchSandbox sandbox;
        bool opens;
    };
    // The default profile's policy wins over the host's where profiles spawn, as the spawn itself takes it.
    auto const row =
        GENERATE(Row { .sandbox = BenchSandbox { .ptySandbox = SandboxEscape::Contained }, .opens = true },
                 Row { .sandbox = BenchSandbox { .ptySandbox = SandboxEscape::Escapes }, .opens = false },
                 Row { .sandbox = BenchSandbox { .ptySandbox = SandboxEscape::Escapes,
                                                 .profileSandbox = SandboxEscape::Contained },
                       .opens = true },
                 Row { .sandbox = BenchSandbox { .ptySandbox = SandboxEscape::Contained,
                                                 .profileSandbox = SandboxEscape::Escapes },
                       .opens = false });
    auto sandbox = row.sandbox;
    sandbox.rule = flatpakRule();
    auto b = PanesBench { std::make_shared<InMemoryOwnedPaneStore>(), sandbox };
    auto const opened = b.panes().open(b.spawn());
    if (row.opens)
    {
        REQUIRE(opened.has_value());
        REQUIRE(b.eventually([&] { return b.stateOf(Orchestrator, *opened) == OwnedPaneState::Open; }));
    }
    else
        CHECK(opened
              == std::expected<std::string, OwnedPaneError> { std::unexpect, OwnedPaneError::NotSupported });
}

TEST_CASE("pane-open follows a profile reload's policy", "[vthost][panes]")
{
    auto sandbox =
        BenchSandbox { .ptySandbox = SandboxEscape::Contained, .profileSandbox = SandboxEscape::Contained };
    sandbox.rule = flatpakRule();
    auto b = PanesBench { std::make_shared<InMemoryOwnedPaneStore>(), sandbox };
    REQUIRE(b.panes().open(b.spawn()).has_value());

    b.sandbox.profileSandbox = SandboxEscape::Escapes;
    b.host.replaceProfiles(b.profiles());
    CHECK(b.panes().open(b.spawn(Orchestrator, "run-2"))
          == std::expected<std::string, OwnedPaneError> { std::unexpect, OwnedPaneError::NotSupported });
}

TEST_CASE("the registry is written off the loop", "[vthost][panes]")
{
    auto const store = std::make_shared<BlockingStore>();
    auto b = PanesBench { store };

    // The loop creates the panes while the disk does not answer.
    auto const first = b.openNow(b.spawn(Orchestrator, "run-1"));
    REQUIRE(b.eventually([&] { return store->entered(); }));
    auto const second = b.openNow(b.spawn(Orchestrator, "run-2"));
    CHECK(b.stateOf(Orchestrator, second) == OwnedPaneState::Open);
    CHECK(store->savedOf(Orchestrator).empty());

    // Once it answers, the newest records are what it holds.
    store->release();
    b.host.flushOwnedPanes();
    CHECK(store->savedOf(Orchestrator)
          == std::vector { OwnedPaneRecord { .pane = *vtcontrol::parseUuid(first), .tag = "run-1" },
                           OwnedPaneRecord { .pane = *vtcontrol::parseUuid(second), .tag = "run-2" } });
}

TEST_CASE("a registry file past the bound is cut at it as it loads", "[vthost][panes]")
{
    auto records = std::vector<OwnedPaneRecord> {};
    // Panes no host names, unlike the sequential ids the host's objects take.
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxOwnedPanesPerExtension + 3))
        records.push_back(
            OwnedPaneRecord { .pane = vtcontrol::Uuid { std::array<uint8_t, 16> {
                                  0xAB, 0xCD, static_cast<uint8_t>(index), 0, 0, 0, 0x40, 0, 0x80 } },
                              .tag = std::format("run-{}", index) });
    auto const store = std::make_shared<InMemoryOwnedPaneStore>(
        std::map<std::string, std::vector<OwnedPaneRecord>, std::less<>> {
            { std::string(Orchestrator), records } });

    // Before the loop turns, the reconciliation has not run: what is owned is what was loaded.
    auto loop = core::net::PlatformLoop {};
    auto host = std::make_unique<SessionHost>(
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&)
            -> std::unique_ptr<vtpty::Pty> { return std::make_unique<vtpty::ChannelPty>(size); },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        SessionHostConfig { .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>(),
                            .ownedPaneStore = store });
    auto const drain = LoopDrain { loop }; // after the host: destroyed first, as LoopDrain asks
    CHECK(host->ownedPanes().ownsPane(records[MaxOwnedPanesPerExtension - 1].pane));
    CHECK_FALSE(host->ownedPanes().ownsPane(records[MaxOwnedPanesPerExtension].pane));
}

TEST_CASE("a pane-open posted before the host stops creates nothing as it stops", "[vthost][panes]")
{
    auto loop = core::net::PlatformLoop {};
    auto const agents = std::make_shared<std::vector<std::string>>(); // every agent spawned
    auto const journal = std::make_shared<std::vector<std::string>>();
    auto host = std::make_unique<SessionHost>(
        loop,
        [agents](vtbackend::PageSize size,
                 std::optional<vtpty::Process::ExecInfo> const& exec) -> std::unique_ptr<vtpty::Pty> {
            if (exec && exec->program == "claude")
                agents->push_back(exec->program);
            return std::make_unique<vtpty::ChannelPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        SessionHostConfig {
            .daemonWorld =
                [journal](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring /*wiring*/) {
                    return std::make_shared<vthost::testing::FakeDaemonWorldHosting>(
                        journal, std::move(post), std::move(events));
                },
            .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>(),
            .ownedPaneStore = std::make_shared<InMemoryOwnedPaneStore>() });
    auto const drain = LoopDrain { loop }; // after the host: destroyed first, as LoopDrain asks
    REQUIRE(host->createTab() != nullptr);
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!host->ownedPanes().list(Orchestrator).has_value() && std::chrono::steady_clock::now() < deadline)
        std::ignore = loop.runOnce(10ms);
    REQUIRE(host->ownedPanes().list(Orchestrator).has_value());

    auto const directory = std::make_shared<vtpty::DirectoryHandle const>(
        vtpty::DirectoryHandle::unbacked(std::filesystem::temp_directory_path().string()));
    // Accepted, its creation posted to the loop -- which turns next in the host's own teardown.
    REQUIRE(host->ownedPanes()
                .open(OwnedPaneSpawn { .extension = std::string(Orchestrator),
                                       .argv = { "claude" },
                                       .directory = directory,
                                       .cwd = directory->resolvedPath(),
                                       .title = "run-1",
                                       .tag = "run-1" })
                .has_value());
    host.reset();
    CHECK(agents->empty());
}

TEST_CASE("pane-open is refused past the most panes an extension may have", "[vthost][panes]")
{
    auto b = PanesBench {};
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxOwnedPanesPerExtension))
        REQUIRE(b.panes().open(b.spawn(Orchestrator, std::format("run-{}", index))).has_value());
    CHECK(b.panes().open(b.spawn())
          == std::expected<std::string, OwnedPaneError> { std::unexpect, OwnedPaneError::TooMany });
    // Another extension's count is its own.
    CHECK(b.panes().open(b.spawn(Other)).has_value());
}

TEST_CASE("pane-open of a program that is not there is refused by name, and opens nothing", "[vthost][panes]")
{
    auto b = PanesBench {};
    CHECK(b.panes().open(b.spawn(Orchestrator, "run-1", std::nullopt, "not-installed"))
          == std::expected<std::string, OwnedPaneError> { std::unexpect, OwnedPaneError::ProgramMissing });
    auto const listed = b.panes().list(Orchestrator);
    REQUIRE(listed.has_value());
    CHECK(listed->empty());
    b.settle();
    CHECK(b.host.model().workspace(b.first)->tabCount() == 1);
}

TEST_CASE("the registry file is written whole and durably", "[vthost][panes]")
{
    using vthost::testing::DurableOp;
    auto files = vthost::testing::RecordingDurableFiles {};
    auto const file = std::filesystem::path { "/state/extensions/default/orchestrator/owned-panes" };
    auto b = PanesBench { std::make_shared<FileOwnedPaneStore>(
        files,
        std::map<std::string, std::filesystem::path, std::less<>> { { std::string(Orchestrator), file } }) };
    b.host.flushOwnedPanes();
    files.calls.clear();

    auto const pane = b.openNow(b.spawn());
    b.host.flushOwnedPanes();
    auto temporary = file;
    temporary += ".tmp";
    auto const content = std::format("{} run-1\n", pane);
    CHECK(files.calls
          == std::vector<vthost::testing::RecordedDurableCall> {
              { .op = DurableOp::CreateExclusive, .path = temporary, .content = {} },
              { .op = DurableOp::SyncFile, .path = temporary, .content = content },
              { .op = DurableOp::Replace, .path = temporary, .content = content },
              { .op = DurableOp::SyncDirectory, .path = file.parent_path(), .content = {} },
          });
    CHECK(files.readFile(file) == content);
    CHECK(parseOwnedPanes(content)
          == std::vector { OwnedPaneRecord { .pane = *vtcontrol::parseUuid(pane), .tag = "run-1" } });

    // Its session ended: the file holds none.
    REQUIRE(b.panes().close(Orchestrator, pane).has_value());
    REQUIRE(b.eventually([&] { return !b.stateOf(Orchestrator, pane).has_value(); }));
    b.host.flushOwnedPanes();
    CHECK(files.readFile(file) == std::string {});
}

TEST_CASE("pane-open from the lane races the loop's session end", "[vthost][panes][threading]")
{
    auto b = PanesBench {};
    constexpr auto Rounds = 40;
    auto opened = std::vector<std::string> {};
    auto openedMutex = std::mutex {};
    auto done = std::atomic<bool> { false };
    auto lane = std::thread { [&] {
        for (auto const index: std::views::iota(0, Rounds))
        {
            auto const tag = std::format("run-{}", index);
            if (auto const pane = b.panes().open(b.spawn(Orchestrator, tag)); pane)
            {
                auto const lock = std::lock_guard { openedMutex };
                opened.push_back(*pane);
            }
            std::ignore = b.panes().list(Orchestrator);
            auto pane = std::optional<std::string> {};
            {
                auto const lock = std::lock_guard { openedMutex };
                // Every position in turn, not only the oldest: the newest is the one most likely pending.
                if (!opened.empty() && index % 2 == 0)
                    pane = opened[static_cast<std::size_t>(index / 2) % opened.size()];
            }
            if (pane)
                std::ignore = b.panes().close(Orchestrator, *pane);
        }
        done = true;
    } };
    // The loop creates what the lane opens and ends what it closes, meanwhile.
    while (!done)
        std::ignore = b.loop.runOnce(1ms);
    lane.join();
    // Once settled, every pane listed is hosted, and every agent hosted is listed.
    auto const listedOnly = [&] {
        auto const listed = b.panes().list(Orchestrator);
        REQUIRE(listed.has_value());
        return std::ranges::count_if(*listed, [&](OwnedPaneInfo const& info) {
            return info.state != OwnedPaneState::Open || !b.sessionOf(info.pane).has_value();
        });
    };
    auto const hostedOnly = [&] {
        auto const listed = b.panes().list(Orchestrator);
        REQUIRE(listed.has_value());
        return std::ranges::count_if(b.host.sessionIds(), [&](SessionId session) {
            auto const uuid = b.host.objects().uuidOf(session);
            return session != b.shell
                   && (!uuid
                       || !std::ranges::contains(*listed, vtcontrol::toString(*uuid), &OwnedPaneInfo::pane));
        });
    };
    REQUIRE(b.eventually([&] { return listedOnly() == 0 && hostedOnly() == 0; }));
    CHECK(listedOnly() == 0);
    CHECK(hostedOnly() == 0);
}
