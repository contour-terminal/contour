// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/ConnectionRegistry.hpp>
#include <vthost/DaemonLifecycle.hpp>
#include <vthost/DaemonRuntime.hpp>
#include <vthost/DaemonUpgradeHooks.hpp>
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

/// An upgrade orchestrator that records its target, announces the upgrade and then refuses it. A free
/// function taking pointers, not a capturing lambda: a lambda coroutine's closure dies at the end of
/// the full-expression that created it, while the coroutine frame lives on (clang-tidy's
/// avoid-capturing-lambda-coroutines).
core::async::Task<std::expected<void, vthost::handover::UpgradeError>> refuseUpgradeAfterAnnouncing(
    vthost::DaemonUpgradeHooks* hooks,
    std::vector<std::string>* targets,
    vthost::handover::UpgradeState* state,
    vthost::handover::UpgradeTarget target)
{
    targets->push_back(target.imageId);
    *state = vthost::handover::UpgradeState::Running;
    // The orchestrator announces once it is certain to proceed.
    hooks->announceUpgrade("1.2.3");
    co_return std::unexpected(
        vthost::handover::UpgradeError { vthost::handover::UpgradeRefusal::QuiesceTimedOut });
}

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
    CHECK(runtime.requestStop(vthost::proto::StopReason::UserKeepingWorkspaces).has_value());
    CHECK(causes == std::vector { vthost::ShutdownCause::StopRequestedKeepingWorkspaces });
    CHECK(vthost::nameOf(vthost::ShutdownCause::StopRequestedKeepingWorkspaces)
          == "stop requested, keeping the workspaces");
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

TEST_CASE("an upgrade request without a port is refused NotSupported", "[vthost][runtime][upgrade]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto runtime = vthost::DaemonRuntime { {}, h.host, clock, [](vthost::ShutdownCause) {} };
    auto result = std::optional<std::expected<void, vthost::handover::UpgradeError>> {};
    h.loop.spawn([](vthost::DaemonRuntime* daemon, auto* out) -> core::async::Task<void> {
        *out = co_await daemon->requestUpgrade({}, {});
    }(&runtime, &result));
    std::ignore = h.loop.runUntilIdle();
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    CHECK(std::get<vthost::handover::UpgradeRefusal>(result->error())
          == vthost::handover::UpgradeRefusal::NotSupported);
}

TEST_CASE("a second upgrade request while one runs is refused InProgress", "[vthost][runtime][upgrade]")
{
    using vthost::handover::UpgradeRefusal;
    using vthost::handover::UpgradeState;
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto const connections = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { connections, {} };
    auto state = UpgradeState::Idle;
    auto targets = std::vector<std::string> {};
    auto runtime = vthost::DaemonRuntime { {},
                                           h.host,
                                           clock,
                                           [](vthost::ShutdownCause) {},
                                           vthost::DaemonUpgradePort {
                                               .hooks = hooks,
                                               .run =
                                                   [&](vthost::handover::UpgradeTarget target) {
                                                       return refuseUpgradeAfterAnnouncing(
                                                           &hooks, &targets, &state, std::move(target));
                                                   },
                                               .state = [&] { return state; } } };
    auto answered = 0;
    auto first = std::optional<std::expected<void, vthost::handover::UpgradeError>> {};
    h.loop.spawn([](vthost::DaemonRuntime* daemon, auto* out, int* count) -> core::async::Task<void> {
        *out = co_await daemon->requestUpgrade({ .imageId = "a" }, [count](std::string_view) { ++*count; });
    }(&runtime, &first, &answered));
    std::ignore = h.loop.runUntilIdle();
    REQUIRE(first.has_value());
    CHECK(answered == 1);
    CHECK(targets == std::vector<std::string> { "a" });

    // The upgrade is under way (state stays Running as it would until the exec), so a second asker is
    // refused.
    auto second = std::optional<std::expected<void, vthost::handover::UpgradeError>> {};
    h.loop.spawn([](vthost::DaemonRuntime* daemon, auto* out) -> core::async::Task<void> {
        *out = co_await daemon->requestUpgrade({ .imageId = "b" }, {});
    }(&runtime, &second));
    std::ignore = h.loop.runUntilIdle();
    REQUIRE(second.has_value());
    REQUIRE_FALSE(second->has_value());
    CHECK(std::get<UpgradeRefusal>(second->error()) == UpgradeRefusal::InProgress);
    CHECK(targets == std::vector<std::string> { "a" });
}

namespace
{

/// An upgrade orchestrator that refuses before it announces anything, as a readiness probe that fails does.
core::async::Task<std::expected<void, vthost::handover::UpgradeError>> refuseUpgradeBeforeAnnouncing(
    vthost::handover::UpgradeTarget /*target*/)
{
    co_return std::unexpected(
        vthost::handover::UpgradeError { vthost::handover::UpgradeRefusal::ConfigUnreadable });
}

/// An upgrade orchestrator that announces the upgrade and then throws, as a failing rollback would.
core::async::Task<std::expected<void, vthost::handover::UpgradeError>> throwUpgradeAfterAnnouncing(
    vthost::DaemonUpgradeHooks* hooks)
{
    hooks->announceUpgrade("1.2.3");
    throw std::runtime_error { "boom" };
    co_return {};
}

/// Runs one upgrade request on @p runtime to its end.
void requestUpgradeOn(RuntimeHarness& h, vthost::DaemonRuntime& runtime)
{
    auto result = std::optional<std::expected<void, vthost::handover::UpgradeError>> {};
    h.loop.spawn([](vthost::DaemonRuntime* daemon, auto* out) -> core::async::Task<void> {
        *out = co_await daemon->requestUpgrade({ .imageId = "a" }, [](std::string_view) {});
    }(&runtime, &result));
    std::ignore = h.loop.runUntilIdle();
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
}

} // namespace

TEST_CASE("a fresh daemon states upgrade generation 0 and no failure", "[vthost][runtime][upgrade]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto const runtime = vthost::DaemonRuntime { {}, h.host, clock, [](vthost::ShutdownCause) {} };
    auto const status = runtime.status();
    REQUIRE(status.has_value());
    CHECK(status->upgrade == vthost::proto::DaemonUpgradeRecord {});
}

TEST_CASE("an upgrade that fails after its announcement is recorded at the same generation",
          "[vthost][runtime][upgrade]")
{
    using vthost::handover::UpgradeRefusal;
    using vthost::handover::UpgradeState;
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto const connections = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { connections, {} };
    auto state = UpgradeState::Idle;
    auto targets = std::vector<std::string> {};
    auto announcing = true;
    auto runtime = vthost::DaemonRuntime {
        { .releaseVersion = "0.9.0",
          .processId = 1,
          .socketPath = {},
          .executablePath = {},
          .logFile = {},
          .lifecycle = vthost::DaemonLifecycle::Persistent,
          .upgradeGeneration = 3 },
        h.host,
        clock,
        [](vthost::ShutdownCause) {},
        vthost::DaemonUpgradePort { .hooks = hooks,
                                    .run =
                                        [&](vthost::handover::UpgradeTarget target) {
                                            if (!announcing)
                                                return refuseUpgradeBeforeAnnouncing(std::move(target));
                                            return refuseUpgradeAfterAnnouncing(
                                                &hooks, &targets, &state, std::move(target));
                                        },
                                    .state = [&] { return state; } }
    };

    requestUpgradeOn(h, runtime);
    state = UpgradeState::Idle;
    auto const failed = runtime.status();
    REQUIRE(failed.has_value());
    REQUIRE(failed->upgrade.has_value());
    // The requester was answered "started" and is gone: the status is where the failure is read.
    CHECK(failed->upgrade->generation == 3);
    CHECK(failed->upgrade->lastFailure == std::to_underlying(UpgradeRefusal::QuiesceTimedOut) + 1);
    CHECK(failed->upgrade->lastFailureDetail == vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut));

    CHECK(failed->upgrade->attempts == 1);

    SECTION("a refusal before the announcement is its requester's answer, and is counted")
    {
        announcing = false;
        requestUpgradeOn(h, runtime);
        // The stale failure is gone, so that no client takes it for this request's.
        auto const after = runtime.status();
        CHECK(after->upgrade->generation == 3);
        CHECK(after->upgrade->attempts == 2);
        CHECK(after->upgrade->lastFailure == 0);
    }

    SECTION("the next announcement forgets it")
    {
        // This orchestrator fails again after it: the record then names the new failure only. Read inside the
        // announcement, the status already has none.
        auto seen = std::optional<vthost::proto::DaemonUpgradeRecord> {};
        auto result = std::optional<std::expected<void, vthost::handover::UpgradeError>> {};
        h.loop.spawn([](vthost::DaemonRuntime* daemon, auto* out, auto* seenOut) -> core::async::Task<void> {
            *out = co_await daemon->requestUpgrade({ .imageId = "b" }, [daemon, seenOut](std::string_view) {
                *seenOut = daemon->status()->upgrade;
            });
        }(&runtime, &result, &seen));
        std::ignore = h.loop.runUntilIdle();
        REQUIRE(seen.has_value());
        CHECK(seen->lastFailure == 0);
        CHECK(seen->lastFailureDetail.empty());
        CHECK(seen->generation == 3);
    }
}

TEST_CASE("an upgrade that throws after its announcement is recorded as an exec failure",
          "[vthost][runtime][upgrade]")
{
    using vthost::handover::UpgradeState;
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto const connections = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { connections, {} };
    auto state = UpgradeState::Idle;
    auto runtime = vthost::DaemonRuntime { {},
                                           h.host,
                                           clock,
                                           [](vthost::ShutdownCause) {},
                                           vthost::DaemonUpgradePort {
                                               .hooks = hooks,
                                               .run =
                                                   [&](vthost::handover::UpgradeTarget const& /*target*/) {
                                                       return throwUpgradeAfterAnnouncing(&hooks);
                                                   },
                                               .state = [&] { return state; } } };

    auto thrown = false;
    h.loop.spawn([](vthost::DaemonRuntime* daemon, bool* out) -> core::async::Task<void> {
        try
        {
            auto const result = co_await daemon->requestUpgrade({ .imageId = "a" }, [](std::string_view) {});
            *out = !result.has_value();
        }
        catch (std::runtime_error const&)
        {
            *out = true;
        }
    }(&runtime, &thrown));
    std::ignore = h.loop.runUntilIdle();
    CHECK(thrown);

    auto const status = runtime.status();
    REQUIRE(status.has_value());
    REQUIRE(status->upgrade.has_value());
    CHECK(status->upgrade->attempts == 1);
    CHECK(status->upgrade->lastFailure == vthost::proto::UpgradeDaemonReply::ExecFailedResult);
    // The requester is gone with the frame: another announcement reaches no dead lambda.
    hooks.announceUpgrade("1.2.4");
}

TEST_CASE("DaemonRuntime states whether its daemon may move onto its staged copy", "[vthost][runtime]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    auto const statusOf = [&](vthost::RelocationRequest request) {
        auto runtime = vthost::DaemonRuntime { { .releaseVersion = "0.9.0",
                                                 .processId = 1,
                                                 .socketPath = "/run/c/default",
                                                 .executablePath = "/s/bin/contour",
                                                 .logFile = {},
                                                 .lifecycle = vthost::DaemonLifecycle::Persistent,
                                                 .upgradeGeneration = 0,
                                                 .relocationRequest = request },
                                               h.host,
                                               clock,
                                               [](vthost::ShutdownCause) {} };
        return runtime.status();
    };
    // The encoder drops the relocation byte of a status that has no upgrade record: every daemon would then
    // read as one that cannot be moved. So the record is part of what this pins.
    CHECK(statusOf(vthost::RelocationRequest::Allowed)->upgrade.has_value());
    CHECK(statusOf(vthost::RelocationRequest::Refused)->relocation
          == std::to_underlying(vthost::RelocationRequest::Refused));
    CHECK(statusOf(vthost::RelocationRequest::Allowed)->relocation
          == std::to_underlying(vthost::RelocationRequest::Allowed));
}

// Review (PR 3, item 5): the switcher's "N workspaces" came from the session count.
TEST_CASE("DaemonRuntime counts the workspaces it holds, not its sessions", "[vthost][runtime]")
{
    auto clock = core::platform::ManualClock {};
    auto h = RuntimeHarness {};
    // Three sessions in one workspace, and a second workspace of one.
    REQUIRE(h.host.createTab() != nullptr);
    REQUIRE(h.host.createTab() != nullptr);
    REQUIRE(h.host.createWorkspace() != nullptr);
    auto runtime = vthost::DaemonRuntime { { .releaseVersion = "0.9.0",
                                             .processId = 1,
                                             .socketPath = "/run/c/default",
                                             .executablePath = "/s/bin/contour",
                                             .logFile = {},
                                             .lifecycle = vthost::DaemonLifecycle::Persistent,
                                             .upgradeGeneration = 0,
                                             .relocationRequest = vthost::RelocationRequest::Allowed },
                                           h.host,
                                           clock,
                                           [](vthost::ShutdownCause) {} };
    auto const status = runtime.status();
    REQUIRE(status.has_value());
    CHECK(status->workspaces == std::optional { uint32_t { 2 } });
    CHECK(status->sessions == 4);
}
