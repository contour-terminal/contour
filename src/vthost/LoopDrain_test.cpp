// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <tuple>

#include <vthost/LoopDrain.hpp>

namespace
{

/// Sets the flag it is given when destroyed -- which, in a coroutine frame, is when the flow has
/// unwound, whether or not the loop still holds the finished frame.
struct UnwindMark
{
    explicit UnwindMark(bool* flag) noexcept: unwound(flag) {}
    UnwindMark(UnwindMark const&) = delete;
    UnwindMark& operator=(UnwindMark const&) = delete;
    UnwindMark(UnwindMark&&) = delete;
    UnwindMark& operator=(UnwindMark&&) = delete;
    ~UnwindMark() { *unwound = true; }

    bool* unwound;
};

core::async::Task<void> readOnce(core::net::ISocket* socket, std::span<std::byte> buffer, bool* unwound)
{
    auto const mark = UnwindMark { unwound };
    std::ignore = co_await socket->read(buffer);
}

core::async::Task<void> readThroughSubTask(core::net::ISocket* socket,
                                           std::span<std::byte> buffer,
                                           bool* unwound)
{
    co_await readOnce(socket, buffer, unwound);
}

} // namespace

TEST_CASE("LoopDrain ends a spawned flow still parked on a read", "[vthost]")
{
    // The shape of a daemon's teardown: a connection flow parked on a read nobody will answer.
    // On an I/O completion port a cancelled read stays parked until its completion packet comes
    // back, so one idle turn is not enough to end it.
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());
    auto buffer = std::array<std::byte, 16> {};
    auto unwound = false;
    loop.spawn(readOnce(pair->first.get(), buffer, &unwound));
    std::ignore = loop.runUntilIdle();
    REQUIRE(loop.parkedWaiterCount() == 1);
    REQUIRE_FALSE(unwound);

    {
        auto const drain = vthost::LoopDrain { loop };
    }

    CHECK(unwound);
    CHECK(loop.parkedWaiterCount() == 0);
    CHECK(loop.spawnedCount() == 0);
}

TEST_CASE("LoopDrain ends a spawned flow parked inside a sub-task", "[vthost]")
{
    // The drain counts spawned flows, so a flow that ends inside its sub-task's resume must stop
    // counting there; before core-cpp 0.3.0 it stayed counted until ~EventLoop.
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());
    auto buffer = std::array<std::byte, 16> {};
    auto unwound = false;
    loop.spawn(readThroughSubTask(pair->first.get(), buffer, &unwound));
    std::ignore = loop.runUntilIdle();
    REQUIRE(loop.spawnedCount() == 1);
    REQUIRE_FALSE(unwound);

    {
        auto const drain = vthost::LoopDrain { loop };
    }

    CHECK(unwound);
    CHECK(loop.spawnedCount() == 0);
}
