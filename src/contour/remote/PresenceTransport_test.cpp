// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/askpass/AskpassPromptSink.hpp>
#include <contour/remote/PresenceTransport.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/KnownDaemons.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Transport.hpp>
#include <vthost/testing/CountingRandom.hpp>

using contour::askpass::AskpassRouteId;
using contour::cli::RemoteStart;
using contour::cli::SshPrompts;
using contour::config::HostEntry;
using contour::config::HostScheme;
using contour::config::HostUrl;
using contour::remote::FailureRecordingTransport;
using contour::remote::interactiveTransport;
using contour::remote::observerTransport;
using contour::window::DaemonLinkState;
using vthost::TransportErrorKind;

namespace
{

using ConnectResult = std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>;

/// @return A failed dial of @p kind.
[[nodiscard]] core::async::Task<ConnectResult> failedWith(TransportErrorKind kind)
{
    co_return std::unexpected { vthost::TransportError { .kind = kind, .message = "test" } };
}

/// A host whose login needs a password: with prompts it asks the window of its route through the real
/// router, as ssh's askpass helper would; without, it fails as batch mode does.
class PasswordHost final: public vthost::ITransportFactory
{
  public:
    PasswordHost(contour::askpass::AskpassPromptRouter& router, AskpassRouteId route, SshPrompts prompts):
        _router(&router), _route(route), _prompts(prompts)
    {
    }

    [[nodiscard]] core::async::Task<ConnectResult> connect(core::net::EventLoop& /*loop*/) override
    {
        if (_prompts == SshPrompts::Askpass)
            _router->ask(_route,
                         1,
                         contour::askpass::AskpassPromptKind::Secret,
                         QStringLiteral("ssh://me@starfive"),
                         QStringLiteral("Password:"),
                         [](contour::askpass::AskpassReply) {});
        return failedWith(TransportErrorKind::AuthenticationRequired);
    }

    [[nodiscard]] std::string describe() const override { return "ssh://me@starfive"; }

  private:
    contour::askpass::AskpassPromptRouter* _router;
    AskpassRouteId _route;
    SshPrompts _prompts;
};

/// A window's prompt sink: keeps what it is shown, and answers nothing.
class RecordingSink final: public contour::askpass::IAskpassPromptSink
{
  public:
    void ask(contour::askpass::AskpassPromptId promptId,
             contour::askpass::AskpassPromptKind /*kind*/,
             QString /*target*/,
             QString /*prompt*/,
             std::function<void(contour::askpass::AskpassReply)> /*reply*/) override
    {
        asked.push_back(promptId);
    }

    void withdraw(contour::askpass::AskpassPromptId /*promptId*/) override {}

    std::vector<contour::askpass::AskpassPromptId> asked;
};

/// How openHost was called.
struct OpenHostCall
{
    std::string host;
    AskpassRouteId route;
    SshPrompts prompts;
    RemoteStart remoteStart;
};

/// Records what a spawn would have run, and starts nothing.
class RecordingSpawner final: public vthost::IStdioSpawner
{
  public:
    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& /*loop*/,
        std::span<std::string const> argv,
        vthost::SpawnEnvironment const& /*environment*/) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                           .message = "not in this test" } };
    }

    std::vector<std::vector<std::string>> spawned;
};

[[nodiscard]] HostEntry sshStarfive()
{
    return HostEntry {
        .url = HostUrl { .scheme = HostScheme::Ssh, .user = "me", .host = "starfive", .port = std::nullopt },
        .tokenFile = {},
        .tlsCa = {},
        .identityFile = {},
        .remoteCommand = {},
        .profile = {}
    };
}

[[nodiscard]] HostEntry tlsStudio()
{
    return HostEntry { .url =
                           HostUrl { .scheme = HostScheme::Tls, .user = {}, .host = "studio", .port = 7297 },
                       .tokenFile = {},
                       .tlsCa = {},
                       .identityFile = {},
                       .remoteCommand = {},
                       .profile = {} };
}

/// The application side of a dial: one saved host, the calls made, a pin file in memory.
struct Dialer
{
    HostEntry host = sshStarfive();
    std::vector<OpenHostCall> opened;
    std::vector<vtworkspace::WorkspaceId> routesAsked;
    core::platform::testing::InMemoryFileSystem files;
    vthost::testing::CountingRandom random {};
    vthost::KnownDaemons known { files, "/pins/known_daemons", random };
    std::filesystem::path localSocket = "/nonexistent/contour/control";
    /// Makes the transport of an opened host; a stand-in that dials nothing unless a case says otherwise.
    std::function<std::expected<contour::cli::HostTransport, std::string>(OpenHostCall const&)> transportOf =
        [](OpenHostCall const&) -> std::expected<contour::cli::HostTransport, std::string> {
        return contour::cli::HostTransport { .factory = std::make_shared<vthost::UnixTransportFactory>(
                                                 "/nonexistent/contour/control", nullptr),
                                             .label = "starfive",
                                             .attach = vthost::AttachBudget { std::chrono::seconds { 1 } } };
    };

    [[nodiscard]] contour::remote::PresenceTransportDeps deps()
    {
        return contour::remote::PresenceTransportDeps {
            .savedHost = [this](std::string const& name) -> HostEntry const* {
                return name == "starfive" || name == "studio" ? &host : nullptr;
            },
            .openHost =
                [this](std::string const& name,
                       HostEntry const& /*entry*/,
                       AskpassRouteId route,
                       SshPrompts prompts,
                       RemoteStart remoteStart) {
                    opened.push_back(OpenHostCall {
                        .host = name, .route = route, .prompts = prompts, .remoteStart = remoteStart });
                    return transportOf(opened.back());
                },
            .localSocketOf = [this](std::string const& /*label*/) { return localSocket; },
            .knownDaemons = &known,
            .routeOf =
                [this](vtworkspace::WorkspaceId workspace) {
                    routesAsked.push_back(workspace);
                    return AskpassRouteId { 7 };
                },
        };
    }
};

/// @return What dialling @p transport once returns.
[[nodiscard]] ConnectResult dialOnce(vthost::ITransportFactory& transport)
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    return loop.blockOn(transport.connect(loop));
}

} // namespace

TEST_CASE("an observer transport never prompts, even with another window's askpass sink attached",
          "[presence-dial]")
{
    auto router = contour::askpass::AskpassPromptRouter {};
    auto sink = RecordingSink {};
    router.attach(AskpassRouteId { 7 }, sink);
    auto dialer = Dialer {};
    dialer.transportOf =
        [&router](OpenHostCall const& call) -> std::expected<contour::cli::HostTransport, std::string> {
        return contour::cli::HostTransport { .factory = std::make_shared<PasswordHost>(
                                                 router, call.route, call.prompts),
                                             .label = call.host,
                                             .attach = vthost::AttachBudget { std::chrono::seconds { 1 } } };
    };

    auto const transport = observerTransport(contour::window::hostDaemonRef("starfive"), dialer.deps());
    REQUIRE(transport.has_value());
    REQUIRE(dialer.opened.size() == 1);
    CHECK(dialer.opened[0].prompts == SshPrompts::Batch);
    CHECK(dialer.opened[0].route == contour::askpass::AnyWindow);
    CHECK(dialer.opened[0].remoteStart == RemoteStart::Never);
    CHECK(dialer.routesAsked.empty());

    auto const connection = dialOnce(**transport);
    REQUIRE_FALSE(connection.has_value());
    CHECK(contour::remote::linkStateOfFailure(connection.error().kind) == DaemonLinkState::NeedsInput);
    CHECK_FALSE(router.hasOpenPrompt());
    CHECK(sink.asked.empty());
    router.shutdown();
}

TEST_CASE("an interactive dial prompts in the asking window", "[presence-dial]")
{
    auto router = contour::askpass::AskpassPromptRouter {};
    auto other = RecordingSink {};
    auto asking = RecordingSink {};
    router.attach(AskpassRouteId { 3 }, other);
    router.attach(AskpassRouteId { 7 }, asking);
    auto dialer = Dialer {};
    dialer.transportOf =
        [&router](OpenHostCall const& call) -> std::expected<contour::cli::HostTransport, std::string> {
        return contour::cli::HostTransport { .factory = std::make_shared<PasswordHost>(
                                                 router, call.route, call.prompts),
                                             .label = call.host,
                                             .attach = vthost::AttachBudget { std::chrono::seconds { 1 } } };
    };

    auto const window = vtworkspace::WorkspaceId { 5 };
    auto const transport =
        interactiveTransport(contour::window::hostDaemonRef("starfive"), window, dialer.deps());
    REQUIRE(transport.has_value());
    REQUIRE(dialer.opened.size() == 2);
    CHECK(dialer.opened[0].prompts == SshPrompts::Askpass);
    CHECK(dialer.opened[0].route == AskpassRouteId { 7 });
    // What it connects is an observer: it starts no daemon, even when the user asked for it.
    CHECK(dialer.opened[0].remoteStart == RemoteStart::Never);
    CHECK(dialer.routesAsked == std::vector { window });

    std::ignore = dialOnce(**transport);
    CHECK(router.hasOpenPrompt());
    CHECK(asking.asked.size() == 1);
    CHECK(other.asked.empty());
    router.shutdown();
}

TEST_CASE("a connected observer's reconnect asks no one", "[presence-dial]")
{
    // The user asked once; a reconnect after a blip -- the window long closed, the user elsewhere -- is a
    // background dial, and dials as an observer does.
    auto router = contour::askpass::AskpassPromptRouter {};
    auto asking = RecordingSink {};
    router.attach(AskpassRouteId { 7 }, asking);
    auto dialer = Dialer {};
    dialer.transportOf =
        [&router](OpenHostCall const& call) -> std::expected<contour::cli::HostTransport, std::string> {
        return contour::cli::HostTransport { .factory = std::make_shared<PasswordHost>(
                                                 router, call.route, call.prompts),
                                             .label = call.host,
                                             .attach = vthost::AttachBudget { std::chrono::seconds { 1 } } };
    };

    auto const transport = interactiveTransport(
        contour::window::hostDaemonRef("starfive"), vtworkspace::WorkspaceId { 5 }, dialer.deps());
    REQUIRE(transport.has_value());
    REQUIRE(dialer.opened.size() == 2);
    CHECK(dialer.opened[1].prompts == SshPrompts::Batch);
    CHECK(dialer.opened[1].route == contour::askpass::AnyWindow);
    CHECK(dialer.opened[1].remoteStart == RemoteStart::Never);

    std::ignore = dialOnce(**transport);
    REQUIRE(asking.asked.size() == 1);
    for (auto const redial: { 1, 2 })
    {
        INFO("redial " << redial);
        auto const connection = dialOnce(**transport);
        REQUIRE_FALSE(connection.has_value());
        CHECK(contour::remote::linkStateOfFailure(connection.error().kind) == DaemonLinkState::NeedsInput);
    }
    CHECK(asking.asked.size() == 1);
    router.shutdown();
}

TEST_CASE("an unpinned TLS host is NeedsInput without dialing", "[presence-dial]")
{
    auto dialer = Dialer {};
    dialer.host = tlsStudio();

    auto const transport = observerTransport(contour::window::hostDaemonRef("studio"), dialer.deps());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().state == DaemonLinkState::NeedsInput);
    CHECK(transport.error().reason.contains("studio"));
    CHECK(dialer.opened.empty());

    // The user's own dial may ask, so it goes ahead.
    CHECK(interactiveTransport(
              contour::window::hostDaemonRef("studio"), vtworkspace::WorkspaceId { 1 }, dialer.deps())
              .has_value());
    // Its first dial may ask; its reconnects are an observer's, made ahead of them.
    REQUIRE(dialer.opened.size() == 2);
    CHECK(dialer.opened[0].prompts == SshPrompts::Askpass);
    CHECK(dialer.opened[1].prompts == SshPrompts::Batch);
}

TEST_CASE("a pinned or CA-verified TLS host is dialed", "[presence-dial]")
{
    auto dialer = Dialer {};
    dialer.host = tlsStudio();

    SECTION("pinned")
    {
        REQUIRE(dialer.known.pin(vthost::pinKey("studio", 7297), "SHA256:AAAA").has_value());
    }
    SECTION("verified by its CA")
    {
        dialer.host.tlsCa = "/home/me/.config/contour/studio-ca.pem";
    }

    auto const transport = observerTransport(contour::window::hostDaemonRef("studio"), dialer.deps());
    REQUIRE(transport.has_value());
    REQUIRE(dialer.opened.size() == 1);
    CHECK(dialer.opened[0].prompts == SshPrompts::Batch);
    CHECK(dialer.opened[0].remoteStart == RemoteStart::Never);
}

TEST_CASE("an unreadable pin file is Failed with its reason", "[presence-dial]")
{
    auto dialer = Dialer {};
    dialer.host = tlsStudio();
    dialer.files.addFile("/pins/known_daemons", "studio:7297 SHA256:AAAA\n");
    dialer.files.denyAccess("/pins/known_daemons");
    auto const unreadable = dialer.known.pinOf(vthost::pinKey("studio", 7297));
    REQUIRE_FALSE(unreadable.has_value());

    auto const transport = observerTransport(contour::window::hostDaemonRef("studio"), dialer.deps());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().state == DaemonLinkState::Failed);
    CHECK(transport.error().reason.contains(unreadable.error()));
    CHECK(dialer.opened.empty());
}

TEST_CASE("a local observer transport never starts its daemon", "[presence-dial]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-presence-dial" };
    auto dialer = Dialer {};
    dialer.localSocket = tmp.path() / "nobody";

    auto const transport = observerTransport(contour::window::localDaemonRef("work"), dialer.deps());
    REQUIRE(transport.has_value());
    auto const connection = dialOnce(**transport);
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::DaemonAbsent);
    CHECK(contour::remote::linkStateOfFailure(connection.error().kind) == DaemonLinkState::NotRunning);
    CHECK(dialer.opened.empty());
}

TEST_CASE("an ssh observer asks the relay not to start a daemon, through a custom remote_command too",
          "[presence-dial]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const spawner = std::make_shared<RecordingSpawner>();
    auto dialer = Dialer {};
    // The real mapping of a host onto its transport, without prompts: what is under test is the command.
    dialer.transportOf = [&](OpenHostCall const& call) {
        return contour::cli::makeHostTransport(call.host,
                                               dialer.host,
                                               fileSystem,
                                               spawner,
                                               contour::cli::SshPrompting {},
                                               contour::cli::TlsTrust {},
                                               call.remoteStart);
    };
    auto const ref = contour::window::hostDaemonRef("starfive");

    SECTION("the default command")
    {
        auto const observer = observerTransport(ref, dialer.deps());
        REQUIRE(observer.has_value());
        std::ignore = dialOnce(**observer);
        REQUIRE(spawner->spawned.size() == 1);
        CHECK(spawner->spawned[0].back() == vthost::NoStartRemoteCommand);
        CHECK(std::ranges::find(spawner->spawned[0], "BatchMode=yes") != spawner->spawned[0].end());

        // Nor does the user's Connect: what it connects is an observer too.
        auto const interactive = interactiveTransport(ref, vtworkspace::WorkspaceId { 1 }, dialer.deps());
        REQUIRE(interactive.has_value());
        std::ignore = dialOnce(**interactive);
        REQUIRE(spawner->spawned.size() == 2);
        CHECK(spawner->spawned[1].back() == vthost::NoStartRemoteCommand);
    }
    SECTION("a custom remote_command")
    {
        dialer.host.remoteCommand = "/opt/contour/bin/contour daemon --stdio";
        auto const observer = observerTransport(ref, dialer.deps());
        REQUIRE(observer.has_value());
        std::ignore = dialOnce(**observer);
        REQUIRE(spawner->spawned.size() == 1);
        CHECK(spawner->spawned[0].back() == "/opt/contour/bin/contour daemon --stdio --no-start");
    }
}

TEST_CASE("a daemon no dial can reach is Failed", "[presence-dial]")
{
    auto dialer = Dialer {};
    for (auto const& ref: { contour::window::linkDaemonRef(4),
                            contour::window::hostDaemonRef("nowhere"),
                            contour::window::DaemonRef { "bogus" } })
    {
        INFO(ref.value);
        auto const transport = observerTransport(ref, dialer.deps());
        REQUIRE_FALSE(transport.has_value());
        CHECK(transport.error().state == DaemonLinkState::Failed);
    }
    CHECK(dialer.opened.empty());
}

TEST_CASE("every transport failure maps to a link state or a retry", "[presence-dial]")
{
    struct Row
    {
        TransportErrorKind kind;
        std::optional<DaemonLinkState> state;
    };
    constexpr auto Rows = std::to_array<Row>({
        { .kind = TransportErrorKind::Unreachable, .state = std::nullopt },
        { .kind = TransportErrorKind::Refused, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::AuthenticationRequired, .state = DaemonLinkState::NeedsInput },
        { .kind = TransportErrorKind::HostKeyUnknown, .state = DaemonLinkState::NeedsInput },
        { .kind = TransportErrorKind::RemoteProgramMissing, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::ProgramNotFound, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::Protocol, .state = std::nullopt },
        { .kind = TransportErrorKind::Cancelled, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::DaemonAbsent, .state = DaemonLinkState::NotRunning },
        { .kind = TransportErrorKind::Misconfigured, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::HostKeyChanged, .state = DaemonLinkState::Failed },
        { .kind = TransportErrorKind::RemoteProgramTooOld, .state = DaemonLinkState::TooOld },
    });
    static_assert(Rows.size() == vthost::TransportErrorKindCount);
    for (auto const& row: Rows)
    {
        INFO("transport error kind " << static_cast<int>(row.kind));
        CHECK(contour::remote::linkStateOfFailure(row.kind) == row.state);
    }
}

namespace
{

/// A transport that answers each dial from a script, and records what it is told.
class ScriptedTransport final: public vthost::ITransportFactory
{
  public:
    /// @param script Each dial's failure, in order; nullopt for a dial that succeeds.
    explicit ScriptedTransport(std::deque<std::optional<TransportErrorKind>> script):
        _script(std::move(script))
    {
    }

    [[nodiscard]] core::async::Task<ConnectResult> connect(core::net::EventLoop& loop) override
    {
        auto const next = _script.front();
        _script.pop_front();
        if (next)
            return failedWith(*next);
        auto pair = core::net::testing::makeSocketPair(loop);
        _peers.push_back(std::move(pair->first));
        return connected(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair->second), .token = {} }));
    }

    [[nodiscard]] std::string describe() const override { return "scripted:"; }

    void holdDaemonStart(core::platform::SteadyTimePoint until) override { held.push_back(until); }

    std::vector<core::platform::SteadyTimePoint> held;

  private:
    [[nodiscard]] static core::async::Task<ConnectResult> connected(
        std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }

    std::deque<std::optional<TransportErrorKind>> _script;
    std::vector<std::unique_ptr<core::net::ISocket>> _peers;
};

} // namespace

TEST_CASE("the recording transport remembers the last failure and forgets it on success", "[presence-dial]")
{
    // The loop outlives the transport, which keeps the far ends of the connections it made on it.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const inner = std::make_shared<ScriptedTransport>(std::deque<std::optional<TransportErrorKind>> {
        TransportErrorKind::Unreachable, TransportErrorKind::DaemonAbsent, std::nullopt });
    auto recording = FailureRecordingTransport { inner };
    CHECK_FALSE(recording.lastFailure().has_value());
    CHECK(recording.describe() == "scripted:");
    auto const until = core::platform::SteadyTimePoint {} + std::chrono::seconds { 3 };
    recording.holdDaemonStart(until);
    CHECK(inner->held == std::vector { until });

    CHECK_FALSE(loop.blockOn(recording.connect(loop)).has_value());
    CHECK(recording.lastFailure() == TransportErrorKind::Unreachable);
    CHECK_FALSE(loop.blockOn(recording.connect(loop)).has_value());
    CHECK(recording.lastFailure() == TransportErrorKind::DaemonAbsent);
    CHECK(loop.blockOn(recording.connect(loop)).has_value());
    CHECK_FALSE(recording.lastFailure().has_value());
}

TEST_CASE("an observer whose daemon is gone dials it no more by itself", "[presence-dial]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const inner = std::make_shared<ScriptedTransport>(
        std::deque<std::optional<TransportErrorKind>> { TransportErrorKind::DaemonAbsent,
                                                        TransportErrorKind::DaemonAbsent,
                                                        TransportErrorKind::Unreachable });
    auto recording = FailureRecordingTransport { inner };

    // Handed on as a failure the reconnect does not retry; the row reads the absence.
    auto const gone = loop.blockOn(recording.connect(loop));
    REQUIRE_FALSE(gone.has_value());
    CHECK(vthost::retryOf(gone.error().kind) == vthost::TransportRetry::Permanent);
    CHECK(recording.lastFailure() == TransportErrorKind::DaemonAbsent);

    // A daemon that announced its upgrade comes back by itself: it is waited for.
    recording.holdDaemonStart(loop.clock().now() + std::chrono::hours { 1 });
    auto const upgrading = loop.blockOn(recording.connect(loop));
    REQUIRE_FALSE(upgrading.has_value());
    CHECK(upgrading.error().kind == TransportErrorKind::DaemonAbsent);
    CHECK(vthost::retryOf(upgrading.error().kind) == vthost::TransportRetry::Transient);

    // Any other failure is handed on as it is.
    recording.holdDaemonStart(core::platform::SteadyTimePoint::min());
    auto const unreachable = loop.blockOn(recording.connect(loop));
    REQUIRE_FALSE(unreachable.has_value());
    CHECK(unreachable.error().kind == TransportErrorKind::Unreachable);
}
