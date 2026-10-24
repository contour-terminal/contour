// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/DaemonRuntime.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/ShutdownFunnel.hpp>

namespace
{

/// A host over MockPty sessions with pump threads disabled, holding one session.
struct RuntimeHarness
{
    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };

    RuntimeHarness() { REQUIRE(host.createTab() != nullptr); }
};

/// A listener that accepts nothing and records its close into a shared event log.
struct RecordingListener final: core::net::IListener
{
    std::vector<std::string>* events;

    // Not noexcept: IListener's constructor allocates its close token's state.
    explicit RecordingListener(std::vector<std::string>* eventsArg): events(eventsArg) {}

    core::async::Task<core::net::AcceptResult> accept() override
    {
        co_return std::unexpected(core::net::NetError {
            .code = core::net::NetErrorCode::Cancelled, .systemCode = 0, .context = "accept" });
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }

  protected:
    void doClose() noexcept override { events->emplace_back("listener closed"); }
};

} // namespace

TEST_CASE("DaemonRuntime reports what it was configured with and its uptime", "[vthost][runtime]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto runtime = vthost::DaemonRuntime { { .releaseVersion = "0.8.1",
                                             .processId = 4242,
                                             .socketPath = "/run/c/default",
                                             .executablePath = "/s/bin/0.8.1-x/bin/contour",
                                             .logFile = "/run/c/default.log",
                                             .lifecycle = vthost::DaemonLifecycle::ExitWhenIdle },
                                           h.host,
                                           clock,
                                           [](vthost::ShutdownCause) {} };
    clock.advance(std::chrono::seconds { 7385 });
    auto const ticket = runtime.admitClient();
    auto const status = runtime.status();
    REQUIRE(status.has_value());
    auto const& s = *status;
    CHECK(s.releaseVersion == "0.8.1");
    CHECK(runtime.releaseVersion() == "0.8.1");
    CHECK(s.processId == 4242);
    CHECK(s.uptimeSeconds == 7385);
    CHECK(s.socketPath == "/run/c/default");
    CHECK(s.executablePath == "/s/bin/0.8.1-x/bin/contour");
    CHECK(s.sessions == 1);
    CHECK(s.clients == 1);
    CHECK(s.lifecycle == std::to_underlying(vthost::DaemonLifecycle::ExitWhenIdle));
    CHECK(s.logFile == "/run/c/default.log");
    CHECK(runtime.census().count() == 1);
}

TEST_CASE("a stop request maps its reason onto a shutdown cause", "[vthost][runtime]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto causes = std::vector<vthost::ShutdownCause> {};
    auto runtime =
        vthost::DaemonRuntime { {}, h.host, clock, [&](vthost::ShutdownCause c) { causes.push_back(c); } };
    CHECK(runtime.requestStop(vthost::proto::StopReason::User).has_value());
    CHECK(runtime.requestStop(vthost::proto::StopReason::UpgradeRestart).has_value());
    CHECK(causes
          == std::vector { vthost::ShutdownCause::StopRequested, vthost::ShutdownCause::UpgradeRestart });
}

TEST_CASE("a stop that keeps the workspace maps to its own cause", "[vthost][daemon]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto causes = std::vector<vthost::ShutdownCause> {};
    auto runtime =
        vthost::DaemonRuntime { {}, h.host, clock, [&](vthost::ShutdownCause c) { causes.push_back(c); } };
    CHECK(runtime.requestStop(vthost::proto::StopReason::UserKeepingWorkspace).has_value());
    CHECK(causes == std::vector { vthost::ShutdownCause::StopRequestedKeepingWorkspace });
    CHECK(vthost::nameOf(vthost::ShutdownCause::StopRequestedKeepingWorkspace)
          == "stop requested, keeping the workspace");
}

TEST_CASE("every lifecycle round-trips through its CLI spelling", "[vthost][runtime]")
{
    for (auto const& [text, value]: vthost::DaemonLifecycleNames)
    {
        CHECK(vthost::daemonLifecycleFrom(text) == value);
        CHECK(vthost::nameOf(value) == text);
    }
    CHECK_FALSE(vthost::daemonLifecycleFrom("Persistent").has_value());
}

TEST_CASE("every shutdown cause has its own log text", "[vthost][runtime]")
{
    for (auto const& [text, value]: vthost::ShutdownCauseNames)
        CHECK(vthost::nameOf(value) == text);
    CHECK(vthost::nameOf(vthost::ShutdownCause::LastSessionClosed) == "the last hosted session closed");
}

TEST_CASE("the shutdown funnel runs beforeShutdown once, before any listener closes", "[vthost][runtime]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto events = std::vector<std::string> {};
    auto servers = std::vector<vthost::ConnectionAcceptor*> {};
    auto funnel = vthost::ShutdownFunnel { loop, servers, [&](vthost::ShutdownCause cause) {
                                              events.emplace_back(vthost::nameOf(cause));
                                          } };
    // Bound after the funnel was made: the funnel reads the list as it is when the shutdown comes.
    auto acceptor = vthost::ConnectionAcceptor { loop,
                                                 "recording",
                                                 std::make_unique<RecordingListener>(&events),
                                                 [](vthost::ConnectionId, std::unique_ptr<core::net::ISocket>)
                                                     -> core::async::Task<void> { co_return; } };
    servers.push_back(&acceptor);
    CHECK(funnel.state() == vthost::ShutdownState::Serving);

    funnel.request(vthost::ShutdownCause::StopRequested);
    funnel.request(vthost::ShutdownCause::Signal); // a second request does nothing at all

    CHECK(funnel.state() == vthost::ShutdownState::ShuttingDown);
    CHECK(events == std::vector<std::string> { "stop requested", "listener closed" });
}

TEST_CASE("the shutdown funnel needs no beforeShutdown", "[vthost][runtime]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto const servers = std::vector<vthost::ConnectionAcceptor*> {};
    auto funnel = vthost::ShutdownFunnel { loop, servers, {} };
    funnel.request(vthost::ShutdownCause::Idle);
    CHECK(funnel.state() == vthost::ShutdownState::ShuttingDown);
}

TEST_CASE("a shutdown requested after the endpoints are gone closes none of them",
          "[vthost][runtime][shutdown]")
{
    // runDaemon empties its endpoint list once the loop has unwound, because the acceptors are destroyed
    // before its drain turns the loop again; a request still queued then must not reach them.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto events = std::vector<std::string> {};
    auto servers = std::vector<vthost::ConnectionAcceptor*> {};
    auto funnel = vthost::ShutdownFunnel { loop, servers, [&](vthost::ShutdownCause cause) {
                                              events.emplace_back(vthost::nameOf(cause));
                                          } };
    auto acceptor = vthost::ConnectionAcceptor { loop,
                                                 "recording",
                                                 std::make_unique<RecordingListener>(&events),
                                                 [](vthost::ConnectionId, std::unique_ptr<core::net::ISocket>)
                                                     -> core::async::Task<void> { co_return; } };
    servers.push_back(&acceptor);
    servers.clear();

    funnel.request(vthost::ShutdownCause::Signal);

    CHECK(events == std::vector<std::string> { "signal received" });
}
