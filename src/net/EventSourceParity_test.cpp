// SPDX-License-Identifier: Apache-2.0
///
/// Every @c EventSource backend must be behaviourally interchangeable — the whole
/// point of the interface is that swapping poll(2) for epoll/kqueue changes only
/// what a wait costs. These cases therefore run the SAME scenario against every
/// backend available on this platform, so a divergence fails here rather than
/// surfacing as a hang in whatever happens to use the native source.
///
/// The `[closehang]` tag selects the family that guards one such divergence: a
/// descriptor CLOSED under a parked flow. poll(2) reports POLLNVAL for it and
/// Windows reports the handle as failed, but epoll silently drops it from the set
/// and kqueue silently drops its filters — so on those two the flow was never
/// resumed at all, and the cases below hung rather than failed. They are named for
/// that symptom because it is what a regression looks like: `net_test [closehang]`
/// runs the lot. See @c EventLoop::notifyHandleClosing for the mechanism.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>
#include <coro/WhenAny.hpp>
#include <net/DefaultEventSource.hpp>
#include <net/EventLoop.hpp>
#include <net/EventSource.hpp>
#include <net/ISocket.hpp>
#include <net/Sockets.hpp>
#include <net/platform/SystemPipe.hpp>
#include <net/testing/EventSourceBackends.hpp>
#include <net/testing/InMemoryTransport.hpp>

#ifndef _WIN32
    #include <sys/resource.h>
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using coro::Task;
using net::EventLoop;
using net::EventSourceKind;
using net::FdInterest;
using net::testing::AllBackends;

namespace
{

/// Writes to a pipe, then waits for the read end to become readable.
Task<void> waitThenRead(EventLoop* loop, net::SystemPipe* pipe, bool* observed)
{
    co_await loop->waitReadable(pipe->waitHandle());
    auto byte = std::array<std::byte, 1> {};
    auto const got = pipe->read(byte.data(), byte.size());
    *observed = got.has_value() && *got == 1;
}

/// Echoes one message over a connected socket pair, exercising both readiness
/// directions through whichever source the loop was built with.
Task<void> echoOnce(net::ISocket* client, net::ISocket* server, std::string* out)
{
    constexpr auto Message = std::string_view { "parity" };
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(Message.data()), Message.size() };
    static_cast<void>(co_await client->write(bytes));

    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await server->read(buffer);
    if (got.has_value())
        out->assign(reinterpret_cast<char const*>(buffer.data()), *got);
}

/// The server flow: accept one connection and read the message it sends.
Task<void> acceptAndEcho(net::IListener* listener, std::string* out)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto socket = std::move(*accepted);
    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await socket->read(buffer);
    if (got.has_value())
        out->assign(reinterpret_cast<char const*>(buffer.data()), *got);
}

/// Runs the accept and connect flows concurrently to completion.
Task<void> echoOverListener(EventLoop* loop, net::IListener* listener, std::uint16_t port, std::string* out);

/// The client flow: connect to @p port and send the message the server expects.
Task<void> connectAndSend(EventLoop* loop, std::uint16_t port)
{
    auto connected = co_await net::connect(loop, "127.0.0.1", port);
    if (!connected.has_value())
        co_return;
    auto socket = std::move(*connected);
    constexpr auto Message = std::string_view { "parity" };
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(Message.data()), Message.size() };
    static_cast<void>(co_await socket->write(bytes));
}

Task<void> echoOverListener(EventLoop* loop, net::IListener* listener, std::uint16_t port, std::string* out)
{
    co_await coro::whenAll(acceptAndEcho(listener, out), connectAndSend(loop, port));
}

/// Parks reading an idle socket and records whether the read resumed with an error
/// rather than hanging forever.
Task<void> parkThenObserveClose(net::ISocket* sock, bool* resumedWithError)
{
    auto buffer = std::array<std::byte, 64> {};
    auto const result = co_await sock->read(buffer);
    *resumedWithError = !result.has_value();
}

/// Lets the reader reach its park, then closes the socket it is parked on.
Task<void> closeAfterParked(EventLoop* loop, net::ISocket* sock)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    sock->close();
}

/// Runs the parked reader and the close concurrently on one loop.
Task<void> closeWhileParked(EventLoop* loop, net::ISocket* sock, bool* resumedWithError)
{
    co_await coro::whenAll(parkThenObserveClose(sock, resumedWithError), closeAfterParked(loop, sock));
}

/// Reads once, recording the byte count so a clean EOF is distinguishable from an
/// error and from a hang.
Task<void> readOnce(net::ISocket* sock, int* outcome)
{
    auto buffer = std::array<std::byte, 64> {};
    auto const result = co_await sock->read(buffer);
    *outcome = result.has_value() ? static_cast<int>(*result) : -1;
}

/// Parks on accept() and records whether it resumed (with a failure) rather than
/// hanging once the listener beneath it is closed.
Task<void> parkThenObserveListenerClose(net::IListener* listener, bool* accepted)
{
    auto const result = co_await listener->accept();
    *accepted = result.has_value();
}

/// Lets the accept reach its park, then closes the listener it is parked on.
Task<void> closeListenerAfterParked(EventLoop* loop, net::IListener* listener)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    listener->close();
}

/// Runs the parked accept and the listener close concurrently on one loop.
Task<void> acceptThenClose(EventLoop* loop, net::IListener* listener, bool* accepted)
{
    co_await coro::whenAll(parkThenObserveListenerClose(listener, accepted),
                           closeListenerAfterParked(loop, listener));
}

/// Writes to a socket whose peer never reads, until the socket refuses.
///
/// Deliberately a LOOP rather than one enormous write. On a POSIX socket pair the
/// send and receive buffers fill within a turn or two and the write parks on
/// writability, which is the case this exercises. Windows auto-tunes its buffers
/// far higher, so a fixed payload — 4 MiB was the first attempt — is simply
/// absorbed and the write succeeds, never parking; the assertion then failed for a
/// reason that had nothing to do with the code under test. Looping with a yield
/// between turns makes the outcome the same everywhere: whether the writer parks
/// and the close resumes it, or the close simply lands between two turns, a writer
/// must end up learning the socket is gone rather than hanging.
/// @param loop The loop to yield to between turns.
/// @param sock The socket to write to (its peer never reads).
/// @param resumedWithError Set to true once a write reports a failure.
Task<void> parkThenObserveWriteClose(EventLoop* loop, net::ISocket* sock, bool* resumedWithError)
{
    constexpr auto ChunkBytes = std::size_t { 256 } * 1024;
    auto const chunk = std::vector<std::byte>(ChunkBytes, std::byte { 'x' });
    // Bounded so a backend that never refuses fails the CHECK instead of hanging.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 512))
    {
        if (auto const result = co_await sock->write(chunk); !result.has_value())
        {
            *resumedWithError = true;
            co_return;
        }
        // Yield, so the flow that closes the socket still gets to run on a platform
        // where the write never had to park.
        co_await loop->delay(std::chrono::milliseconds { 1 });
    }
}

/// Runs the parked writer and the close concurrently on one loop.
Task<void> closeWhileWriteParked(EventLoop* loop, net::ISocket* sock, bool* resumedWithError)
{
    co_await coro::whenAll(parkThenObserveWriteClose(loop, sock, resumedWithError),
                           closeAfterParked(loop, sock));
}

/// Parks a reader AND a writer on ONE socket, then closes it under both. Two
/// registrations on one descriptor is the case epoll and kqueue cannot express
/// natively (an epoll set rejects a duplicate with EEXIST; a kqueue filter is keyed
/// by descriptor), so each keeps a private dup() for the second one — and a close
/// has to resume both flows and release that duplicate.
/// @param loop The loop driving both parks.
/// @param sock The socket both flows park on.
/// @param readerResumed Set when the reader resumed with an error.
/// @param writerResumed Set when the writer resumed with an error.
Task<void> closeWhileBothParked(EventLoop* loop, net::ISocket* sock, bool* readerResumed, bool* writerResumed)
{
    co_await coro::whenAll(parkThenObserveClose(sock, readerResumed),
                           parkThenObserveWriteClose(loop, sock, writerResumed),
                           closeAfterParked(loop, sock));
}

/// Closes @p sock twice with nothing parked on it, then proves the loop still
/// works by running a delay to completion. A close that recorded a wake for a
/// registration that no longer exists — or recorded one twice — would strand or
/// double-resume whatever ran next.
/// @param loop The loop to keep using afterwards.
/// @param sock The socket to close (twice).
/// @param stillPumps Set to true once the trailing delay completed.
Task<void> closeTwiceThenKeepPumping(EventLoop* loop, net::ISocket* sock, bool* stillPumps)
{
    sock->close();
    sock->close(); // idempotent: the second must not record a second wake
    co_await loop->delay(std::chrono::milliseconds { 1 });
    *stillPumps = true;
}

/// Parks a reader on @p idle, closes @p closing under its own parked reader, and
/// checks that only the closed one resumed — the wake must be routed by descriptor,
/// not broadcast.
/// @param loop The loop driving both parks.
/// @param closing The socket to close.
/// @param idle The socket that stays open with a reader parked on it.
/// @param closedResumed Set when the reader on @p closing resumed.
/// @param idleResumed Set when the reader on @p idle resumed (it must not).
Task<void> closeOneOfTwo(
    EventLoop* loop, net::ISocket* closing, net::ISocket* idle, bool* closedResumed, bool* idleResumed)
{
    // whenAny, not whenAll: the reader on `idle` is expected NEVER to resume, so
    // waiting for it would hang by design. The losing branch is cancelled.
    static_cast<void>(co_await coro::whenAny(closeWhileParked(loop, closing, closedResumed),
                                             parkThenObserveClose(idle, idleResumed)));
}

/// Parks a reader on a socket that is closed under it, while a timer is also
/// pending. A close-wake must not swallow the pump's wait: the timer has to fire
/// too, in the same pump or the next one.
/// @param loop The loop driving both.
/// @param sock The socket to park on and close.
/// @param readerResumed Set when the reader resumed.
/// @param timerFired Set when the pending delay elapsed.
Task<void> closeWhileTimerPending(EventLoop* loop, net::ISocket* sock, bool* readerResumed, bool* timerFired)
{
    auto tick = [](EventLoop* inner, bool* fired) -> Task<void> {
        co_await inner->delay(std::chrono::milliseconds { 40 });
        *fired = true;
    };
    co_await coro::whenAll(
        parkThenObserveClose(sock, readerResumed), closeAfterParked(loop, sock), tick(loop, timerFired));
}

/// Parks reading a socket and records that it unwound through OperationCancelled
/// rather than resuming on its normal path. That distinction is the whole point of
/// @c FdWakePolicy::Cancel: the normal path would re-read the socket's members
/// through a `this` that no longer exists.
/// @param sock The socket to park on (destroyed under this flow).
/// @param cancelled Set to true if the read unwound as cancelled.
Task<void> parkThenRecordCancellation(net::ISocket* sock, bool* cancelled)
{
    auto buffer = std::array<std::byte, 64> {};
    try
    {
        static_cast<void>(co_await sock->read(buffer));
    }
    catch (coro::OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// Lets the reader reach its park, then DESTROYS the socket it is parked on.
/// @param loop The loop driving the delay.
/// @param owner The owning pointer to reset.
Task<void> destroySocketAfterParked(EventLoop* loop, std::unique_ptr<net::ISocket>* owner)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    owner->reset(); // ~PosixSocket -> close(FdWakePolicy::Cancel)
}

/// Runs the parked reader and the socket's destruction concurrently on one loop.
Task<void> destroyWhileParked(EventLoop* loop, std::unique_ptr<net::ISocket>* owner, bool* cancelled)
{
    co_await coro::whenAll(parkThenRecordCancellation(owner->get(), cancelled),
                           destroySocketAfterParked(loop, owner));
}

/// Lets the accept reach its park, then DESTROYS the listener beneath it.
Task<void> destroyListenerAfterParked(EventLoop* loop, std::unique_ptr<net::IListener>* owner)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    owner->reset(); // ~PosixListener -> close(FdWakePolicy::Cancel)
}

/// Runs the parked accept and the listener's destruction concurrently on one loop.
Task<void> destroyListenerWhileParked(EventLoop* loop, std::unique_ptr<net::IListener>* owner, bool* accepted)
{
    co_await coro::whenAll(parkThenObserveListenerClose(owner->get(), accepted),
                           destroyListenerAfterParked(loop, owner));
}

/// Reads once and counts the resume, however it arrives. A count above one means
/// the same coroutine was queued twice — the use-after-free the close-wake path had
/// to be designed around.
/// @param sock The socket to park on.
/// @param count Incremented exactly once, when the read resolves or unwinds.
Task<void> countOneResume(net::ISocket* sock, int* count)
{
    auto buffer = std::array<std::byte, 64> {};
    try
    {
        static_cast<void>(co_await sock->read(buffer));
        ++*count;
    }
    catch (coro::OperationCancelled const&)
    {
        // Cancellation is a resume too — and the one ~EventLoop delivers. Counted
        // in both arms, so either route through here registers exactly once.
        ++*count;
    }
}

/// Like @c countOneResume, but requests stop the moment it wakes — while a sibling
/// woken by the same close is still sitting in the ready queue with its cancellation
/// callback armed. That is exactly the window in which the previous attempt queued
/// the sibling a SECOND time and then resumed a frame the first resume had
/// destroyed.
/// @param loop The loop to stop.
/// @param sock The socket to park on.
/// @param count Incremented once, when the read resolves or unwinds.
Task<void> countOneResumeThenRequestStop(EventLoop* loop, net::ISocket* sock, int* count)
{
    co_await countOneResume(sock, count);
    loop->requestStop();
}

/// Accepts once and counts the resume, however it arrives.
Task<void> countOneAccept(net::IListener* listener, int* count)
{
    static_cast<void>(co_await listener->accept());
    ++*count;
}

/// Closes both sockets in one go, then keeps the pump alive long enough for the
/// resulting wakes to be delivered.
/// @param loop The loop driving both delays.
/// @param first The first socket to close.
/// @param second The second socket to close.
/// @param stopRequested Set when the trailing delay was cancelled — which proves a
///        woken reader ran and called requestStop() while its sibling was still
///        queued, i.e. that the case actually reached the window it exists to test.
Task<void> closeBothThenSettle(EventLoop* loop,
                               net::ISocket* first,
                               net::ISocket* second,
                               bool* stopRequested)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    first->close();
    second->close();
    try
    {
        co_await loop->delay(std::chrono::milliseconds { 20 });
    }
    catch (coro::OperationCancelled const&)
    {
        *stopRequested = true;
    }
}

/// Suspends briefly so spawned flows reach their parks before the caller acts.
Task<void> settle(EventLoop* loop)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
}

} // namespace

TEST_CASE("every available event source reports pipe readability", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pipe = net::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            auto observed = false;
            loop.blockOn(waitThenRead(&loop, pipe->get(), &observed));
            REQUIRE(observed);
        }
    }
}

TEST_CASE("every available event source drives a socket round-trip", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto got = std::string {};
            loop.blockOn(echoOnce(pair->first.get(), pair->second.get(), &got));
            REQUIRE(got == "parity");
        }
    }
}

TEST_CASE("every available event source serves a loopback listener", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());
            auto const port = (*listener)->localPort();
            REQUIRE(port != 0);

            // accept() and connect() park on opposite readiness directions, so this
            // exercises the attach/interest-update path the reconciliation covers.
            // They must run as CONCURRENT flows: a Task is lazy, so a connect that
            // is merely created (not awaited) never runs, and accept would wait for
            // a connection nobody initiated.
            auto got = std::string {};
            loop.blockOn(echoOverListener(&loop, listener->get(), port, &got));
            REQUIRE(got == "parity");
        }
    }
}

// Regression guard for the close-wake path. A readiness poller cannot report a
// CLOSED descriptor: epoll drops it from the set and kqueue drops its filters, both
// silently, so a flow parked on it used to hang for ever. poll(2) reports POLLNVAL
// and Windows reports the handle as failed, which is why only the native backends
// were broken. EventLoop::notifyHandleClosing supplies the missing readiness, and
// these cases HUNG until it existed.
TEST_CASE("closing a socket resumes a parked reader on every event source",
          "[net][eventsource][parity][closehang]")
{
    // Socket_test covers this only for PollEventSource, so it stayed green while the
    // native backends were broken. Driving it through the loop on every backend is
    // what catches a source that holds a descriptor the socket thinks it closed.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto resumedWithError = false;
            loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));
            CHECK(resumedWithError); // resumed at all (no hang) AND saw the close
        }
    }
}

TEST_CASE("closing a socket resumes a parked writer on every event source",
          "[net][eventsource][parity][closehang]")
{
    // The write direction of the same hazard. A writer parks on WRITABILITY, so it
    // is registered with a different interest than the reader above — and a backend
    // that only routed the read side of a close would hang here instead.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto resumedWithError = false;
            loop.blockOn(closeWhileWriteParked(&loop, pair->second.get(), &resumedWithError));
            CHECK(resumedWithError);
        }
    }
}

TEST_CASE("closing a socket resumes BOTH flows parked on it on every event source",
          "[net][eventsource][parity][closehang]")
{
    // Two registrations on one descriptor is the shape epoll and kqueue cannot hold
    // natively, so each keeps a private dup() for the second. A close must resume
    // both flows -- and detach both registrations, or that duplicate would keep the
    // peer's connection open past the close it was supposed to end.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto readerResumed = false;
            auto writerResumed = false;
            loop.blockOn(closeWhileBothParked(&loop, pair->second.get(), &readerResumed, &writerResumed));
            CHECK(readerResumed);
            CHECK(writerResumed);
        }
    }
}

TEST_CASE("closing a socket wakes only the flows parked on it", "[net][eventsource][parity][closehang]")
{
    // The wake is routed by descriptor, not broadcast: a reader on an untouched
    // socket must stay parked. A close that woke every waiter would look like it
    // worked -- until an unrelated flow resumed early and read from a live socket
    // that had nothing to give.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto closing = net::testing::makeSocketPair(loop);
            auto idle = net::testing::makeSocketPair(loop);
            REQUIRE(closing.has_value());
            REQUIRE(idle.has_value());

            auto closedResumed = false;
            auto idleResumed = false;
            loop.blockOn(closeOneOfTwo(
                &loop, closing->second.get(), idle->second.get(), &closedResumed, &idleResumed));
            CHECK(closedResumed);
            CHECK_FALSE(idleResumed); // never woken: nothing closed under it
        }
    }
}

TEST_CASE("closing a socket with nothing parked leaves the loop usable",
          "[net][eventsource][parity][closehang]")
{
    // Closing an idle socket records no wake at all, and closing it twice records
    // no second one. Either mistake would leave a token queued for a registration
    // that no longer exists, and the next pump would resume whatever now answers to
    // it -- or spin delivering a wake nobody claims.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto stillPumps = false;
            loop.blockOn(closeTwiceThenKeepPumping(&loop, pair->second.get(), &stillPumps));
            CHECK(stillPumps);
        }
    }
}

TEST_CASE("a close-wake does not swallow a pending timer", "[net][eventsource][parity][closehang]")
{
    // The pump merges close-wakes INTO its wait outcome rather than short-circuiting
    // and returning early. Skipping the wait would strand every other thing due in
    // that pump -- a peer's EOF, or this timer -- until some later pump that may
    // never come, because blockOn exits as soon as its root flow is done.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto readerResumed = false;
            auto timerFired = false;
            loop.blockOn(closeWhileTimerPending(&loop, pair->second.get(), &readerResumed, &timerFired));
            CHECK(readerResumed);
            CHECK(timerFired);
        }
    }
}

TEST_CASE("destroying a socket under a parked reader cancels it", "[net][eventsource][parity][closehang]")
{
    // A destructor cannot use the same wake an explicit close() does. Resuming the
    // flow on its NORMAL path would send it back into PosixSocket::read, which
    // re-reads _closed and _fd through a `this` that has just stopped existing.
    // FdWakePolicy::Cancel makes await_resume throw instead, so the frame unwinds
    // without ever re-entering its body. Run under ASan, this case is the guard.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto cancelled = false;
            loop.blockOn(destroyWhileParked(&loop, &pair->second, &cancelled));
            CHECK(cancelled); // unwound as cancelled, not resumed into a dead object
        }
    }
}

TEST_CASE("destroying a listener under a parked accept cancels it", "[net][eventsource][parity][closehang]")
{
    // The listener form of the same rule, and the sharper one: acceptOne holds
    // `int const* fd` and `bool const* closed` pointing INTO the listener, and
    // re-reads both at the top of every turn. A normal-path resume would dereference
    // them after the listener was destroyed; the cancelling resume returns from the
    // catch without touching either.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto accepted = true;
            loop.blockOn(destroyListenerWhileParked(&loop, &*listener, &accepted));
            CHECK_FALSE(accepted);
        }
    }
}

TEST_CASE("a close recorded before the loop dies resumes its flow exactly once",
          "[net][eventsource][parity][closehang]")
{
    // close() records a wake for the next pump — but if the loop is destroyed first
    // that pump never comes. ~EventLoop must then be the ONLY thing that resumes the
    // flow, through its own request_stop()-first path. Consuming the recorded wake
    // there as well would resume the same coroutine twice: the first resume runs it
    // to completion and its owner destroys the frame, and the second calls .done()
    // on freed memory.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto resumeCount = 0;
            {
                auto loop = EventLoop { *source };
                auto pair = net::testing::makeSocketPair(loop);
                REQUIRE(pair.has_value());

                loop.spawn(countOneResume(pair->second.get(), &resumeCount));
                loop.blockOn(settle(&loop)); // the reader reaches its park
                REQUIRE(resumeCount == 0);

                pair->second->close(); // records a wake no pump will ever consume
            } // ~EventLoop: cancels and drains
            CHECK(resumeCount == 1);
        }
    }
}

TEST_CASE("closing every listener and then requesting stop resumes each accept once",
          "[net][eventsource][parity][closehang]")
{
    // requestDaemonShutdown's sequence, verbatim: close every listener, THEN
    // requestStop(). This is the shape that segfaulted vthost_test and
    // contour_gui_test on every platform when close() queued the parked flow itself:
    // the queued coroutine still had its cancellation callback armed, so
    // request_stop() queued it a second time.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto resumeCount = 0;
            {
                auto loop = EventLoop { *source };
                auto listeners = std::vector<std::unique_ptr<net::IListener>> {};
                for ([[maybe_unused]] auto const each: std::views::iota(0, 3))
                {
                    auto listener = net::listen(loop, "127.0.0.1", 0);
                    REQUIRE(listener.has_value());
                    listeners.push_back(std::move(*listener));
                }
                for (auto const& listener: listeners)
                    loop.spawn(countOneAccept(listener.get(), &resumeCount));
                loop.blockOn(settle(&loop)); // all three accepts reach their parks
                REQUIRE(resumeCount == 0);

                for (auto const& listener: listeners)
                    listener->close();
                loop.requestStop();
            } // ~EventLoop: drains what is left
            CHECK(resumeCount == 3); // once each -- never twice
        }
    }
}

TEST_CASE("a close-woken flow may request stop while a sibling is still queued",
          "[net][eventsource][parity][closehang]")
{
    // Both readers are woken by the same pump. The first to resume requests stop
    // while the second is still sitting in the ready queue, un-resumed and with its
    // cancellation callback still armed — the callback only disarms in await_resume.
    // requeueForCancellation must recognise that the sibling is already queued and
    // NOT push it again.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto resumeCount = 0;
            auto stopRequested = false;
            {
                auto loop = EventLoop { *source };
                auto first = net::testing::makeSocketPair(loop);
                auto second = net::testing::makeSocketPair(loop);
                REQUIRE(first.has_value());
                REQUIRE(second.has_value());

                loop.spawn(countOneResumeThenRequestStop(&loop, first->second.get(), &resumeCount));
                loop.spawn(countOneResume(second->second.get(), &resumeCount));
                loop.blockOn(
                    closeBothThenSettle(&loop, first->second.get(), second->second.get(), &stopRequested));
            }
            CHECK(stopRequested);    // the woken reader really did request stop
            CHECK(resumeCount == 2); // once each -- never twice
        }
    }
}

TEST_CASE("a peer's close is delivered as EOF on every event source", "[net][eventsource][parity]")
{
    // The end-to-end form of the descriptor-ownership rule: the reader goes through
    // EventLoop and ISocket rather than touching the source directly, so a backend
    // that keeps the peer's file description alive shows up as a read that never
    // reports EOF.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            pair->first->close(); // the peer goes away

            auto outcome = -99;
            loop.blockOn(readOnce(pair->second.get(), &outcome));
            CHECK(outcome == 0); // 0 == clean EOF; -1 would be an error, -99 a hang
        }
    }
}

TEST_CASE("an attached token is reported and a detached one is not", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto pipe = net::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto const token = source->attach((*pipe)->waitHandle(), FdInterest::Read);
            REQUIRE(token);

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            auto const ready = source->wait(200);
            REQUIRE(std::ranges::find(ready.readyRead, token) != ready.readyRead.end());

            // After detaching, the same still-readable fd must not be reported.
            source->detach(token);
            auto const afterDetach = source->wait(0);
            REQUIRE(afterDetach.readyRead.empty());
            REQUIRE(afterDetach.readyWrite.empty());
        }
    }
}

TEST_CASE("two registrations on one descriptor are accepted by every event source",
          "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto pipe = net::createSystemPipe();
            REQUIRE(pipe.has_value());

            // FdRegistry permits it and poll(2) takes two entries, so the native
            // backends must too — an epoll set is keyed by descriptor and would
            // otherwise refuse the second with EEXIST, and a kqueue filter is keyed
            // by (descriptor, filter) and would replace rather than add.
            auto const first = source->attach((*pipe)->waitHandle(), FdInterest::Read);
            auto const second = source->attach((*pipe)->waitHandle(), FdInterest::Read);
            REQUIRE(first);
            REQUIRE(second);
            REQUIRE(first != second);

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            // At least one registration is reported. NOT both: whether a duplicated
            // descriptor yields one ready entry or two is the multiplexer's own
            // business — Linux's poll(2) fills in every matching pollfd, macOS's
            // reports the descriptor once — and EventSource deliberately does not
            // promise either. What it does promise is that a registration is
            // accepted and that readiness reaches somebody.
            auto const ready = source->wait(200);
            auto const sawFirst = std::ranges::find(ready.readyRead, first) != ready.readyRead.end();
            auto const sawSecond = std::ranges::find(ready.readyRead, second) != ready.readyRead.end();
            CHECK((sawFirst || sawSecond));

            // Detaching one must not disturb the other: they are separate kernel
            // registrations, so dropping one cannot take the survivor's with it.
            // This is the property the dup() exists to provide, and it holds
            // everywhere regardless of how duplicates are reported above.
            source->detach(first);
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());
            auto const afterOne = source->wait(200);
            CHECK(std::ranges::find(afterOne.readyRead, second) != afterOne.readyRead.end());
            CHECK(std::ranges::find(afterOne.readyRead, first) == afterOne.readyRead.end());

            source->detach(second);
        }
    }
}

#ifndef _WIN32
TEST_CASE("a registration does not keep a closed descriptor's connection alive", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto sv = std::array<int, 2> {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

            // Attach one end, then close it WITHOUT detaching first — the ordering
            // a cancelled flow or an early socket destructor produces. A backend
            // that registers a dup() of the descriptor keeps the underlying open
            // file description alive, so no FIN reaches the peer: its read blocks
            // forever instead of reporting EOF, and the connection leaks.
            auto const token = source->attach(sv[0], FdInterest::Read);
            REQUIRE(token);
            ::close(sv[0]);

            // The peer must see EOF now. Read non-blocking so a backend that holds
            // the description open fails the assertion instead of hanging the suite.
            auto const flags = ::fcntl(sv[1], F_GETFL, 0);
            ::fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);
            auto buffer = std::array<char, 16> {};
            auto const got = ::read(sv[1], buffer.data(), buffer.size());
            CHECK(got == 0); // 0 == EOF; -1/EAGAIN means the FIN never arrived

            source->detach(token);
            ::close(sv[1]);
        }
    }
}
#endif

#ifndef _WIN32
TEST_CASE("a duplicate registration refuses cleanly when descriptors run out", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto pipe = net::createSystemPipe();
            REQUIRE(pipe.has_value());

            // The first registration watches the caller's descriptor directly, so it
            // needs no descriptor of its own.
            auto const first = source->attach((*pipe)->waitHandle(), FdInterest::Read);
            REQUIRE(first);

            // A second registration on the same descriptor needs a private dup().
            // Lower the soft descriptor limit to what is already open so that dup()
            // must fail, and require a refusal rather than a token for a registration
            // the kernel never accepted — parking on one of those is unresumable.
            // The soft limit is restored below, so this stays scoped to this case
            // rather than starving the rest of the suite.
            auto limit = rlimit {};
            REQUIRE(::getrlimit(RLIMIT_NOFILE, &limit) == 0);
            auto const originalSoft = limit.rlim_cur;

            auto const probe = ::dup(0); // the lowest descriptor still free
            REQUIRE(probe >= 0);
            auto squeezed = limit;
            squeezed.rlim_cur = static_cast<rlim_t>(probe); // no descriptor >= probe may be opened
            REQUIRE(::setrlimit(RLIMIT_NOFILE, &squeezed) == 0);
            ::close(probe);

            auto const underPressure = source->attach((*pipe)->waitHandle(), FdInterest::Read);

            limit.rlim_cur = originalSoft;
            REQUIRE(::setrlimit(RLIMIT_NOFILE, &limit) == 0);

            // What must hold on EVERY backend is that the answer is honest: either
            // the registration was refused, or it was genuinely armed. What must
            // never happen is a valid token for a registration the kernel does not
            // have. poll(2) needs no descriptor of its own, so it legitimately
            // succeeds here; epoll and kqueue must dup() and so must refuse.
            if (backend.kind == EventSourceKind::Poll)
                CHECK(underPressure);
            else
                CHECK_FALSE(underPressure);

            // Recovery: with descriptors available again, a duplicate must work.
            auto const afterRecovery = source->attach((*pipe)->waitHandle(), FdInterest::Read);
            CHECK(afterRecovery);

            source->detach(afterRecovery);
            if (underPressure)
                source->detach(underPressure); // poll(2) succeeded above
            source->detach(first);
        }
    }
}
#endif

TEST_CASE("an invalid handle is refused by every event source", "[net][eventsource][parity]")
{
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            // A refused registration must report invalid rather than succeeding:
            // the awaiting flow has to fail instead of parking on an interest the
            // kernel never accepted, which nothing could resume.
            auto const token = source->attach(net::InvalidHandle, FdInterest::Read);
            REQUIRE_FALSE(token);
        }
    }
}

TEST_CASE("the default source drives the scenarios Socket_test pins to poll",
          "[net][eventsource][parity][closehang]")
{
    // Socket_test hardcodes PollEventSource in all of its cases, which is why two
    // native-backend defects (a registration holding the peer's connection open, and
    // a parked reader never resuming after close) passed a green suite. These run the
    // same shapes against whatever makeDefaultEventSource picks -- epoll on Linux,
    // kqueue on macOS/BSD -- so the backend production actually uses is exercised.
    auto source = net::makeDefaultEventSource();
    REQUIRE(source != nullptr);

    SECTION("loopback echo")
    {
        auto loop = EventLoop { *source };
        auto listener = net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        auto const port = (*listener)->localPort();
        REQUIRE(port != 0);

        auto got = std::string {};
        loop.blockOn(echoOverListener(&loop, listener->get(), port, &got));
        CHECK(got == "parity");
    }

    SECTION("close resumes a parked reader")
    {
        auto loop = EventLoop { *source };
        auto pair = net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());

        auto resumedWithError = false;
        loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));
        CHECK(resumedWithError);
    }

    SECTION("a peer's close reads as EOF")
    {
        auto loop = EventLoop { *source };
        auto pair = net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());

        pair->first->close();

        auto outcome = -99;
        loop.blockOn(readOnce(pair->second.get(), &outcome));
        CHECK(outcome == 0);
    }
}

// Same hazard as the parked reader, one layer up; it HUNG on epoll/kqueue until
// notifyHandleClosing existed.
TEST_CASE("closing a listener resumes a parked accept on every event source",
          "[net][eventsource][parity][closehang]")
{
    // Same hazard as a parked reader, one layer up: accept() parks on waitReadable,
    // so a listener closed while an accept is pending must resume it rather than
    // leave it parked on a descriptor the poller can no longer report.
    for (auto const& backend: AllBackends)
    {
        auto source = net::makeEventSource(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto listener = net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto accepted = true;
            loop.blockOn(acceptThenClose(&loop, listener->get(), &accepted));
            CHECK_FALSE(accepted); // it resumed at all, and reported the close
        }
    }
}

TEST_CASE("makeDefaultEventSource yields a usable source", "[net][eventsource]")
{
    auto source = net::makeDefaultEventSource();
    REQUIRE(source != nullptr);

    // Whatever it picked must drive a real round-trip.
    auto loop = EventLoop { *source };
    auto pair = net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto got = std::string {};
    loop.blockOn(echoOnce(pair->first.get(), pair->second.get(), &got));
    REQUIRE(got == "parity");
}

TEST_CASE("the preferred backend is constructible on this platform", "[net][eventsource]")
{
    // If the platform names a native backend, it must actually build here — a
    // silent permanent fallback to poll would mean the port is not exercised at all.
    auto const preferred = net::preferredEventSourceKind();
    auto source = net::makeEventSource(preferred);
    REQUIRE(source != nullptr);
}
