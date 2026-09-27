// SPDX-License-Identifier: Apache-2.0
//
// A turn that wakes a flow parked on a closed handle is not an idle turn.
//
// `notifyHandleClosing` records the parks on a closing handle, and the NEXT turn queues their
// waiters after its wait. That turn used to report itself idle all the same -- nothing posted,
// nothing drained, nothing dispatched, nothing due -- so `runUntilIdle` and `TestLoop::drain`
// returned with the woken waiters still in the ready queue. A caller that then tore its objects
// down left `~EventLoop` to resume or free those frames after their owners were gone, which is the
// heap-use-after-free fastcached's server teardown reported under ASan and TSan.
//
// Each case parks an operation, closes what it is parked on, and asks ONE `runUntilIdle` to finish
// the job: the operation must have resolved by the time it returns.
//
// **Only a park that goes through the closed-park queue can fail here**, and on POSIX the socket
// case does not: `PosixSocket::close()` settles its parked read inline, before the drain even
// starts, so that case is a CONTROL there -- it would pass on the unfixed loop. (It
// distinguished on Windows while the WFMO backend's `WindowsSocket` parked through the loop's
// coroutine awaiter; that transport was removed in 0.5.0.) The listener case distinguishes
// everywhere, and on POSIX
// `posix/WaitReadableClose_test.cpp` parks a raw descriptor through `waitReadable`, which does.
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <tuple>

using core::async::Task;
using core::net::EventLoop;
using core::net::NetErrorCode;
using core::net::testing::BackendMatrix;

namespace
{

/// How a parked operation ended.
struct Outcome
{
    bool resolved = false;                ///< Whether it finished at all.
    bool hasValue = false;                ///< Whether it answered with a value.
    NetErrorCode code = NetErrorCode::Ok; ///< The code, where it answered with an error.
};

/// Accepts once and records how it ended.
Task<void> acceptOnce(core::net::IListener* listener, Outcome* out)
{
    auto accepted = co_await listener->accept();
    out->resolved = true;
    out->hasValue = accepted.has_value();
    if (!accepted.has_value())
        out->code = accepted.error().code;
}

/// Reads once and records how it ended.
Task<void> readOnce(core::net::ISocket* sock, Outcome* out)
{
    auto buffer = std::array<std::byte, 16> {};
    auto const got = co_await sock->read(buffer);
    out->resolved = true;
    out->hasValue = got.has_value();
    if (!got.has_value())
        out->code = got.error().code;
}

} // namespace

TEST_CASE("runUntilIdle does not return while a closed listener's accept is still queued",
          "[net][loop][idle]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto outcome = Outcome {};
            loop.spawn(acceptOnce(listener->get(), &outcome));
            std::ignore = loop.runUntilIdle();
            // Parked: no client ever connects, so only the close can end it.
            REQUIRE_FALSE(outcome.resolved);

            (*listener)->close();
            std::ignore = loop.runUntilIdle();

            CHECK(outcome.resolved);
            CHECK_FALSE(outcome.hasValue);
            CHECK(outcome.code == NetErrorCode::Cancelled);
        }
    }
}

TEST_CASE("runUntilIdle does not return while a closed socket's read is still queued", "[net][loop][idle]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto outcome = Outcome {};
            loop.spawn(readOnce(pair->second.get(), &outcome));
            std::ignore = loop.runUntilIdle();
            // Parked: the peer never writes, so only the close can end it.
            REQUIRE_FALSE(outcome.resolved);

            pair->second->close();
            std::ignore = loop.runUntilIdle();

            CHECK(outcome.resolved);
            CHECK_FALSE(outcome.hasValue);
            // And the code, which every transport now agrees on: the WFMO backend's
            // `WindowsSocket` answered `BadHandle` here (core-cpp#46) until 0.5.0 removed it.
            CHECK(outcome.code == NetErrorCode::Cancelled);
        }
    }
}
