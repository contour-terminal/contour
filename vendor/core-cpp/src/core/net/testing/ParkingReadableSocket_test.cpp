// SPDX-License-Identifier: Apache-2.0
//
// The shared socket doubles' own contract: a double is held to the rule the sockets it stands for
// are held to, that retiring a parked operation completes it LAST. The first case is the shape that
// makes the rule bite -- the coroutine the completion resumes OWNS the double and drops it before
// control returns -- so a double that touched a member after completing would write through a
// freed object, which only a sanitizer build reports. The rest pin what the doubles count, and that
// `SocketDecorator` forwards what a hand-written decorator forgets.
//
// Origin: fastcached `src/FastCache/Net/SocketDecorator_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`), the fake-contract case; the counter and forwarding
// cases are new.
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/ParkingReadableSocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using core::async::DetachedTask;
using core::async::Task;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::testing::InMemorySocketPair;
using core::net::testing::ParkingReadableSocket;
using core::net::testing::ParkingWritableSocket;
using core::net::testing::SocketDecorator;

namespace
{

/// A double over @p inner, held by exactly one `shared_ptr` in an allocation of its own.
///
/// Not `make_shared`: an object sharing its allocation with the control block stays mapped while
/// any `weak_ptr` to it lives, so ASan would see no freed object for a late member access -- and a
/// `weak_ptr` is how each section observes the destruction.
template <typename Double>
std::shared_ptr<Double> soleOwner(ISocket& inner)
{
    return std::make_unique<Double>(inner);
}

/// Awaits a readability watch on a socket this flow owns, and drops it the moment the watch
/// resumes -- what a connection does when its watch says the peer is gone.
DetachedTask watchThenDrop(std::shared_ptr<ParkingReadableSocket> owner)
{
    std::ignore = co_await owner->waitReadable();
    owner.reset();
}

/// Writes to a socket this flow owns, and drops it the moment the write resumes.
DetachedTask writeThenDrop(std::shared_ptr<ParkingWritableSocket> owner)
{
    auto const bytes = std::array { std::byte { 0x2A } };
    std::ignore = co_await owner->write(bytes);
    owner.reset();
}

/// Awaits one readability watch and hands back its answer; an owned task, so a watch it leaves
/// parked is freed with it rather than leaked.
Task<IoResult> watchOnce(ISocket* socket)
{
    co_return co_await socket->waitReadable();
}

/// Records the last receive deadline it was given; forwards everything else.
class DeadlineRecorder final: public SocketDecorator
{
  public:
    using SocketDecorator::SocketDecorator;

    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override { last = deadline; }

    std::optional<std::chrono::milliseconds> last;
};

} // namespace

TEST_CASE("A parking double lets the coroutine it resumes destroy it", "[net][socket][fake]")
{
    auto const pair = InMemorySocketPair::create();

    // Each route resumes the awaiting coroutine inline, which destroys the double before the call
    // returns. The coroutine holds the only owning reference, so the section watches it through a
    // `weak_ptr`, and nothing after the call may touch the raw pointer.
    SECTION("a readable watch retired by close")
    {
        auto owner = soleOwner<ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        auto const watched = std::weak_ptr<ParkingReadableSocket> { owner };
        watchThenDrop(std::move(owner));
        REQUIRE(socket->isWatchParked());
        REQUIRE_FALSE(watched.expired());

        socket->close();
        CHECK(watched.expired());
    }

    SECTION("a readable watch retired by cancelRead")
    {
        auto owner = soleOwner<ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        auto const watched = std::weak_ptr<ParkingReadableSocket> { owner };
        watchThenDrop(std::move(owner));
        REQUIRE(socket->isWatchParked());
        REQUIRE_FALSE(watched.expired());

        socket->cancelRead();
        CHECK(watched.expired());
    }

    SECTION("a parked write retired by close")
    {
        auto owner = soleOwner<ParkingWritableSocket>(*pair.server);
        auto* const socket = owner.get();
        auto const watched = std::weak_ptr<ParkingWritableSocket> { owner };
        socket->stopReading();
        writeThenDrop(std::move(owner));
        REQUIRE(socket->isWriteParked());
        REQUIRE_FALSE(watched.expired());

        socket->close();
        CHECK(watched.expired());
    }
}

TEST_CASE("ParkingReadableSocket counts each way a watch ends, apart", "[net][socket][fake]")
{
    auto const pair = InMemorySocketPair::create();
    auto socket = ParkingReadableSocket { *pair.server };

    // Resolved as readability: the count is the answer, and it is what the watch returns.
    auto resolved = watchOnce(&socket);
    resolved.handle().resume();
    REQUIRE(socket.isWatchParked());
    socket.resolveReadable(4);
    REQUIRE(resolved.done());
    REQUIRE(resolved.result().has_value());

    // Retired by the caller, then by the teardown: opposite answers, two counters.
    auto cancelled = watchOnce(&socket);
    cancelled.handle().resume();
    socket.cancelRead();
    REQUIRE(cancelled.done());
    auto const cancelledResult = cancelled.result();
    REQUIRE_FALSE(cancelledResult.has_value());
    CHECK(cancelledResult.error().code == NetErrorCode::Cancelled);

    // Orphaned: a second watch armed over a parked one drops the first, which is the defect the
    // real sockets' slot tripwire exists for, observed here instead of aborting.
    auto first = watchOnce(&socket);
    first.handle().resume();
    auto second = watchOnce(&socket);
    second.handle().resume();
    CHECK_FALSE(first.done());

    socket.close();
    REQUIRE(second.done());
    CHECK_FALSE(first.done()); // orphaned: nothing will ever resume it

    CHECK(socket.watchesArmed() == 4);
    CHECK(socket.watchesResolved() == 1);
    CHECK(socket.watchesRetiredByCancel() == 1);
    CHECK(socket.watchesRetiredByClose() == 1);
    CHECK(socket.watchesOrphaned() == 1);
}

TEST_CASE("SocketDecorator forwards a zero receive deadline unchanged", "[net][socket][fake]")
{
    // Zero REMOVES the bound (`ISocket::setReceiveDeadline`); it once meant "leave it alone", and a
    // decorator that still read it that way would swallow the one call that lifts a deadline.
    auto const pair = InMemorySocketPair::create();
    auto recorder = DeadlineRecorder { *pair.server };
    // Through the ISocket reference, or brace-initialising from a decorator picks the (deleted)
    // copy constructor rather than the forwarding one.
    auto decorator = SocketDecorator { static_cast<ISocket&>(recorder) };

    decorator.setReceiveDeadline(250ms);
    CHECK(recorder.last == 250ms);
    decorator.setReceiveDeadline(0ms);
    CHECK(recorder.last == 0ms);
    decorator.setReceiveDeadline(-1ms);
    CHECK(recorder.last == -1ms);
}

TEST_CASE("SocketDecorator forwards the verbs whose defaults would otherwise answer", "[net][socket][fake]")
{
    // Every one of these has a default in `ISocket` that succeeds, so a decorator that forgot to
    // forward one would pass a case that only checked for success. Each is asserted by an effect on
    // the DECORATED socket.
    auto const pair = InMemorySocketPair::create(0, "192.0.2.7");
    auto decorator = SocketDecorator { *pair.server };

    CHECK(decorator.peerAddress() == "192.0.2.7");

    // A half-close through the decorator reaches the peer as EOF.
    auto shut = decorator.shutdownWrite();
    CHECK(shut.await_ready());
    auto eof = watchOnce(pair.client.get());
    eof.handle().resume();
    REQUIRE(eof.done());
    auto const atPeer = eof.result();
    REQUIRE(atPeer.has_value());
    CHECK(*atPeer == 0);

    // And a close through it closes the decorated socket.
    decorator.close();
    CHECK(pair.server->isClosed());
}
