// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/DaemonStarter.hpp>
#include <vthost/testing/FakeServiceBackend.hpp>

namespace fs = std::filesystem;

using vthost::DaemonLaunchStyle;
using vthost::DaemonPresence;
using vthost::ServiceRunState;
using vthost::testing::FakeServiceBackend;
using vthost::testing::FakeServiceLog;

namespace
{
/// A DaemonSpawner that starts nothing: it counts spawns and answers each probe as told.
class ScriptedSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const>,
                                                         vtpty::ProcessPlacement&) override
    {
        ++spawns;
        return {};
    }

    [[nodiscard]] DaemonPresence probe(fs::path const&) override { return presence; }

    DaemonPresence presence = DaemonPresence::Absent;
    int spawns = 0;
};

/// A DaemonBinarySource that counts how often it is asked.
class CountingBinary final: public vthost::DaemonBinarySource
{
  public:
    [[nodiscard]] vthost::StagedImage imageForSpawn() override
    {
        ++calls;
        return vthost::StagedImage { .executable = "contour-no-such-binary", .lease = nullptr };
    }

    int calls = 0;
};

constexpr auto SocketPath = "/nonexistent/contour/control";

struct OnDemandFixture
{
    ScriptedSpawner spawner {};
    CountingBinary binary {};
    vtpty::NoPlacement placement {};

    [[nodiscard]] vthost::OnDemandSpawn spawn(std::chrono::seconds timeout)
    {
        return vthost::OnDemandSpawn { .endpoint = vthost::UnixEndpoint { .socketPath = SocketPath },
                                       .spawner = spawner,
                                       .binary = binary,
                                       .placement = placement,
                                       .options = {},
                                       .timeout = timeout };
    }
};
} // namespace

TEST_CASE("a live probe decides whether a registered daemon runs", "[vthost][starter]")
{
    using vthost::effectiveRunState;
    CHECK(effectiveRunState(ServiceRunState::Stopped, DaemonPresence::Serving) == ServiceRunState::Running);
    CHECK(effectiveRunState(ServiceRunState::Running, DaemonPresence::Absent) == ServiceRunState::Stopped);
    CHECK(effectiveRunState(ServiceRunState::NotInstalled, DaemonPresence::Serving)
          == ServiceRunState::NotInstalled);
    CHECK(effectiveRunState(ServiceRunState::NotInstalled, DaemonPresence::Absent)
          == ServiceRunState::NotInstalled);
}

TEST_CASE("only a self-detaching service leaves its daemon to the daemon's own stop", "[vthost][starter]")
{
    CHECK(vthost::stopVia(DaemonLaunchStyle::Foreground) == vthost::StopRoute::ServiceManager);
    CHECK(vthost::stopVia(DaemonLaunchStyle::SelfDetaching) == vthost::StopRoute::DaemonControl);
}

TEST_CASE("a registered service is started instead of spawning on demand", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    // The daemon appears once the service has been started.
    fixture.spawner.presence = DaemonPresence::Serving;

    auto starter = vthost::chooseDaemonStarter(
        vthost::RegisteredService { .backend =
                                        std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log),
                                    .name = "contour-daemon" },
        SocketPath,
        fixture.spawn(std::chrono::seconds { 1 }));
    REQUIRE(starter);
    CHECK(starter->start().has_value());
    CHECK(log.starts == 1);
    CHECK(fixture.binary.calls == 0);
    CHECK(fixture.spawner.spawns == 0);
}

TEST_CASE("a started service that never serves is an error naming the socket", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    auto starter = vthost::ServiceDaemonStarter(
        vthost::RegisteredService { .backend =
                                        std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log),
                                    .name = "contour-daemon" },
        fixture.spawner,
        SocketPath,
        std::chrono::milliseconds { 0 });
    auto const result = starter.start();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains(SocketPath));
    CHECK(log.starts == 1);
}

TEST_CASE("a registered service is not started for a socket it does not serve", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    // $CONTOUR_MUX or --socket points elsewhere than the label's default the service serves.
    auto starter = vthost::chooseDaemonStarter(
        vthost::RegisteredService { .backend =
                                        std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log),
                                    .name = "contour-daemon" },
        "/nonexistent/contour/default",
        fixture.spawn(std::chrono::seconds { 0 }));
    REQUIRE(starter);
    CHECK_FALSE(starter->start().has_value());
    CHECK(log.starts == 0);
    CHECK(fixture.spawner.spawns == 1);
    CHECK(fixture.binary.calls == 1);
}

TEST_CASE("a registration that pins its socket is compared by that socket", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    fixture.spawner.presence = DaemonPresence::Serving;
    auto const pinned = std::string { "/usr/bin/contour daemon --socket=" } + SocketPath;
    auto starter = vthost::chooseDaemonStarter(
        vthost::RegisteredService { .backend = std::make_unique<FakeServiceBackend>(
                                        ServiceRunState::Stopped, log, DaemonLaunchStyle::Foreground, pinned),
                                    .name = "contour-daemon" },
        "/nonexistent/contour/default",
        fixture.spawn(std::chrono::seconds { 1 }));
    REQUIRE(starter);
    CHECK(starter->start().has_value());
    CHECK(log.starts == 1);
    CHECK(fixture.spawner.spawns == 0);
}

TEST_CASE("socket paths compare regardless of separators and dot segments", "[vthost][starter]")
{
    CHECK(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/b"));
    CHECK(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/b/"));
    CHECK(vthost::isSameSocketPath("/nonexistent/a/./c/../b", "/nonexistent/a/b"));
    CHECK_FALSE(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/c"));
}

TEST_CASE("without a registration the daemon is spawned on demand", "[vthost][starter]")
{
    auto fixture = OnDemandFixture {};
    auto starter =
        vthost::chooseDaemonStarter(std::nullopt, SocketPath, fixture.spawn(std::chrono::seconds { 0 }));
    REQUIRE(starter);
    auto const result = starter->start();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains(SocketPath));
    CHECK(fixture.spawner.spawns == 1);
    CHECK(fixture.binary.calls == 1);
}

TEST_CASE("findRegisteredService finds a legacy-named registration", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto asked = std::vector<std::string> {};
    auto const factory = vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode, std::string_view name)
                                                             -> std::unique_ptr<vthost::ServiceBackend> {
        asked.emplace_back(name);
        return std::make_unique<FakeServiceBackend>(
            name == "contour-daemon-default" ? ServiceRunState::Stopped : ServiceRunState::NotInstalled, log);
    } };

    auto const found = vthost::findRegisteredService("default", factory);
    REQUIRE(found.has_value());
    CHECK(found->name == "contour-daemon-default");
    CHECK(found->backend != nullptr);
    // The current name is asked (under every mode) before the legacy one.
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.front() == "contour-daemon");
    CHECK(asked.back() == "contour-daemon-default");
}

TEST_CASE("findRegisteredService reports nothing when no name is registered", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return std::make_unique<FakeServiceBackend>(ServiceRunState::NotInstalled, log);
        } };
    CHECK_FALSE(vthost::findRegisteredService("default", factory).has_value());
}
