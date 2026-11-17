// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/ServedSessions.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/ServedTesting.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vthost::testing::openServedTab;

namespace
{

constexpr auto Size = vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };

/// A host over MockPty sessions with no pump threads, that ends with its last session, as an auto-spawned
/// daemon does; a counter stands in for the shutdown.
struct WatchedHost
{
    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       core::defaultEnvironment(),
                       SessionHostConfig { .pumping = SessionPumping::Manual } };
    int shutdowns = 0;
    LastSessionWatcher watcher { host, loop, [this] { ++shutdowns; } };

    /// Turns the loop once.
    void turn() { loop.blockOn(core::net::testing::sleepFor(&loop, 0ms)); }
};

} // namespace

TEST_CASE("a served device takes output and buffers input", "[vthost][served]")
{
    auto served = vthost::ServedSessions { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    auto const device = served.makeDevice(Size);
    REQUIRE(device != nullptr);

    // What the extension draws reaches the terminal reading the device.
    device->feed("\x1b[Hdrawn");
    auto pool = crispy::BufferObjectPool<char> { 4096 };
    auto storage = pool.allocateBufferObject();
    auto const read = device->read(*storage, 1s, 4096);
    REQUIRE(read.has_value());
    CHECK(read->data == "\x1b[Hdrawn");

    // What the terminal writes is the extension's to read.
    CHECK(device->writeInput("q", vtpty::InputKind::Keyboard) == 1);
    CHECK(served.inputOf(*device) == "q");

    // A device it did not make has nothing buffered here.
    auto const stranger = vtpty::ChannelPty { Size };
    CHECK(served.inputOf(stranger).empty());
}

TEST_CASE("a chunk that does not fit whole is dropped whole", "[vthost][served]")
{
    auto served = vthost::ServedSessions { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    auto const device = served.makeDevice(Size);
    auto const filler = std::string(vthost::MaxServedInputBytes - 2, 'a');
    REQUIRE(device->writeInput(filler, vtpty::InputKind::Keyboard) == static_cast<int>(filler.size()));
    REQUIRE(served.inputOf(*device) == filler);

    // Three bytes into two of room: not cut, so the reader never sees half a key.
    std::ignore = device->writeInput("\x1b[A", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == filler);

    // What fits is kept again.
    std::ignore = device->writeInput("x", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == filler + "x");
}

TEST_CASE("a chunk bigger than the bound is dropped, however empty the buffer", "[vthost][served]")
{
    auto served = vthost::ServedSessions { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    auto const device = served.makeDevice(Size);
    std::ignore =
        device->writeInput(std::string(vthost::MaxServedInputBytes + 1, 'p'), vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device).empty());
    std::ignore = device->writeInput("q", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == "q");
}

TEST_CASE("(c) served tab as the last session", "[daemon][served][lifecycle]")
{
    SECTION("an upgrade's close of it keeps the daemon")
    {
        auto h = WatchedHost {};
        REQUIRE(openServedTab(h.host).has_value());
        h.host.closeServedSessions();
        h.turn();
        h.turn();
        CHECK(h.host.sessionCount() == 0);
        CHECK(h.shutdowns == 0);
    }
    SECTION("the user's close of it ends the daemon")
    {
        auto h = WatchedHost {};
        auto const served = openServedTab(h.host);
        REQUIRE(served.has_value());
        h.host.closeSession(*served);
        h.turn();
        h.turn();
        CHECK(h.shutdowns == 1);
    }
}

TEST_CASE("the upgrade latch refuses served spawns until the rollback", "[daemon][served][upgrade]")
{
    auto h = WatchedHost {};
    h.host.closeServedSessions();
    CHECK(h.host.servedAdmission() == ServedAdmission::Closed);
    CHECK_FALSE(openServedTab(h.host).has_value());
    CHECK(h.host.sessionCount() == 0);

    h.host.admitServedSessions();
    CHECK(h.host.servedAdmission() == ServedAdmission::Open);
    auto const served = openServedTab(h.host);
    REQUIRE(served.has_value());
    CHECK(h.host.servedBy(*served) != nullptr);
    CHECK(*h.host.servedBy(*served)
          == ServedBy { .extension = "stub", .scheme = "stub", .moduleDigest = "" });
}
