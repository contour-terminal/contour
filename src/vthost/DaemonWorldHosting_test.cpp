// SPDX-License-Identifier: Apache-2.0
//
// How a SessionHost makes and stops its daemon-scoped extensions: through a fake, in a build with
// extensions or without.
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>

using vthost::testing::FakeDaemonWorldHosting;

namespace
{

/// @return A host on @p loop, its sessions on mock PTYs, with @p daemonWorld.
[[nodiscard]] std::unique_ptr<vthost::SessionHost> hostOn(core::net::EventLoop& loop,
                                                          vthost::DaemonWorldFactory daemonWorld)
{
    return std::make_unique<vthost::SessionHost>(
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
            return std::make_unique<vtpty::MockPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .daemonWorld = std::move(daemonWorld) });
}

} // namespace

TEST_CASE("SessionHost: the daemon world is made once, and stopped before the loop drains",
          "[daemon][extensions]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const journal = std::make_shared<std::vector<std::string>>();
    auto made = std::shared_ptr<FakeDaemonWorldHosting> {};
    auto calls = 0;
    auto host = hostOn(loop, [&](vthost::DaemonWorldEvents const& /*events*/, vthost::OwnerPost post) {
        ++calls;
        made = std::make_shared<FakeDaemonWorldHosting>(journal, std::move(post));
        return made;
    });
    CHECK(calls == 1);
    REQUIRE(made != nullptr);

    // What it posts runs on the loop.
    made->post("first");
    CHECK(journal->empty());
    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(*journal == std::vector<std::string> { "first" });
    CHECK(calls == 1);

    // A job still on its way when the host goes is drained after the extensions stopped.
    made->post("late");
    host.reset();
    CHECK(*journal == std::vector<std::string> { "first", "stop", "late" });
    CHECK(calls == 1);
}

TEST_CASE("SessionHost: an empty daemon-world factory constructs nothing", "[daemon][extensions]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = hostOn(loop, {});
    // A host with daemon-scoped extensions must not go during a turn of its loop: it drains the loop as it
    // goes (a checked precondition). One without them has nothing to stop or drain, so it may.
    loop.post([&host] { host.reset(); });
    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(host == nullptr);
}
