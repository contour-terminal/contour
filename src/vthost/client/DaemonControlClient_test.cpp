// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <vthost/DaemonControl.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>

namespace proto = vthost::proto;
namespace client = vthost::client;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

/// A daemon that records the stops it is asked for, and from the first one on hangs up on the connections it
/// has. Then it either refuses every new connection (as a real daemon's shutdown does), or keeps accepting.
class StoppableDaemon final: public vthost::DaemonControl
{
  public:
    enum class OnStop : uint8_t
    {
        KeepAccepting = 0, ///< A daemon that ignores the request.
        GoAway,            ///< A daemon that stops serving.
    };

    explicit StoppableDaemon(OnStop onStop): _onStop(onStop) {}

    [[nodiscard]] std::expected<proto::DaemonStatusReply, vthost::DaemonControlError> status() const override
    {
        ++_statusRequests;
        auto reply = *_inner.status();
        reply.lifecycle = std::to_underlying(vthost::DaemonLifecycle::ExitWhenIdle);
        reply.processId = 4711;
        reply.uptimeSeconds = 90;
        reply.socketPath = "/run/contour/control";
        reply.sessions = 2;
        reply.clients = 1;
        return reply;
    }

    [[nodiscard]] std::string_view releaseVersion() const noexcept override
    {
        return _inner.releaseVersion();
    }

    [[nodiscard]] std::expected<void, vthost::DaemonControlError> requestStop(
        proto::StopReason reason) override
    {
        std::ignore = _inner.requestStop(reason);
        _stopRequested = true;
        if (_onStop == OnStop::GoAway)
            _gone = true;
        return {};
    }

    [[nodiscard]] vthost::ClientCensus::Ticket admitClient() override { return _inner.admitClient(); }

    /// @return Whether it refuses new connections.
    [[nodiscard]] bool gone() const noexcept { return _gone; }
    /// @return Whether it was asked to stop, which makes it hang up on the connections it has.
    [[nodiscard]] bool stopRequested() const noexcept { return _stopRequested; }
    /// @return How many status requests reached it.
    [[nodiscard]] std::size_t statusRequests() const noexcept { return _statusRequests; }
    [[nodiscard]] std::vector<proto::StopReason> const& stops() const noexcept { return _inner.stops; }

  private:
    vthost::testing::FakeDaemonControl _inner;
    OnStop _onStop;
    bool _gone = false;
    bool _stopRequested = false;
    mutable std::size_t _statusRequests = 0;
};

/// One loop, a session host, and a daemon whose connections are in-memory pairs served by real
/// NativeSessions.
struct ControlHarness
{
    explicit ControlHarness(
        StoppableDaemon::OnStop onStop = StoppableDaemon::OnStop::GoAway,
        proto::CapabilitySet offered = proto::CapabilitySet::of({ proto::Capability::DaemonControl })):
        daemon(onStop), capabilities(std::move(offered))
    {
    }

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    StoppableDaemon daemon;
    proto::CapabilitySet capabilities;
    std::vector<std::unique_ptr<vthost::NativeSession>> sessions;
    std::size_t connections = 0;
    /// After the sessions, so destroyed before them: every run() serving one ends while it still exists.
    vthost::LoopDrain drain { loop };

    /// @return A connector handing out one end of a fresh pair, a NativeSession serving the other, until
    ///         the daemon has gone away -- then it refuses, in the words the OS uses.
    [[nodiscard]] client::SocketDaemonControl::Connector connector()
    {
        return [this] {
            return connect();
        };
    }

    /// Hands out one end of a fresh pair, a NativeSession serving the other, until the daemon is gone.
    core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, client::ControlError>> connect()
    {
        if (daemon.gone())
            co_return std::unexpected { client::ControlError { client::ControlErrorCode::NotRunning,
                                                               "refused" } };
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto* serverEnd = pair.first.get();
        sessions.push_back(std::make_unique<vthost::NativeSession>(
            loop,
            host,
            vthost::ConnectionId { .endpoint = "test", .index = ++connections },
            std::move(pair.first),
            vthost::NativeSession::DefaultWriteQueueBytes,
            vthost::NativeSessionServices { .handshake = { .capabilities = capabilities },
                                            .daemon = daemon }));
        loop.spawn(runAndHangUpWhenGone(sessions.back().get(), serverEnd));
        co_return std::move(pair.second);
    }

    /// Serves @p session, and closes its socket once the daemon has gone away, as the daemon's shutdown does.
    core::async::Task<void> runAndHangUpWhenGone(vthost::NativeSession* session, core::net::ISocket* socket)
    {
        auto watcher = core::net::pollUntil(&loop, [this] { return daemon.stopRequested(); });
        loop.spawn(closeWhenGone(std::move(watcher), socket));
        co_await session->run();
    }

    static core::async::Task<void> closeWhenGone(core::async::Task<void> gone, core::net::ISocket* socket)
    {
        co_await std::move(gone);
        socket->close();
    }
};

} // namespace

TEST_CASE("status() over a Control connection returns the daemon's report", "[vthost][client][control]")
{
    auto h = ControlHarness {};
    auto port = client::SocketDaemonControl { h.loop, h.connector(), 2s };
    auto const status = port.status();
    REQUIRE(status.has_value());
    CHECK(status->releaseVersion == "9.9.9");
    CHECK(status->processId == 4711);
    CHECK(status->uptime == 90s);
    CHECK(status->socketPath == fs::path { "/run/contour/control" });
    CHECK(status->sessions == 2);
    CHECK(status->clients == 1);
    CHECK(status->lifecycle == vthost::DaemonLifecycle::ExitWhenIdle);
    CHECK(h.host.sessionCount() == 0);
}

TEST_CASE("a status says what the daemon offered", "[vthost][client][control]")
{
    auto const offered =
        proto::CapabilitySet::of({ proto::Capability::DaemonControl, proto::Capability::WorkspaceRestore });
    auto h = ControlHarness { StoppableDaemon::OnStop::GoAway, offered };
    auto port = client::SocketDaemonControl { h.loop, h.connector(), 2s };
    auto const status = port.status();
    REQUIRE(status.has_value());
    CHECK(status->capabilities.contains(proto::Capability::WorkspaceRestore));
    CHECK(client::DefaultStopTimeout > client::DefaultControlTimeout);
}

TEST_CASE("a daemon without DaemonControl is Unsupported, not unreachable", "[vthost][client][control]")
{
    auto h = ControlHarness { StoppableDaemon::OnStop::GoAway, proto::CapabilitySet {} };
    auto port = client::SocketDaemonControl { h.loop, h.connector(), 2s };
    auto const status = port.status();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == client::ControlErrorCode::Unsupported);
    CHECK(status.error().detail.contains("9.9.9")); // names its release
    CHECK(h.daemon.stops().empty());
    CHECK(h.daemon.statusRequests() == 0); // closed at once: no request followed the hello
}

TEST_CASE("stop() succeeds once the daemon refuses new connections", "[vthost][client][control]")
{
    auto h = ControlHarness {};
    auto port = client::SocketDaemonControl { h.loop, h.connector(), 2s };
    auto const stopped = port.stop(proto::StopReason::User);
    REQUIRE(stopped.has_value());
    CHECK(h.daemon.stops() == std::vector<proto::StopReason> { proto::StopReason::User });
}

TEST_CASE("stop() times out against a daemon that keeps accepting", "[vthost][client][control]")
{
    auto h = ControlHarness { StoppableDaemon::OnStop::KeepAccepting };
    auto port = client::SocketDaemonControl { h.loop, h.connector(), 100ms };
    auto const stopped = port.stop(proto::StopReason::UpgradeRestart);
    REQUIRE_FALSE(stopped.has_value());
    INFO(stopped.error().detail);
    CHECK(stopped.error().code == client::ControlErrorCode::Timeout);
    // The stop connection plus reconnects: the timeout came out of the reconnect loop, not the EOF wait.
    CHECK(h.connections >= 3);
}

TEST_CASE("a connector that reports an absent daemon reads as not running", "[vthost][client][control]")
{
    auto h = ControlHarness {};
    auto const absent = client::SocketDaemonControl::Connector {
        []() -> core::async::Task<std::expected<std::unique_ptr<core::net::ISocket>, client::ControlError>> {
            co_return std::unexpected { client::ControlError { client::ControlErrorCode::NotRunning,
                                                               "No such file or directory" } };
        }
    };
    auto port = client::SocketDaemonControl { h.loop, absent, 2s };
    CHECK(port.status().error().code == client::ControlErrorCode::NotRunning);
    CHECK(port.stop(proto::StopReason::User).error().code == client::ControlErrorCode::NotRunning);
}

TEST_CASE("a wire status with an unknown lifecycle is not a status", "[vthost][client][control]")
{
    CHECK(client::fromWire(proto::DaemonStatusReply { .lifecycle = 200 }) == std::nullopt);
    CHECK(client::fromWire(proto::DaemonStatusReply { .lifecycle = 2 }).has_value());
}

TEST_CASE("control errors read as sentences", "[vthost][client][control]")
{
    CHECK(client::ControlError { client::ControlErrorCode::Refused, "protocol mismatch" }.toString()
          == "protocol mismatch");
    CHECK(client::ControlError { client::ControlErrorCode::NotRunning, "" }.toString()
          == "no daemon is running");
    CHECK(client::ControlError { client::ControlErrorCode::Unsupported, "an earlier release" }.toString()
          == "the daemon does not support this request (an earlier release)");
}

TEST_CASE("an absent socket reads as not running", "[vthost][client][control][afunix]")
{
    auto const directory = fs::temp_directory_path() / std::format("contour-ctl-{}", std::random_device {}());
    auto port = client::makeDaemonControl(
        std::make_shared<vthost::UnixTransportFactory>(directory / "nobody", nullptr));
    auto const status = port->status();
    REQUIRE_FALSE(status.has_value());
    INFO(status.error().toString());
    CHECK(status.error().code == client::ControlErrorCode::NotRunning);
}

TEST_CASE("a refused TCP connection reads as not running", "[vthost][client][control]")
{
    // Bind to learn a free port, then release it: nothing listens there any more.
    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto port = std::uint16_t { 0 };
    {
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        port = (*listener)->boundPort();
    }
    // Windows takes about two seconds to give up on a refused connection, retrying it first: the call's
    // timeout must outlast that, or what this reads is the timeout.
    auto control =
        client::makeDaemonControl(std::make_shared<vthost::TlsTransportFactory>(vthost::TcpEndpoint {
                                      .host = "127.0.0.1", .port = port, .token = {}, .caPem = {} }),
                                  std::chrono::seconds { 10 });
    auto const status = control->status();
    REQUIRE_FALSE(status.has_value());
    INFO(status.error().toString());
    CHECK(status.error().code == client::ControlErrorCode::NotRunning);
}

TEST_CASE("a TLS setup failure is a transport error, not an absent daemon", "[vthost][client][control]")
{
    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto control = client::makeDaemonControl(
        std::make_shared<vthost::TlsTransportFactory>(vthost::TcpEndpoint { .host = "127.0.0.1",
                                                                            .port = (*listener)->boundPort(),
                                                                            .token = {},
                                                                            .caPem = "not a certificate" }));
    auto const status = control->status();
    REQUIRE_FALSE(status.has_value());
    INFO(status.error().toString());
    CHECK(status.error().code == client::ControlErrorCode::Transport);
}

TEST_CASE("daemon control over a unix transport never starts a daemon",
          "[vthost][transport][control][afunix]")
{
    auto const directory = fs::temp_directory_path() / std::format("contour-ctl-{}", std::random_device {}());
    auto const controlSocket = directory / "control";
    // The factory every probe passes: an empty ensureDaemon, so there is nothing that could start one.
    auto port =
        client::makeDaemonControl(std::make_shared<vthost::UnixTransportFactory>(controlSocket, nullptr));
    auto const status = port->status();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == client::ControlErrorCode::NotRunning);
    // A daemon started on the probe's behalf would have made its socket directory.
    CHECK_FALSE(fs::exists(directory));
}
