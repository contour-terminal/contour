// SPDX-License-Identifier: Apache-2.0
//
// The daemon world end to end: the agent orchestrator's real module on the real DaemonWorldExtensions, its
// declared commands answered from the orchestrator's recorded fixtures, granted by a NativeClient that proved
// the daemon's consent key -- and still running, summary and all, once no window is left.
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vthost/ConnectionId.hpp>
#include <vthost/DaemonConsentKey.hpp>
#include <vthost/DaemonWorldExtensions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/CountingRandom.hpp>

#ifdef VTHOST_TEST_ORCHESTRATOR_WASM

using core::async::Task;
using vtextension::testing::FixtureProcessRunner;
using vthost::client::NativeClient;
using namespace std::string_view_literals;
namespace proto = vthost::proto;

namespace
{

/// The longest the test waits, in 1 ms loop ticks, for what it counts on.
constexpr auto WaitTicks = 60'000;

/// The socket the daemon is at, as its consent proofs are bound to it.
constexpr auto DaemonSocket = "/run/test/contour"sv;

constexpr auto Home = "/home/u"sv;
constexpr auto WorktreeRoot = "/home/u/worktrees"sv;
constexpr auto RepoPath = "/home/u/projects/contour"sv;
constexpr auto ModulePath = "/ext/orchestrator.wasm"sv;

/// The orchestrator's configuration: one repository, with the issues assigned to the user.
constexpr auto OrchestratorConfig = R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github",
               "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "prompt": "/contour-workflows:fix-ci {url}" } ] })"sv;

/// The indicator status line every hosted session shows: the orchestrator's summary alone.
constexpr auto IndicatorTemplate = "[{DaemonExtension:orchestrator}]"sv;

/// @return The consent key of the test's daemon.
[[nodiscard]] vthost::ConsentKey daemonKey()
{
    auto key = vthost::ConsentKey {};
    key.fill(std::byte { 0x42 });
    return key;
}

/// @return The file at @p path, whole.
[[nodiscard]] std::string fileText(std::filesystem::path const& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// @return Settings whose indicator status line shows the orchestrator's summary.
[[nodiscard]] vtbackend::Settings showingSummary()
{
    auto settings = vtbackend::Settings {};
    settings.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
    settings.indicatorStatusLine.left = std::string(IndicatorTemplate);
    settings.indicatorStatusLine.middle.clear();
    settings.indicatorStatusLine.right.clear();
    return settings;
}

/// @return The indicator row @p screen mirrors, as UTF-8, trailing blanks trimmed; none if it mirrors none.
[[nodiscard]] std::optional<std::string> indicatorRowOf(vthost::client::RemoteScreen const& screen)
{
    if (screen.statusLinesDisplay != std::to_underlying(vtbackend::StatusDisplayType::Indicator)
        || screen.statusLines.empty())
        return std::nullopt;
    auto text = std::string {};
    for (auto const& cell: screen.statusLines.front().cells)
    {
        if (cell.codepoint == 0)
            text += ' ';
        else
            vthost::client::appendCluster(text, cell);
    }
    while (text.ends_with(' '))
        text.pop_back();
    return text;
}

/// What a test's client is.
enum class ClientKind : uint8_t
{
    Cli = 0, ///< Offers nothing to answer with: the CLI's native clients.
    Window,  ///< Offers DaemonExtensionConsent and proves the daemon's key.
};

/// What a test's client is told and does: the questions it was asked.
struct ClientLog
{
    std::vector<proto::ExtensionPermissionRequest> requests; ///< Every question asked, in order.
};

/// The handshake of a window: it offers DaemonExtensionConsent and proves the daemon's key.
[[nodiscard]] NativeClient::HandshakeOptions windowHandshake()
{
    auto handshake = NativeClient::HandshakeOptions {};
    handshake.capabilities = proto::CapabilitySet::of({ proto::Capability::DaemonExtensionConsent });
    handshake.consentProver = [](uint64_t instance, vthost::ConsentNonce const& nonce) {
        return std::optional { vthost::consentProofOf(
            daemonKey(),
            vthost::ConsentContext { .daemonInstance = instance, .socket = std::string(DaemonSocket) },
            nonce) };
    };
    return handshake;
}

/// The daemon's side of one connection, and the client's.
struct Connection
{
    std::unique_ptr<vthost::NativeSession> server;
    std::unique_ptr<NativeClient> client;
    ClientLog log;
};

/// A daemon host whose daemon world is the orchestrator on the real DaemonWorldExtensions, its commands
/// answered from the recorded fixtures, its module, grants and store in memory, and a control service on it.
struct DaemonWorldHarness
{
    DaemonWorldHarness()
    {
        for (auto const path: { WorktreeRoot, RepoPath })
        {
            fileSystem.addDirectory(std::filesystem::path(path));
            directories.add(std::string(path));
        }
        fileSystem.addFile(std::filesystem::path(ModulePath), fileText(VTHOST_TEST_ORCHESTRATOR_WASM));
        host = std::make_unique<vthost::SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            showingSummary(),
            core::defaultEnvironment(),
            vthost::SessionHostConfig {
                .pumping = vthost::SessionPumping::Manual,
                .daemonWorld = [this](vthost::DaemonWorldEvents events, vthost::OwnerPost const& /*post*/) {
                    world = std::make_shared<vthost::DaemonWorldExtensions>(
                        runtimeConfig(),
                        vthost::DaemonWorldServices { .backend = *backend,
                                                      .runner = processRunner,
                                                      .directories = directories,
                                                      .fileSystem = fileSystem,
                                                      .locker = locker,
                                                      .wallClock = wallClock,
                                                      .localTime = localTime },
                        std::move(events));
                    return std::shared_ptr<vthost::IDaemonWorldHosting> { world };
                } });
        std::ignore = host->createTab();
        control = std::make_unique<vthost::control::ControlService>(
            *host,
            vthost::control::ControlServiceConfig { .version = "9.8.7",
                                                    .socketPath = "/run/test.sock",
                                                    .clock = wallClock,
                                                    .extensions = host->daemonExtensionControl() });
    }

    DaemonWorldHarness(DaemonWorldHarness const&) = delete;
    DaemonWorldHarness(DaemonWorldHarness&&) = delete;
    DaemonWorldHarness& operator=(DaemonWorldHarness const&) = delete;
    DaemonWorldHarness& operator=(DaemonWorldHarness&&) = delete;

    ~DaemonWorldHarness()
    {
        control.reset();
        host.reset();
    }

    /// @return A new connection to the host, of a client of @p kind.
    [[nodiscard]] std::unique_ptr<Connection> connect(ClientKind kind)
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto connection = std::make_unique<Connection>();
        auto services = vthost::NativeSessionServices {};
        services.handshake.consent =
            vthost::ConsentGate { .key = daemonKey(), .socket = std::string(DaemonSocket), .random = random };
        connection->server = std::make_unique<vthost::NativeSession>(
            loop,
            *host,
            vthost::ConnectionId { .endpoint = "test", .index = ++connections },
            std::move(pair.first),
            vthost::NativeSession::DefaultWriteQueueBytes,
            std::move(services));
        auto onConsent = NativeClient::DaemonConsentHandler {};
        if (kind == ClientKind::Window)
            onConsent = [log = &connection->log](NativeClient::DaemonConsentEvent const& event) {
                if (auto const* request = std::get_if<proto::ExtensionPermissionRequest>(&event))
                    log->requests.push_back(*request);
            };
        connection->client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            kind == ClientKind::Window ? windowHandshake() : NativeClient::HandshakeOptions {},
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {},
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {},
            NativeClient::LayoutHandler {},
            NativeClient::PermissionHandler {},
            vthost::client::LivenessPolicy {},
            NativeClient::TabMergeRefusedHandler {},
            NativeClient::MoveNodeRefusedHandler {},
            NativeClient::LayoutBatchEndHandler {},
            NativeClient::PanesReprojectedHandler {},
            NativeClient::SwitchRequestHandler {},
            NativeClient::PaneSummaryHandler {},
            NativeClient::RaiseRoleHandler {},
            NativeClient::WorkspaceMetaHandler {},
            NativeClient::SnapshotProgressHandler {},
            std::move(onConsent));
        return connection;
    }

    /// @return What `extension.status orchestrator` answers, through the control service.
    [[nodiscard]] vtcontrol::ExtensionStatusReply status()
    {
        auto reply =
            std::optional<std::expected<vtcontrol::ExtensionStatusReply, vtcontrol::ControlError>> {};
        control->handle(context,
                        vtcontrol::ExtensionStatusRequest { .name = "orchestrator" },
                        [&](std::expected<vtcontrol::ExtensionStatusReply, vtcontrol::ControlError> answer) {
                            reply = std::move(answer);
                        });
        REQUIRE(reply.has_value());
        REQUIRE(reply->has_value());
        return std::move(**reply);
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

    [[nodiscard]] static vtextension::DaemonExtensionRuntimeConfig runtimeConfig()
    {
        return vtextension::DaemonExtensionRuntimeConfig {
            .declarations = { vtextension::DaemonExtensionDeclaration {
                .name = "orchestrator",
                .module = std::filesystem::path(ModulePath),
                .autostart = vtextension::ExtensionAutostart::AtDaemonStart,
                .configJson = std::string(OrchestratorConfig),
                .storeDirectory = "/state/extensions/default/orchestrator/store" } },
            .grantsFile = "/config/extension-grants.daemon-scoped.txt",
            .home = std::string(Home),
            .inheritedEnvironment = { "PATH=/usr/bin" },
            .bundledDirectories = {},
        };
    }

    // What the daemon world borrows: declared before the host, which stops it, so that they outlive it.
    std::unique_ptr<vtextension::wasm::IWasmBackend> backend = vtextension::wasm::makeDefaultBackend();
    FixtureProcessRunner processRunner { VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    vtextension::testing::FakeDirectoryOpener directories;
    core::platform::testing::InMemoryFileSystem fileSystem;
    vtextension::testing::FakeFileLocker locker;
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point {
        std::chrono::hours { 500'000 } } };
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    vthost::testing::CountingRandom random;

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<vthost::DaemonWorldExtensions> world;
    std::unique_ptr<vthost::SessionHost> host;
    std::unique_ptr<vthost::control::ControlService> control;
    vtcontrol::testing::FakeCallContext context;
    uint64_t connections = 0;
};

/// The window's part: answers the question it is asked with Granted, waits until the daemon world runs the
/// extension, and closes its connection -- leaving no window.
Task<void> grantAndLeave(core::net::EventLoop* loop, DaemonWorldHarness* h, Connection* window)
{
    std::ignore = co_await core::net::testing::waitUntil(
        loop, [window] { return !window->log.requests.empty(); }, WaitTicks);
    if (window->log.requests.empty())
    {
        window->client->detach();
        co_return;
    }
    auto const& request = window->log.requests.front();
    window->client->answerDaemonExtension(request.name, request.grantDigest, vthost::GrantState::Granted);
    std::ignore = co_await core::net::testing::waitUntil(
        loop,
        [h] {
            auto const status = h->world->status("orchestrator");
            return status && status->state == vthost::DaemonExtensionState::Running;
        },
        WaitTicks);
    window->client->detach();
}

/// The later client's part: waits until it mirrors the indicator row @p row, and leaves.
Task<void> mirrorAndLeave(core::net::EventLoop* loop, Connection* later, std::string row)
{
    std::ignore = co_await core::net::testing::waitUntil(
        loop,
        [later, &row] {
            return std::ranges::any_of(later->client->screens(), [&row](auto const& entry) {
                return indicatorRowOf(entry.second) == row;
            });
        },
        WaitTicks);
    later->client->detach();
}

} // namespace

TEST_CASE("DaemonWorld: a window grants, every window closes, the summary stays", "[daemon][e2e]")
{
    auto h = DaemonWorldHarness {};
    REQUIRE(h.world != nullptr);
    // Started at the daemon's start, it waits for its user: no command has run.
    REQUIRE(h.world->status("orchestrator")->state == vthost::DaemonExtensionState::Pending);
    CHECK(h.status().state == "pending");
    CHECK(h.processRunner.waiting() == 0);

    // A window that proved the daemon's consent key is asked, and grants; then it closes.
    auto window = h.connect(ClientKind::Window);
    h.loop.blockOn(core::net::testing::allOf(
        window->server->run(), window->client->run(), grantAndLeave(&h.loop, &h, window.get())));
    REQUIRE(window->log.requests.size() == 1);
    CHECK(window->log.requests.front().name == "orchestrator");
    window.reset();

    // No window is left. Within one poll -- every command it runs started once and answered -- it reports
    // running, with the run list it published.
    for ([[maybe_unused]] auto const _: std::views::iota(0, WaitTicks))
    {
        std::ignore = h.loop.runOnce(std::chrono::milliseconds { 1 });
        std::ignore = h.processRunner.answerWaiting();
        if (h.pollDone() && h.status().summary.starts_with("⬢ agents"))
            break;
    }
    for (auto const& answer: vtextension::testing::OrchestratorFixtureAnswers)
    {
        CAPTURE(answer.program, answer.verb);
        CHECK(h.processRunner.startsOf(answer.program, answer.verb) == 1);
    }
    auto const status = h.status();
    CHECK(status.state == "running");
    CHECK(status.problem.empty());
    REQUIRE(status.summary.starts_with("⬢ agents"));
    // The detail is the short text and the run list, one line each.
    CHECK(status.detail.starts_with(status.summary));

    // A client attaching afterwards -- one that answers nothing -- mirrors the row holding the summary.
    auto later = h.connect(ClientKind::Cli);
    auto const row = std::format("[{}]", status.summary);
    h.loop.blockOn(core::net::testing::allOf(
        later->server->run(), later->client->run(), mirrorAndLeave(&h.loop, later.get(), row)));
    auto const mirrored = std::ranges::any_of(
        later->client->screens(), [&row](auto const& entry) { return indicatorRowOf(entry.second) == row; });
    CHECK(mirrored);
    CHECK(later->log.requests.empty());
}

#endif
