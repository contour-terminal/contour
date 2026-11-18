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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/testing/FakeBeneathReader.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/testing/GuestSlowdown.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vthost/ConnectionId.hpp>
#include <vthost/DaemonConsentKey.hpp>
#include <vthost/DaemonWorldExtensions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/CountingRandom.hpp>

namespace vthost::testing
{

/// What the daemon configuration calls the orchestrator.
inline constexpr auto OrchestratorName = std::string_view { "orchestrator" };

/// The socket the test daemon is at, as its consent proofs are bound to it.
inline constexpr auto OrchestratorSocket = std::string_view { "/run/test/contour" };

inline constexpr auto OrchestratorHome = std::string_view { "/home/u" };
inline constexpr auto OrchestratorWorktreeRoot = std::string_view { "/home/u/worktrees" };
inline constexpr auto OrchestratorRepoPath = std::string_view { "/home/u/projects/contour" };
inline constexpr auto OrchestratorModulePath = std::string_view { "/ext/orchestrator.wasm" };

/// The orchestrator's configuration: one repository, with the issues assigned to the user.
inline constexpr auto OrchestratorConfig =
    std::string_view { R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github",
               "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "prompt": "/contour-workflows:fix-ci {url}" } ] })" };

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
    Window,  ///< Offers DaemonExtensionConsent, ExtensionPresence and AttachIntent; proves the daemon's key.
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
/// @return The handshake of a window: it offers DaemonExtensionConsent, ExtensionPresence and AttachIntent,
/// and
///         proves the daemon's key.
[[nodiscard]] inline client::NativeClient::HandshakeOptions windowHandshake(proto::AttachIntent intent)
{
    auto handshake = client::NativeClient::HandshakeOptions {};
    handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::DaemonExtensionConsent,
                                                        proto::Capability::ExtensionPresence,
                                                        proto::Capability::AttachIntent });
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

/// What the orchestrator's daemon world runs on, apart from the host: the real WebAssembly backend, its
/// commands answered from the recorded fixtures, its module, grants and store in memory. It outlives every
/// host it is the daemon world of, so that what one daemon wrote -- the grant, the store -- the next reads,
/// as across a live upgrade or a keeper recovery.
struct OrchestratorWorld
{
    /// @param module The orchestrator's module, read once into the files.
    /// @param testdata The orchestrator's testdata directory, which the commands' answers are read from.
    OrchestratorWorld(std::filesystem::path const& module, std::filesystem::path testdata):
        processRunner { std::move(testdata) }
    {
        for (auto const path: { OrchestratorWorktreeRoot, OrchestratorRepoPath })
        {
            fileSystem.addDirectory(std::filesystem::path(path));
            directories.add(std::string(path));
        }
        fileSystem.addFile(std::filesystem::path(OrchestratorModulePath), wholeFile(module));
    }

    OrchestratorWorld(OrchestratorWorld const&) = delete;
    OrchestratorWorld(OrchestratorWorld&&) = delete;
    OrchestratorWorld& operator=(OrchestratorWorld const&) = delete;
    OrchestratorWorld& operator=(OrchestratorWorld&&) = delete;
    ~OrchestratorWorld() = default;

    /// @return What makes a host's daemon world on these services; each host it is called for gets its own.
    [[nodiscard]] DaemonWorldFactory factory()
    {
        return [this](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring) {
            auto made =
                std::make_shared<DaemonWorldExtensions>(runtimeConfig(),
                                                        DaemonWorldServices { .backend = *backend,
                                                                              .runner = processRunner,
                                                                              .directories = directories,
                                                                              .files = files,
                                                                              .fileSystem = fileSystem,
                                                                              .locker = locker,
                                                                              .wallClock = wallClock,
                                                                              .localTime = localTime },
                                                        std::move(events),
                                                        std::move(post),
                                                        wiring);
            current = made;
            return std::shared_ptr<IDaemonWorldHosting> { std::move(made) };
        };
    }

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
    [[nodiscard]] static vtextension::DaemonExtensionRuntimeConfig runtimeConfig()
    {
        auto const limits = vtextension::testing::guestTestLimits();
        return vtextension::DaemonExtensionRuntimeConfig {
            .declarations = { vtextension::DaemonExtensionDeclaration {
                .name = std::string(OrchestratorName),
                .modulePath = std::filesystem::path(OrchestratorModulePath),
                .autostart = vtextension::ExtensionAutostart::AtDaemonStart,
                .configJson = std::string(OrchestratorConfig),
                .storeDirectory = "/state/extensions/default/orchestrator/store" } },
            .grantsFile = "/config/extension-grants.daemon-scoped.txt",
            .home = std::string(OrchestratorHome),
            .inheritedEnvironment = { "PATH=/usr/bin" },
            .bundledDirectories = {},
            .limits = limits,
        };
    }

    std::unique_ptr<vtextension::wasm::IWasmBackend> backend = vtextension::wasm::makeDefaultBackend();
    vtextension::testing::FixtureProcessRunner processRunner;
    vtextension::testing::FakeDirectoryOpener directories;
    /// The files `file-read` reads: none unless a test adds them, every other read `not-supported-yet`.
    vtextension::testing::FakeBeneathReader files;
    core::platform::testing::InMemoryFileSystem fileSystem;
    vtextension::testing::FakeFileLocker locker;
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point {
        std::chrono::hours { 500'000 } } };
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    CountingRandom random;
    std::weak_ptr<DaemonWorldExtensions> current; ///< The latest host's daemon world (@see world()).
    uint64_t connections = 0;                     ///< How many connections were made.
};

} // namespace vthost::testing
