// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The agent orchestrator's real module on the real DaemonWorldExtensions, as the end-to-end tests run it:
/// its declared commands answered from its recorded fixtures, its module, grants and store in memory, and the
/// windows that connect to the daemon proving its consent key.

#include <vtbackend/screen/LocalTime.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/ThreadCpuClock.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/testing/GuestSlowdown.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vthost/ConnectionId.hpp>
#include <vthost/DaemonConsentKey.hpp>
#include <vthost/DaemonWorldExtensions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/OwnedPaneStore.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/SerializedFileSystem.hpp>

namespace vthost::testing
{

/// What the daemon configuration calls the orchestrator.
inline constexpr auto OrchestratorName = std::string_view { "orchestrator" };

/// The socket the test daemon is at, as its consent proofs are bound to it.
inline constexpr auto OrchestratorSocket = std::string_view { "/run/test/contour" };

/// The home the recorded fixtures name paths beneath; a world serves them beneath its own.
inline constexpr auto FixtureHome = std::string_view { "/home/u" };
inline constexpr auto OrchestratorModulePath = std::string_view { "/ext/orchestrator.wasm" };

/// @param worktreeRoot Where the worktrees go.
/// @param repository The repository's main checkout.
/// @return The orchestrator's configuration: one repository, with the issues assigned to the user; a task for
///         pull requests and one for issues.
[[nodiscard]] inline std::string orchestratorConfig(std::string_view worktreeRoot,
                                                    std::string_view repository)
{
    return std::format(R"({{ "worktree_root": "{}", "models": ["opus", "sonnet"],
  "repos": [ {{ "name": "contour", "path": "{}", "forge": "github",
               "issues": [ {{ "source": "forge", "query": "assignee:@me" }} ] }} ],
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "work", "key": "w", "label": "work on it", "applies_to": "issue", "model": "opus",
      "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                       worktreeRoot,
                       repository);
}

/// The daemon's environment the orchestrator inherits, and the test process holds while a world lives (as a
/// daemon's own environment is the one its panes inherit): the search path, and what a user's shell may hold
/// that must not choose the agent's repository (`GIT_DIR`, `GH_REPO`, `GH_HOST`) beside what it may keep (an
/// SSH command, a git setting of its own).
inline constexpr auto OrchestratorInheritedEnvironment = std::array {
    std::string_view { "PATH=/usr/bin" },
    std::string_view { "GIT_DIR=/elsewhere/.git" },
    std::string_view { "GH_REPO=someone/else" },
    std::string_view { "GH_HOST=forge.elsewhere.example" },
    std::string_view { "GIT_SSH_COMMAND=ssh -i /keys/agents" },
    std::string_view { "GIT_CONFIG_COUNT=1" },
    std::string_view { "GIT_CONFIG_KEY_0=user.name" },
    std::string_view { "GIT_CONFIG_VALUE_0=Agent User" },
};

/// @param reports Where the agent writes its report.
/// @param index Which agent it is, in spawn order.
/// @return What an agent's pane runs instead of `claude`: it writes its whole environment to
///         `<reports>/<index>.env` and its physical working directory to `<reports>/<index>.cwd`, tells on
///         its screen that it is ready, then echoes what it is sent -- so that a status report typed into it
///         reaches its terminal as the agent's own would.
[[nodiscard]] inline std::string fakeAgentScript(std::string_view reports, std::size_t index)
{
    return std::format(
        R"sh(env > '{0}/{1}.env'; pwd -P > '{0}/{1}.cwd'; )sh"
        R"sh(stty raw -echo; printf 'agent in %s ready' "$(basename "$(pwd -P)")"; exec cat)sh",
        reports,
        index);
}

/// What a fake agent reported of itself.
struct AgentReport
{
    std::string cwd;                      ///< Its physical working directory.
    std::vector<std::string> environment; ///< Its environment, `NAME=value` per entry.

    /// @return Whether its environment holds @p name at all.
    [[nodiscard]] bool has(std::string_view name) const
    {
        return std::ranges::any_of(environment, [&](std::string const& entry) {
            return entry.starts_with(name) && entry.size() > name.size() && entry[name.size()] == '=';
        });
    }

    /// @return Whether its environment holds @p entry (`NAME=value`).
    [[nodiscard]] bool holds(std::string_view entry) const
    {
        return std::ranges::contains(environment, entry);
    }
};

/// @return The consent key of the test daemon.
[[nodiscard]] inline ConsentKey orchestratorConsentKey()
{
    auto key = ConsentKey {};
    key.fill(std::byte { 0x42 });
    return key;
}

/// @return The file at @p path, whole.
[[nodiscard]] inline std::string wholeFile(std::filesystem::path const& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// What a test's client is.
enum class ClientKind : uint8_t
{
    Cli = 0, ///< Offers nothing to answer with: the CLI's native clients.
    Window,  ///< Offers DaemonExtensionConsent, ExtensionPresence, AttachIntent and InputKinds; proves the
             ///< daemon's key.
};

/// What a test's client was told.
struct ClientLog
{
    std::vector<proto::ExtensionPermissionRequest> requests;           ///< Every question asked, in order.
    std::vector<client::NativeClient::ExtensionEvent> extensionEvents; ///< Every palette update and reveal.
};

/// @return Whether @p log's latest palette rows offer @p extension's @p action, the extension running.
[[nodiscard]] inline bool offersRunning(ClientLog const& log,
                                        std::string_view extension,
                                        std::string_view action)
{
    auto const latest =
        std::ranges::find_if(log.extensionEvents | std::views::reverse, [](auto const& event) {
            return std::holds_alternative<proto::ExtensionPresence>(event);
        });
    if (latest == std::ranges::end(log.extensionEvents | std::views::reverse))
        return false;
    return std::ranges::any_of(
        std::get<proto::ExtensionPresence>(*latest).extensions, [&](auto const& entry) {
            return entry.extension == extension && entry.state == proto::WirePresenceState::Running
                   && std::ranges::contains(entry.rows, action, &proto::WireActionRow::id);
        });
}

/// @param intent Which of the daemon's workspaces it asks to be shown.
/// @return The handshake of a window: it offers DaemonExtensionConsent, ExtensionPresence, AttachIntent and
///         InputKinds, and proves the daemon's key.
[[nodiscard]] inline client::NativeClient::HandshakeOptions windowHandshake(proto::AttachIntent intent)
{
    auto handshake = client::NativeClient::HandshakeOptions {};
    handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::DaemonExtensionConsent,
                                                        proto::Capability::ExtensionPresence,
                                                        proto::Capability::AttachIntent,
                                                        proto::Capability::InputKinds });
    handshake.attachIntent = intent;
    handshake.consentProver = [](uint64_t instance, ConsentNonce const& nonce) {
        return std::optional { consentProofOf(
            orchestratorConsentKey(),
            ConsentContext { .daemonInstance = instance, .socket = std::string(OrchestratorSocket) },
            nonce) };
    };
    return handshake;
}

/// The daemon's side of one connection, and the client's.
struct Connection
{
    std::unique_ptr<NativeSession> server;
    std::unique_ptr<client::NativeClient> client;
    ClientLog log;
};

/// The host's owned panes as a daemon world reaches them, with a stall a test can put after one pane-open:
/// the host has opened the pane, but the guest hears of it only once its call deadline is spent. A deadline
/// is spent in the CPU time of the guest's thread, which the stall runs on, so the stall keeps that thread
/// busy: a host that merely took long to answer would cost the guest nothing short of the real-time bound.
class StallingPanes final: public IOwnedPanes
{
  public:
    /// @param host The host's owned panes.
    /// @param stall How much CPU time the next open() spends after the host answered it, once; read on the
    ///        lane.
    StallingPanes(IOwnedPanes& host, std::atomic<std::chrono::milliseconds::rep>& stall):
        _host { host }, _stall { stall }
    {
    }

    [[nodiscard]] std::expected<std::string, OwnedPaneError> open(OwnedPaneSpawn spawn) override
    {
        auto opened = _host.open(std::move(spawn));
        if (auto const stall = _stall.exchange(0); stall != 0)
            spend(std::chrono::milliseconds { stall });
        return opened;
    }

    [[nodiscard]] std::expected<void, OwnedPaneError> close(std::string_view extension,
                                                            std::string_view pane) override
    {
        return _host.close(extension, pane);
    }

    [[nodiscard]] std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> list(
        std::string_view extension) const override
    {
        return _host.list(extension);
    }

    [[nodiscard]] std::expected<void, OwnedPaneError> reveal(std::string_view extension,
                                                             std::string_view pane,
                                                             Origin origin) override
    {
        return _host.reveal(extension, pane, origin);
    }

    [[nodiscard]] std::expected<void, OwnedPaneError> write(std::string_view extension,
                                                            std::string_view pane,
                                                            std::string_view bytes) override
    {
        return _host.write(extension, pane, bytes);
    }

  private:
    /// Keeps the calling thread busy until it has spent @p cpu of CPU time; where its clock cannot be read,
    /// sleeps that long in real time.
    static void spend(std::chrono::milliseconds cpu)
    {
        auto const& clock = vtextension::nativeThreadCpuClock();
        auto const thread = clock.currentThread();
        auto const start = thread ? clock.consumed(*thread) : std::nullopt;
        if (!start)
        {
            std::this_thread::sleep_for(cpu);
            return;
        }
        auto spins = std::uint64_t { 0 };
        while (clock.consumed(*thread).value_or(*start + cpu) - *start < cpu)
            ++spins;
        std::ignore = spins;
    }

    IOwnedPanes& _host;
    std::atomic<std::chrono::milliseconds::rep>& _stall;
};

/// Where a world's git runs for real. Its paths must name themselves: no symlink on the way.
struct RealGit
{
    std::filesystem::path repository;   ///< The repository's main checkout, with a remote `origin`.
    std::filesystem::path worktreeRoot; ///< Where the worktrees go; it exists.
    vtpty::IProcessRunner& runner;      ///< What starts git; it outlives the world.
    /// What git runs with besides each command's own: a home and configuration of the test's, never the
    /// user's or the machine's.
    std::vector<std::pair<std::string, std::string>> environment;
};

/// What the orchestrator's daemon world runs on, apart from the host: the real WebAssembly backend, its
/// commands answered from the recorded fixtures, its module, grants and store in memory. It outlives every
/// host it is the daemon world of, so that what one daemon wrote -- the grant, the store -- the next reads,
/// as across a live upgrade or a keeper recovery.
struct OrchestratorWorld
{
    /// @param module The orchestrator's module, read once into the files.
    /// @param testdata The orchestrator's testdata directory, which the commands' answers are read from.
    /// @param git Where git runs for real; none: its commands are answered too, beneath a home of the world's
    ///        own on disk, where a worktree a command adds is a directory holding a `.git` file that names
    ///        the repository's; every removal check answered as a clean worktree's. Directories are opened,
    ///        and files read, by the native services either way.
    OrchestratorWorld(std::filesystem::path const& module,
                      std::filesystem::path const& testdata,
                      std::optional<RealGit> git = std::nullopt):
        processRunner { testdata },
        ownHome { [&]() -> std::optional<core::testing::ScopedTempDir> {
            if (git)
                return std::nullopt;
            return std::optional<core::testing::ScopedTempDir> { std::in_place,
                                                                 "contour-orchestrator-world" };
        }() },
        home { git ? git->worktreeRoot.parent_path().string()
                   : std::filesystem::canonical(ownHome->path()).string() },
        worktreeRootPath { git ? git->worktreeRoot.string() : home + "/worktrees" },
        repositoryPath { git ? git->repository.string() : home + "/projects/contour" }
    {
        // Set before any host or thread exists, and restored after every one is gone.
        for (auto const entry: OrchestratorInheritedEnvironment | std::views::drop(1))
        {
            auto const equals = entry.find('=');
            daemonEnvironment.push_back(std::make_unique<core::testing::ScopedEnv>(entry.substr(0, equals),
                                                                                   entry.substr(equals + 1)));
        }
        fileSystem.files.addFile(std::filesystem::path(OrchestratorModulePath), wholeFile(module));
        if (git)
        {
            processRunner.forward("git", git->runner, std::move(git->environment));
            return;
        }
        for (auto const& path: { worktreeRoot(), repoPath() })
            std::filesystem::create_directories(path);
        // The worktrees the recorded list names, beneath this world's home.
        auto listed = wholeFile(testdata / "git/worktree-list.porcelain-z");
        for (auto at = listed.find(FixtureHome); at != std::string::npos; at = listed.find(FixtureHome, at))
        {
            listed.replace(at, FixtureHome.size(), home);
            at += home.size();
        }
        processRunner.script({ .prefix = { "git", "worktree", "list" },
                               .standardOutput = std::move(listed),
                               .exitCode = 0,
                               .effect = {},
                               .times = std::nullopt });
        for (auto& answer: vtextension::testing::cleanWorktreeAnswers(repoPath()))
            processRunner.script(std::move(answer));
        processRunner.script({ .prefix = { "git", "worktree", "add" },
                               .standardOutput = {},
                               .exitCode = 0,
                               .effect = [this](auto const& argv) { addWorktree(argv[argv.size() - 2]); },
                               .times = std::nullopt });
        processRunner.script({ .prefix = { "git", "worktree", "remove" },
                               .standardOutput = {},
                               .exitCode = 0,
                               .effect = [](auto const& argv) { std::filesystem::remove_all(argv.back()); },
                               .times = std::nullopt });
    }

    /// @return Where the orchestrator adds worktrees.
    [[nodiscard]] std::string const& worktreeRoot() const noexcept { return worktreeRootPath; }

    /// @return The repository's main checkout.
    [[nodiscard]] std::string const& repoPath() const noexcept { return repositoryPath; }

    OrchestratorWorld(OrchestratorWorld const&) = delete;
    OrchestratorWorld(OrchestratorWorld&&) = delete;
    OrchestratorWorld& operator=(OrchestratorWorld const&) = delete;
    OrchestratorWorld& operator=(OrchestratorWorld&&) = delete;
    ~OrchestratorWorld() = default;

    /// @return What makes a host's daemon world on these services; each host it is called for gets its own.
    [[nodiscard]] DaemonWorldFactory factory()
    {
        return [this](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring) {
            auto const& panes =
                stallingPanes.emplace_back(std::make_unique<StallingPanes>(wiring.panes, stall));
            auto made = std::make_shared<DaemonWorldExtensions>(
                runtimeConfig(),
                DaemonWorldServices { .backend = *backend,
                                      .runner = processRunner,
                                      .directories = vtpty::nativeDirectoryOpener(),
                                      .files = vtpty::nativeBeneathReader(),
                                      .fileSystem = fileSystem,
                                      .locker = locker,
                                      .wallClock = wallClock,
                                      .localTime = localTime },
                std::move(events),
                std::move(post),
                DaemonWorldWiring { .tabs = wiring.tabs, .actions = wiring.actions, .panes = *panes });
            current = made;
            return std::shared_ptr<IDaemonWorldHosting> { std::move(made) };
        };
    }

    /// @return A configuration naming objects with sequential UUIDs, whose daemon world is this world's and
    ///         whose registry of agent panes is @ref registry, which every host made with it shares.
    [[nodiscard]] SessionHostConfig hostConfig()
    {
        auto config = LiveHost::sequentialIds();
        config.daemonWorld = factory();
        config.ownedPaneStore = registry;
        return config;
    }

    /// @return What makes each session's device: the echoing shell, and for an agent's pane the fake agent
    ///         (fakeAgentScript()), with the environment and the directory handle its pane template gave it.
    [[nodiscard]] PtyFactory ptys()
    {
        return [this](vtpty::PageSize size, std::optional<vtpty::Process::ExecInfo> const& commandOverride)
                   -> std::unique_ptr<vtpty::Pty> {
            // Every shell is told where it lives through the override; only an agent names a program.
            if (!commandOverride || commandOverride->program.empty())
                return shellProcess(size);
            auto exec = *commandOverride;
            {
                auto const lock = std::scoped_lock { spawnsMutex };
                exec.arguments = { "-c", fakeAgentScript(reports.path().string(), agentSpawns.size()) };
                agentSpawns.push_back(*commandOverride);
            }
            exec.program = "/bin/sh";
            return std::make_unique<vtpty::Process>(exec,
                                                    vtpty::createPty(size, std::nullopt),
                                                    vtpty::SandboxEscape::Contained,
                                                    std::make_shared<vtpty::NoPlacement>());
        };
    }

    /// @return What every agent's pane was asked to run, in the order they were spawned.
    [[nodiscard]] std::vector<vtpty::Process::ExecInfo> spawnedAgents() const
    {
        auto const lock = std::scoped_lock { spawnsMutex };
        return agentSpawns;
    }

    /// @param index Which agent, in spawn order.
    /// @return What it reported of itself; std::nullopt until it has.
    [[nodiscard]] std::optional<AgentReport> agentReport(std::size_t index) const
    {
        auto const base = reports.path() / std::format("{}", index);
        auto cwd = std::ifstream(base.string() + ".cwd");
        auto environment = std::ifstream(base.string() + ".env");
        auto report = AgentReport {};
        if (!cwd || !environment || !std::getline(cwd, report.cwd))
            return std::nullopt;
        for (auto line = std::string {}; std::getline(environment, line);)
            report.environment.push_back(line);
        return report;
    }

    /// @return Whether the worktree @p path is there, on disk.
    [[nodiscard]] static bool hasWorktree(std::string const& path)
    {
        return std::filesystem::is_directory(path);
    }

    /// @param run A run's id.
    /// @return The run as the orchestrator stored it, as JSON; empty if it stored none.
    [[nodiscard]] std::string storedRun(std::string_view run) const
    {
        return fileSystem.readFile(std::filesystem::path(OrchestratorStore) / std::format("run%{}", run))
            .value_or(std::string {});
    }

    /// @return The ids of the runs the orchestrator stored, in the store's order.
    [[nodiscard]] std::vector<std::string> storedRuns() const
    {
        auto runs = std::vector<std::string> {};
        auto const entries = fileSystem.listDirectory(OrchestratorStore);
        if (!entries)
            return runs;
        for (auto const& entry: *entries)
            if (auto const name = entry.path.filename().string(); name.starts_with("run%"))
                runs.push_back(name.substr(4));
        return runs;
    }

    /// Has the host's next pane-open answer the guest only after spending twice its call deadline of the
    /// guest thread's CPU time: the pane is opened, and the guest faults before it can store that it was.
    void stallNextPaneOpen() { stall = (2 * vtextension::testing::guestTestLimits().callDeadline).count(); }

    /// @return The daemon world of the latest host made with factory(); null once that host is gone.
    [[nodiscard]] std::shared_ptr<DaemonWorldExtensions> world() const { return current.lock(); }

    /// @return Whether the orchestrator runs in the latest host's daemon world.
    [[nodiscard]] bool isRunning() const
    {
        auto const hosting = world();
        if (!hosting)
            return false;
        auto const status = hosting->status(OrchestratorName);
        return status && status->state == DaemonExtensionState::Running;
    }

    /// @return Whether every command one poll runs was started, and none waits for its answer.
    [[nodiscard]] bool pollDone() const
    {
        return processRunner.waiting() == 0
               && std::ranges::all_of(vtextension::testing::OrchestratorFixtureAnswers,
                                      [this](vtextension::testing::FixtureAnswer const& answer) {
                                          return processRunner.startsOf(answer.program, answer.verb) != 0;
                                      });
    }

    /// @return A new connection to @p host on @p loop, of a client of @p kind; a window asks for the
    /// workspace
    ///         @p intent names.
    [[nodiscard]] std::unique_ptr<Connection> connect(core::net::EventLoop& loop,
                                                      SessionHost& host,
                                                      ClientKind kind,
                                                      proto::AttachIntent intent = {})
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto connection = std::make_unique<Connection>();
        auto services = NativeSessionServices {};
        services.handshake.consent = ConsentGate { .key = orchestratorConsentKey(),
                                                   .socket = std::string(OrchestratorSocket),
                                                   .random = random };
        connection->server =
            std::make_unique<NativeSession>(loop,
                                            host,
                                            ConnectionId { .endpoint = "test", .index = ++connections },
                                            std::move(pair.first),
                                            NativeSession::DefaultWriteQueueBytes,
                                            std::move(services));
        auto onConsent = client::NativeClient::DaemonConsentHandler {};
        auto onExtensionEvent = client::NativeClient::ExtensionEventHandler {};
        if (kind == ClientKind::Window)
        {
            onConsent = [log = &connection->log](client::NativeClient::DaemonConsentEvent const& event) {
                if (auto const* request = std::get_if<proto::ExtensionPermissionRequest>(&event))
                    log->requests.push_back(*request);
            };
            onExtensionEvent = [log = &connection->log](client::NativeClient::ExtensionEvent const& event) {
                log->extensionEvents.push_back(event);
            };
        }
        connection->client = std::make_unique<client::NativeClient>(
            loop,
            std::move(pair.second),
            kind == ClientKind::Window ? windowHandshake(intent) : client::NativeClient::HandshakeOptions {},
            vtbackend::CommandBlockStoreLimits {},
            client::NativeClient::UpdateHandler {},
            client::NativeClient::ImageHandler {},
            client::NativeClient::SessionEventHandler {},
            client::NativeClient::LayoutHandler {},
            client::NativeClient::PermissionHandler {},
            client::LivenessPolicy {},
            client::NativeClient::TabMergeRefusedHandler {},
            client::NativeClient::MoveNodeRefusedHandler {},
            client::NativeClient::LayoutBatchEndHandler {},
            client::NativeClient::PanesReprojectedHandler {},
            client::NativeClient::SwitchRequestHandler {},
            client::NativeClient::PaneSummaryHandler {},
            client::NativeClient::RaiseRoleHandler {},
            client::NativeClient::WorkspaceMetaHandler {},
            client::NativeClient::SnapshotProgressHandler {},
            std::move(onConsent),
            std::move(onExtensionEvent));
        return connection;
    }

    /// @return The daemon configuration's orchestrator: started with the daemon, its module and grants in the
    ///         files, under the daemon world's limits (their deadlines stretched by
    ///         vtextension::testing::GuestSlowdown).
    [[nodiscard]] vtextension::DaemonExtensionRuntimeConfig runtimeConfig() const
    {
        auto const limits = vtextension::testing::guestTestLimits();
        return vtextension::DaemonExtensionRuntimeConfig {
            .declarations = { vtextension::DaemonExtensionDeclaration {
                .name = std::string(OrchestratorName),
                .modulePath = std::filesystem::path(OrchestratorModulePath),
                .autostart = vtextension::ExtensionAutostart::AtDaemonStart,
                .configJson = orchestratorConfig(worktreeRootPath, repositoryPath),
                .storeDirectory = std::filesystem::path(OrchestratorStore) } },
            .grantsFile = "/config/extension-grants.daemon-scoped.txt",
            .home = home,
            .inheritedEnvironment =
                std::ranges::to<std::vector<std::string>>(OrchestratorInheritedEnvironment),
            .bundledDirectories = {},
            .limits = limits,
        };
    }

    /// The store's directory.
    static constexpr auto OrchestratorStore =
        std::string_view { "/state/extensions/default/orchestrator/store" };

    std::unique_ptr<vtextension::wasm::IWasmBackend> backend = vtextension::wasm::makeDefaultBackend();
    vtextension::testing::FixtureProcessRunner processRunner;
    std::optional<core::testing::ScopedTempDir> ownHome; ///< The world's own home, unless git runs for real.
    std::string home;                                    ///< The orchestrator's `~`, symlinks resolved.
    std::string worktreeRootPath;                        ///< @see worktreeRoot()
    std::string repositoryPath;                          ///< @see repoPath()
    /// The test process's environment while the world lives: OrchestratorInheritedEnvironment's, but PATH.
    std::vector<std::unique_ptr<core::testing::ScopedEnv>> daemonEnvironment;
    core::testing::ScopedTempDir reports { "contour-agent-reports" }; ///< Where the fake agents report.
    mutable std::mutex spawnsMutex; ///< Guards @ref agentSpawns: the loop spawns, the test reads.
    std::vector<vtpty::Process::ExecInfo> agentSpawns; ///< @see spawnedAgents()
    /// The registry of agent panes, shared by every host this world serves: what a live upgrade or a keeper
    /// recovery finds the old daemon wrote.
    std::shared_ptr<InMemoryOwnedPaneStore> registry = std::make_shared<InMemoryOwnedPaneStore>();
    std::atomic<std::chrono::milliseconds::rep> stall { 0 };   ///< @see stallNextPaneOpen()
    std::vector<std::unique_ptr<StallingPanes>> stallingPanes; ///< One per host made, kept with the world.
    SerializedFileSystem fileSystem;
    vtextension::testing::FakeFileLocker locker;
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point {
        std::chrono::hours { 500'000 } } };
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    CountingRandom random;
    std::weak_ptr<DaemonWorldExtensions> current; ///< The latest host's daemon world (@see world()).
    uint64_t connections = 0;                     ///< How many connections were made.

  private:
    /// What `git worktree add` does: the worktree, and its `.git` file naming the repository's.
    void addWorktree(std::string const& path) const
    {
        auto const name = std::filesystem::path(path).filename().string();
        std::filesystem::create_directories(path);
        auto file = std::ofstream(std::filesystem::path(path) / ".git");
        file << std::format("gitdir: {}/.git/worktrees/{}\n", repoPath(), name);
    }
};

} // namespace vthost::testing
