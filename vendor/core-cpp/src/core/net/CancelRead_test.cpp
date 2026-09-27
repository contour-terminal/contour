// SPDX-License-Identifier: Apache-2.0
//
// `ISocket::cancelRead` — retiring a parked read-side operation WITHOUT closing the socket.
//
// Until it existed, a parked read could only be retrieved by `close()`, which made the
// one-read-operation rule unfollowable: a caller holding a parked `waitReadable` must resolve or
// abandon it before it reads, and *abandon* had no spelling short of tearing the connection down
// ([fastcached#710](https://github.com/LASTRADA-Software/fastcached/issues/710)).
//
// Two promises, and they are not the same promise. The SLOT is free when it returns — that is what
// lets a caller arm the next read in the same turn. The WAITER's operation is settled with
// `Cancelled` at once, because a readiness transport consumes nothing, so a retired read can lose
// nothing -- and its flow is resumed by the loop, never inside `cancelRead` (G2, since 0.2.1). The
// completion-based transport, `IocpSocket`, keeps both promises for what these cases park -- a `waitReadable`
// probe, a zero-byte receive with nothing in it to lose -- and so runs them too; for a REAL read
// it keeps only the first and settles the waiter with whatever its receive did
// (fastcached#884), which `windows/IocpSocket_test.cpp` holds.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

using core::async::OperationCancelled;
using core::async::StopSource;
using core::async::StopToken;
using core::async::Task;
using core::net::EventLoop;
using core::net::IoResult;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::testing::BackendMatrix;

namespace
{

/// How a watch ended.
struct WatchOutcome
{
    bool resolved = false;                ///< Whether it finished at all.
    bool threw = false;                   ///< Whether it unwound through OperationCancelled.
    bool hasValue = false;                ///< Whether it answered with a count.
    NetErrorCode code = NetErrorCode::Ok; ///< The code, where it answered with an error.
    std::size_t count = 0;                ///< The count, where it answered with one.
};

/// Parks a readability watch and records how it ended.
Task<void> watchOnce(ISocket* sock, WatchOutcome* out)
{
    try
    {
        auto const watched = co_await sock->waitReadable();
        out->resolved = true;
        out->hasValue = watched.has_value();
        if (watched.has_value())
            out->count = *watched;
        else
            out->code = watched.error().code;
    }
    catch (OperationCancelled const&)
    {
        out->resolved = true;
        out->threw = true;
    }
}

/// Parks a watch, and when it is retired arms a SECOND one from inside the resumption.
///
/// The shape fastcached#1233 was about: while a retirement resumed its victim inline, the second
/// call retired the watch this resumption armed. Now the resumption runs on the loop, after both
/// calls have returned.
Task<void> watchThenRearm(ISocket* sock, WatchOutcome* first, WatchOutcome* second)
{
    co_await watchOnce(sock, first);
    co_await watchOnce(sock, second);
}

/// Retires the parked read once the watcher has parked, then records that the socket still works.
/// @param loop The loop both ends belong to.
/// @param sock The socket whose read to retire.
/// @param peer The other end, used to prove the socket is still usable afterwards.
/// @param parkedFirst Where to record that something really was parked.
/// @param reusable Where to record that a read after the retirement still works.
Task<void> retireThenReuse(EventLoop* loop, ISocket* sock, ISocket* peer, bool* parkedFirst, bool* reusable)
{
    *parkedFirst = loop->parkedWaiterCount() > 0;
    sock->cancelRead();

    // Not a close: the socket stays open, and a later read works. Asserted by moving a byte through
    // it, because "isClosed() is still false" would also hold for a socket whose descriptor had
    // been quietly broken.
    auto const payload = std::array<std::byte, 1> { std::byte { 0x5A } };
    std::ignore = co_await peer->write(std::span<std::byte const> { payload });
    auto buffer = std::array<std::byte, 4> {};
    auto const got = co_await sock->read(buffer);
    *reusable = got.has_value() && *got == 1;
}

/// Runs a watcher and a retirement concurrently on one loop.
Task<void> watchAndRetire(
    EventLoop* loop, ISocket* sock, ISocket* peer, WatchOutcome* out, bool* parkedFirst, bool* reusable)
{
    co_await core::async::whenAll(watchOnce(sock, out),
                                  retireThenReuse(loop, sock, peer, parkedFirst, reusable));
}

/// Retires twice in a row, lets the loop run the retired flow, looks, and then retires the watch
/// that flow armed, so the case can finish.
Task<void> retireTwiceThenLook(
    EventLoop* loop, ISocket* sock, WatchOutcome const* second, bool* parkedFirst, bool* secondParkedAfter)
{
    *parkedFirst = loop->parkedWaiterCount() > 0;
    sock->cancelRead();
    sock->cancelRead(); // the retired flow has not run yet, so the slot is empty: a no-op
    co_await loop->delay(std::chrono::milliseconds { 1 }); // the loop resumes it; it re-arms
    *secondParkedAfter = !second->resolved && loop->parkedWaiterCount() > 0;
    sock->cancelRead(); // the one call meant for the second watch
}

/// Runs the re-arming watcher against the retirements.
Task<void> watchTwiceAndRetire(EventLoop* loop,
                               ISocket* sock,
                               WatchOutcome* first,
                               WatchOutcome* second,
                               bool* parkedFirst,
                               bool* secondParkedAfter)
{
    co_await core::async::whenAll(watchThenRearm(sock, first, second),
                                  retireTwiceThenLook(loop, sock, second, parkedFirst, secondParkedAfter));
}

/// Gives the awaiting coroutine a stop token of the caller's choosing, without suspending, so a
/// case can stop ONE flow's token while its siblings' stay live -- the state a `whenAny` or
/// `withTimeout` loser is in for the rest of the turn its sibling won.
struct AdoptStopToken
{
    StopToken token; ///< The token the awaiting coroutine, and everything it awaits, observes.

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// @return False: resume at once, now observing @c token.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> self) const noexcept
    {
        self.promise().setStopToken(token);
        return false;
    }

    void await_resume() const noexcept {}
};

/// Parks a read under @p token and records how it ended.
Task<void> readUnderToken(StopToken token, ISocket* sock, WatchOutcome* out)
{
    co_await AdoptStopToken { std::move(token) };
    auto buffer = std::array<std::byte, 4> {};
    try
    {
        auto const got = co_await sock->read(buffer);
        out->resolved = true;
        out->hasValue = got.has_value();
        if (got.has_value())
            out->count = *got;
        else
            out->code = got.error().code;
    }
    catch (OperationCancelled const&)
    {
        out->resolved = true;
        out->threw = true;
    }
}

/// One read, as a task, so `withTimeout` can bound it.
Task<IoResult> readInto(ISocket* sock, std::span<std::byte> buffer)
{
    co_return co_await sock->read(buffer);
}

/// Sends one byte once the reader has had a turn to park.
Task<void> sendAfterPark(EventLoop* loop, ISocket* peer)
{
    co_await loop->delay(std::chrono::milliseconds { 10 });
    auto const payload = std::array<std::byte, 1> { std::byte { 0x5A } };
    std::ignore = co_await peer->write(std::span<std::byte const> { payload });
}

/// Reads once, bounded, and records the outcome; a timeout records nothing but @p timedOut.
Task<void> boundedRead(EventLoop* loop, ISocket* sock, WatchOutcome* out, bool* timedOut)
{
    auto buffer = std::array<std::byte, 4> {};
    auto const got =
        co_await core::net::withTimeout(loop, readInto(sock, buffer), std::chrono::seconds { 5 });
    *timedOut = !got.has_value();
    if (!got.has_value())
        co_return;
    out->resolved = true;
    out->hasValue = got->has_value();
    if (got->has_value())
        out->count = **got;
    else
        out->code = got->error().code;
}

/// Stops the parked reader's token, retires it with `cancelRead` in the SAME turn -- before the
/// loop delivers the stop -- and then reads again, with the byte arriving only after that read
/// has parked, so the read is completed by a real wake-up rather than by data already waiting.
Task<void> stopRetireThenReadAgain(EventLoop* loop,
                                   ISocket* sock,
                                   ISocket* peer,
                                   StopSource* source,
                                   bool* parkedFirst,
                                   WatchOutcome* next,
                                   bool* timedOut)
{
    *parkedFirst = loop->parkedWaiterCount() > 0;
    source->request_stop();
    sock->cancelRead();
    co_await core::async::whenAll(boundedRead(loop, sock, next, timedOut), sendAfterPark(loop, peer));
}

/// Runs the reader whose token is stopped against the retirement and the read after it.
Task<void> stoppedReaderRetiredThenReadAgain(EventLoop* loop,
                                             ISocket* sock,
                                             ISocket* peer,
                                             StopSource* source,
                                             WatchOutcome* first,
                                             bool* parkedFirst,
                                             WatchOutcome* next,
                                             bool* timedOut)
{
    co_await core::async::whenAll(
        readUnderToken(source->get_token(), sock, first),
        stopRetireThenReadAgain(loop, sock, peer, source, parkedFirst, next, timedOut));
}

} // namespace

TEST_CASE("cancelRead retrieves a parked read and leaves the socket usable", "[net][socket][cancelread]")
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

            auto outcome = WatchOutcome {};
            auto parkedFirst = false;
            auto reusable = false;
            loop.blockOn(watchAndRetire(
                &loop, pair->first.get(), pair->second.get(), &outcome, &parkedFirst, &reusable));

            CHECK(parkedFirst); // or nothing was retired and the rest of this case is vacuous
            REQUIRE(outcome.resolved);

            // **A VALUE, not a throw.** The cancel came from the RESOURCE, not from the flow's own
            // token: the flow is alive and asked a question about a read that has been taken away
            // from it. A merge that collapsed the two would make a retired read indistinguishable
            // from a cancelled connection.
            CHECK_FALSE(outcome.threw);
            REQUIRE_FALSE(outcome.hasValue);
            CHECK(outcome.code == NetErrorCode::Cancelled);

            CHECK(reusable); // not a close: a later read still works
            CHECK_FALSE(pair->first->isClosed());
        }
    }
}

TEST_CASE("cancelRead with nothing parked disturbs nothing", "[net][socket][cancelread]")
{
    // The property a caller with a RAII watch relies on: its destructor may retire a watch that has
    // already resolved, and that must not reach into whatever came after it.
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

            pair->first->cancelRead();
            pair->first->cancelRead();
            CHECK_FALSE(pair->first->isClosed());
            CHECK(loop.parkedWaiterCount() == 0);

            // And the socket still moves bytes.
            auto wrote = false;
            auto got = std::size_t { 0 };
            loop.blockOn([](ISocket* from, ISocket* to, bool* w, std::size_t* g) -> Task<void> {
                auto const payload =
                    std::array<std::byte, 3> { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };
                auto const written = co_await from->write(std::span<std::byte const> { payload });
                *w = written.has_value() && *written == 3;
                auto buffer = std::array<std::byte, 8> {};
                auto const read = co_await to->read(buffer);
                if (read.has_value())
                    *g = *read;
            }(pair->second.get(), pair->first.get(), &wrote, &got));
            CHECK(wrote);
            CHECK(got == 3);
        }
    }
}

TEST_CASE("A second cancelRead in a row leaves the watch the retired flow arms afterwards",
          "[net][socket][cancelread]")
{
    // **It retires whatever is parked NOW**
    // ([fastcached#1233](https://github.com/LASTRADA-Software/fastcached/issues/1233)). Until 0.2.1
    // the first retirement resumed its victim inline, a flow that armed its next read there left a
    // NEW watch in the slot, and a second call in a row retired THAT one -- `Cancelled` on an
    // operation nobody meant to cancel. The victim now runs on the loop after both calls have
    // returned, so the second call finds the slot empty, and the watch the flow arms afterwards is
    // left parked until something meant for it arrives.
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

            auto first = WatchOutcome {};
            auto second = WatchOutcome {};
            auto parkedFirst = false;
            auto secondParkedAfter = false;
            loop.blockOn(watchTwiceAndRetire(
                &loop, pair->first.get(), &first, &second, &parkedFirst, &secondParkedAfter));

            CHECK(parkedFirst);
            REQUIRE(first.resolved);
            REQUIRE_FALSE(first.hasValue);
            CHECK(first.code == NetErrorCode::Cancelled);

            // The second call in a row did not reach the second watch: it was still parked a turn
            // later, and only the call meant for it retired it.
            CHECK(secondParkedAfter);
            REQUIRE(second.resolved);
            REQUIRE_FALSE(second.hasValue);
            CHECK(second.code == NetErrorCode::Cancelled);
        }
    }
}

TEST_CASE("A read retired while its own stop is pending leaves the next read untouched",
          "[net][socket][cancelread]")
{
    // A `withTimeout` loser: its token is stopped, and the stop is delivered on the NEXT turn -- but
    // the timeout's handler retires the stale read with `cancelRead` in this one. The retirement
    // resumes a flow whose token is stopped, so the read unwinds through `OperationCancelled`
    // rather than answering `Cancelled`, and whatever the retirement marked on the socket for that
    // answer must not outlive it: the NEXT read, woken by a real byte, returns the byte.
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

            auto stop = StopSource {};
            auto first = WatchOutcome {};
            auto next = WatchOutcome {};
            auto parkedFirst = false;
            auto timedOut = false;
            loop.blockOn(stoppedReaderRetiredThenReadAgain(
                &loop, pair->first.get(), pair->second.get(), &stop, &first, &parkedFirst, &next, &timedOut));

            CHECK(parkedFirst); // or nothing was retired and the rest of this case is vacuous
            REQUIRE(first.resolved);
            CHECK(first.threw); // its own token was stopped, so it unwound

            CHECK_FALSE(timedOut); // waited 5 s for the second read's byte
            REQUIRE(next.resolved);
            CHECK(next.code == NetErrorCode::Ok);
            REQUIRE(next.hasValue);
            CHECK(next.count == 1);
        }
    }
}

TEST_CASE("A read on a flow already stopped unwinds inside that flow, never past its handler",
          "[net][socket][cancelread]")
{
    // Nothing is armed: the operation sees the stop before it asks the owner for anything, and
    // resumes the flow at once to throw `OperationCancelled` -- the resume-at-once-then-throw shape
    // in which MSVC 19.44's ARM64 code generator lost the awaiting coroutine's handler for a `Task`
    // owning no frame (fastcached#1546, on the `windows (cl-release-arm64)` leg). The handler is
    // `readUnderToken`'s own `catch`; a throw that passed it would reach `blockOn` instead.
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

            auto stop = StopSource {};
            stop.request_stop();
            auto outcome = WatchOutcome {};
            loop.blockOn(readUnderToken(stop.get_token(), pair->first.get(), &outcome));

            REQUIRE(outcome.resolved);
            CHECK(outcome.threw);
            CHECK(loop.parkedWaiterCount() == 0);
        }
    }
}
