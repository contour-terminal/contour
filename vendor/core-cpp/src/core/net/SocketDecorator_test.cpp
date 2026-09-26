// SPDX-License-Identifier: Apache-2.0
//
// A retired operation's coroutine may destroy the socket it belonged to, and nothing the retiring
// verb does may be in its way.
//
// Since 0.2.1 the verb SETTLES the operation and the LOOP resumes the coroutine, in a later drain
// step (G2): so in these cases the socket is still alive when `close()` or `cancelRead()` returns,
// and the resumed coroutine destroys it a turn later. (That is these cases' order, not a promise to
// an owner: an owner may destroy the socket before the turn, and the resumed flow must then not
// touch it -- `CloseResumesThroughLoop_test.cpp` and `ISocket::close` hold that side.) Before 0.2.1
// the coroutine was resumed inside the verb, destroyed the socket there, and the rule "detach
// first, complete last, touch no member afterwards" was all that stood between a transport and a
// write through freed storage. The rule still holds -- a destructor abandons, and a loop-less
// double still resumes inline -- but it is no longer what these cases measure.
//
// The coroutine the completion resumes is the socket's only owner, and it drops it from inside the
// resumption. Each case watches the destruction through a `weak_ptr`, so the assertion is a
// deterministic value on every platform rather than an ASan report once in N runs.
//
// It is named for decorators because they are where the rule is easiest to lose: a decorator
// forwards the verb and may hold state of its own around it. `SplitSocket` is exercised here for
// exactly that reason.
#include <core/async/DetachedTask.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/SplitSocket.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using core::async::DetachedTask;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;

namespace
{

/// Awaits a readability watch on a socket this flow OWNS, and drops it the moment the watch
/// resumes — what a connection handler does when its watch says the peer is gone.
/// @param owner The socket's only owning reference, moved in; reset from inside the resumption.
DetachedTask watchThenDrop(std::shared_ptr<ISocket> owner)
{
    std::ignore = co_await owner->waitReadable();
    owner.reset();
}

/// Writes to a socket this flow OWNS, and drops it the moment the write resumes.
/// @param owner The socket's only owning reference, moved in.
/// @param payload The bytes; must outlive the flow.
DetachedTask writeThenDrop(std::shared_ptr<ISocket> owner, std::vector<std::byte> const* payload)
{
    std::ignore = co_await owner->write(std::span<std::byte const> { *payload });
    owner.reset();
}

/// Big enough that no platform's send buffer takes it, so the write parks.
constexpr std::size_t UnsendablePayload = std::size_t { 8 } * 1024 * 1024;

} // namespace

TEST_CASE("A retired operation may destroy the socket it belonged to", "[net][socket][decorator]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            SECTION("a readability watch retired by close()")
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());
                auto keepPeerAlive = std::move(pair->second);

                auto owner = std::shared_ptr<ISocket> { std::move(pair->first) };
                auto* const socket = owner.get();
                auto const watched = std::weak_ptr<ISocket> { owner };
                watchThenDrop(std::move(owner));
                // No turn is needed and none may be taken: a `DetachedTask` starts eagerly,
                // so the watch has already reached its park -- and driving a turn here would
                // BLOCK in the backend's wait with nothing left to wake it, turning this
                // case's red into a hang.

                REQUIRE(loop.parkedWaiterCount() > 0);
                REQUIRE_FALSE(watched.expired());

                socket->close();
                CHECK_FALSE(watched.expired()); // settled, not resumed: the socket outlives close()
                std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
                CHECK(watched.expired()); // the loop resumed it, and the resumption destroyed it
            }

            SECTION("a readability watch retired by cancelRead()")
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());
                auto keepPeerAlive = std::move(pair->second);

                auto owner = std::shared_ptr<ISocket> { std::move(pair->first) };
                auto* const socket = owner.get();
                auto const watched = std::weak_ptr<ISocket> { owner };
                watchThenDrop(std::move(owner));

                REQUIRE(loop.parkedWaiterCount() > 0);
                REQUIRE_FALSE(watched.expired());

                socket->cancelRead();
                CHECK_FALSE(watched.expired());
                std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
                CHECK(watched.expired());
            }

            SECTION("a parked write retired by close()")
            {
                auto loop = EventLoop { *source };
                auto pair = core::net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());
                auto keepPeerAlive = std::move(pair->second);

                auto const payload = std::vector<std::byte>(UnsendablePayload, std::byte { 0xA5 });
                auto owner = std::shared_ptr<ISocket> { std::move(pair->first) };
                auto* const socket = owner.get();
                auto const watched = std::weak_ptr<ISocket> { owner };
                writeThenDrop(std::move(owner), &payload);

                REQUIRE(loop.parkedWaiterCount() > 0);
                REQUIRE_FALSE(watched.expired());

                socket->close();
                CHECK_FALSE(watched.expired());
                std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
                CHECK(watched.expired());
            }
        }
    }
}

TEST_CASE("A decorator forwards the rule along with the verb", "[net][socket][decorator]")
{
    // A `SplitSocket` adds no operation of its own: it hands the inner half's awaitable straight
    // out by value, so the slot being claimed is the half's and the retirement is the half's. What
    // could go wrong is a decorator that wrapped the operation in something of its own and then
    // touched that after completing — which is the same rule one layer up.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto readPair = core::net::testing::makeSocketPair(loop);
            auto writePair = core::net::testing::makeSocketPair(loop);
            REQUIRE(readPair.has_value());
            REQUIRE(writePair.has_value());
            auto keepReadPeer = std::move(readPair->second);
            auto keepWritePeer = std::move(writePair->second);

            auto owner = std::shared_ptr<ISocket> { core::net::combineHalves(std::move(readPair->first),
                                                                             std::move(writePair->first)) };
            auto* const socket = owner.get();
            auto const watched = std::weak_ptr<ISocket> { owner };
            watchThenDrop(std::move(owner));

            REQUIRE(loop.parkedWaiterCount() > 0);
            REQUIRE_FALSE(watched.expired());

            socket->cancelRead(); // forwarded to the read half, which retires; the loop resumes
            CHECK_FALSE(watched.expired());
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
            CHECK(watched.expired());
        }
    }
}

TEST_CASE("A decorator's close() touches nothing once a completion has run", "[net][socket][decorator]")
{
    // `SplitSocket::close()` retires TWO operations, one per half, and each retirement resumes a
    // coroutine that may own the decorator. So "complete last" cannot be satisfied by ordering the
    // two calls: whichever half closes first may destroy the decorator -- and both halves with it
    // -- before the second call is made. The second call then reads a member of a freed object.
    //
    // The watch parks on the READ half, which `close()` retires first, and its resumption drops the
    // only owner. What the case asserts is that `close()` returns at all: without a liveness check
    // between the two retirements, it goes on to call `close()` through a destroyed `_writeHalf`.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto readPair = core::net::testing::makeSocketPair(loop);
            auto writePair = core::net::testing::makeSocketPair(loop);
            REQUIRE(readPair.has_value());
            REQUIRE(writePair.has_value());
            auto keepReadPeer = std::move(readPair->second);
            auto keepWritePeer = std::move(writePair->second);

            auto owner = std::shared_ptr<ISocket> { core::net::combineHalves(std::move(readPair->first),
                                                                             std::move(writePair->first)) };
            auto* const socket = owner.get();
            auto const watched = std::weak_ptr<ISocket> { owner };
            watchThenDrop(std::move(owner));

            REQUIRE(loop.parkedWaiterCount() > 0);
            REQUIRE_FALSE(watched.expired());

            socket->close(); // retires both halves; the read half's watch will destroy the decorator
            CHECK_FALSE(watched.expired());
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
            CHECK(watched.expired());
        }
    }
}
