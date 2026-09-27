// SPDX-License-Identifier: Apache-2.0
///
/// Every @c IoBackend must be behaviourally interchangeable — the whole point of the
/// interface is that swapping poll(2) for epoll, kqueue or a completion port
/// changes only what a wait costs. These cases therefore run the SAME scenario
/// against every backend built on this platform, so a divergence fails here rather
/// than surfacing as a hang in whatever happens to use the native one.
///
/// The `[closehang]` tag selects the family that guards one such divergence: a
/// descriptor CLOSED under a parked flow. poll(2) reports POLLNVAL for it and
/// Windows reports the handle as failed, but epoll silently drops it from the set
/// and kqueue silently drops its filters — so on those two the flow was never
/// resumed at all, and the cases below hung rather than failed. They are named for
/// that symptom because it is what a regression looks like: `net_test [closehang]`
/// runs the lot. See @c EventLoop::notifyHandleClosing for the mechanism.
#ifdef _WIN32
    // winsock2.h MUST precede windows.h, and this is the only part of the parity suite
    // that needs either: the console-input case below is the one property here that has
    // no portable spelling at all.
    // clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on
#endif

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/HostDrivenBackend.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/ManualHostScheduler.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <sys/resource.h>
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using core::async::Task;
using core::net::BackendKind;
using core::net::EventLoop;
using core::net::Interest;
using core::net::testing::BackendMatrix;

namespace
{

/// Writes to a pipe, then waits for the read end to become readable.
Task<void> waitThenRead(EventLoop* loop, core::platform::SystemPipe* pipe, bool* observed)
{
    co_await loop->waitReadable(pipe->waitHandle());
    auto byte = std::array<std::byte, 1> {};
    auto const got = pipe->read(byte.data(), byte.size());
    *observed = got.has_value() && got->bytesRead() == 1;
}

/// Echoes one message over a connected socket pair, exercising both readiness
/// directions through whichever source the loop was built with.
Task<void> echoOnce(core::net::ISocket* client, core::net::ISocket* server, std::string* out)
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
Task<void> acceptAndEcho(core::net::IListener* listener, std::string* out)
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
Task<void> echoOverListener(EventLoop* loop, core::net::IListener* listener, std::string* out);

/// The client flow: connect to @p listener's port and send the message the server expects.
///
/// Closes @p listener when it cannot connect. An arm that simply returns leaves its whenAll
/// sibling parked in accept() with nothing left to wake it, which turns the case's red into a
/// hang just as surely as a REQUIRE here would (.agent/rules/testing.md).
Task<void> connectAndSend(EventLoop* loop, core::net::IListener* listener)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto socket = std::move(*connected);
    constexpr auto Message = std::string_view { "parity" };
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(Message.data()), Message.size() };
    static_cast<void>(co_await socket->write(bytes));
}

Task<void> echoOverListener(EventLoop* loop, core::net::IListener* listener, std::string* out)
{
    co_await core::async::whenAll(acceptAndEcho(listener, out), connectAndSend(loop, listener));
}

/// Parks reading an idle socket and records whether the read resumed with an error
/// rather than hanging forever.
Task<void> parkThenObserveClose(core::net::ISocket* sock, bool* resumedWithError)
{
    auto buffer = std::array<std::byte, 64> {};
    auto const result = co_await sock->read(buffer);
    *resumedWithError = !result.has_value();
}

/// Lets the reader reach its park, then closes the socket it is parked on.
Task<void> closeAfterParked(EventLoop* loop, core::net::ISocket* sock)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    sock->close();
}

/// Runs the parked reader and the close concurrently on one loop.
Task<void> closeWhileParked(EventLoop* loop, core::net::ISocket* sock, bool* resumedWithError)
{
    co_await core::async::whenAll(parkThenObserveClose(sock, resumedWithError), closeAfterParked(loop, sock));
}

/// Reads once, recording the byte count so a clean EOF is distinguishable from an
/// error and from a hang.
Task<void> readOnce(core::net::ISocket* sock, int* outcome)
{
    auto buffer = std::array<std::byte, 64> {};
    auto const result = co_await sock->read(buffer);
    *outcome = result.has_value() ? static_cast<int>(*result) : -1;
}

/// Parks on accept() and records whether it resumed (with a failure) rather than
/// hanging once the listener beneath it is closed.
Task<void> parkThenObserveListenerClose(core::net::IListener* listener, bool* accepted)
{
    auto const result = co_await listener->accept();
    *accepted = result.has_value();
}

/// Lets the accept reach its park, then closes the listener it is parked on.
Task<void> closeListenerAfterParked(EventLoop* loop, core::net::IListener* listener)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    listener->close();
}

/// Runs the parked accept and the listener close concurrently on one loop.
Task<void> acceptThenClose(EventLoop* loop, core::net::IListener* listener, bool* accepted)
{
    co_await core::async::whenAll(parkThenObserveListenerClose(listener, accepted),
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
Task<void> parkThenObserveWriteClose(EventLoop* loop, core::net::ISocket* sock, bool* resumedWithError)
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
Task<void> closeWhileWriteParked(EventLoop* loop, core::net::ISocket* sock, bool* resumedWithError)
{
    co_await core::async::whenAll(parkThenObserveWriteClose(loop, sock, resumedWithError),
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
Task<void> closeWhileBothParked(EventLoop* loop,
                                core::net::ISocket* sock,
                                bool* readerResumed,
                                bool* writerResumed)
{
    co_await core::async::whenAll(parkThenObserveClose(sock, readerResumed),
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
Task<void> closeTwiceThenKeepPumping(EventLoop* loop, core::net::ISocket* sock, bool* stillPumps)
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
Task<void> closeOneOfTwo(EventLoop* loop,
                         core::net::ISocket* closing,
                         core::net::ISocket* idle,
                         bool* closedResumed,
                         bool* idleResumed)
{
    // whenAny, not whenAll: the reader on `idle` is expected NEVER to resume, so
    // waiting for it would hang by design. The losing branch is cancelled.
    static_cast<void>(co_await core::async::whenAny(closeWhileParked(loop, closing, closedResumed),
                                                    parkThenObserveClose(idle, idleResumed)));
}

/// Parks a reader on a socket that is closed under it, while a timer is also
/// pending. A close-wake must not swallow the pump's wait: the timer has to fire
/// too, in the same pump or the next one.
/// @param loop The loop driving both.
/// @param sock The socket to park on and close.
/// @param readerResumed Set when the reader resumed.
/// @param timerFired Set when the pending delay elapsed.
Task<void> closeWhileTimerPending(EventLoop* loop,
                                  core::net::ISocket* sock,
                                  bool* readerResumed,
                                  bool* timerFired)
{
    auto tick = [](EventLoop* inner, bool* fired) -> Task<void> {
        co_await inner->delay(std::chrono::milliseconds { 40 });
        *fired = true;
    };
    co_await core::async::whenAll(
        parkThenObserveClose(sock, readerResumed), closeAfterParked(loop, sock), tick(loop, timerFired));
}

/// Parks reading a socket and records that it unwound through OperationCancelled
/// rather than resuming on its normal path. That distinction is the whole point of
/// @c FdWakePolicy::Cancel: the normal path would re-read the socket's members
/// through a `this` that no longer exists.
/// @param sock The socket to park on (destroyed under this flow).
/// @param cancelled Set to true if the read unwound as cancelled.
Task<void> parkThenRecordCancellation(core::net::ISocket* sock, bool* cancelled)
{
    auto buffer = std::array<std::byte, 64> {};
    try
    {
        static_cast<void>(co_await sock->read(buffer));
    }
    catch (core::async::OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// Lets the reader reach its park, then DESTROYS the socket it is parked on.
/// @param loop The loop driving the delay.
/// @param owner The owning pointer to reset.
Task<void> destroySocketAfterParked(EventLoop* loop, std::unique_ptr<core::net::ISocket>* owner)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    owner->reset(); // ~PosixSocket -> close(FdWakePolicy::Cancel)
}

/// Runs the parked reader and the socket's destruction concurrently on one loop.
Task<void> destroyWhileParked(EventLoop* loop, std::unique_ptr<core::net::ISocket>* owner, bool* cancelled)
{
    co_await core::async::whenAll(parkThenRecordCancellation(owner->get(), cancelled),
                                  destroySocketAfterParked(loop, owner));
}

/// Lets the accept reach its park, then DESTROYS the listener beneath it.
Task<void> destroyListenerAfterParked(EventLoop* loop, std::unique_ptr<core::net::IListener>* owner)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    owner->reset(); // ~PosixListener -> close(FdWakePolicy::Cancel)
}

/// Runs the parked accept and the listener's destruction concurrently on one loop.
Task<void> destroyListenerWhileParked(EventLoop* loop,
                                      std::unique_ptr<core::net::IListener>* owner,
                                      bool* accepted)
{
    co_await core::async::whenAll(parkThenObserveListenerClose(owner->get(), accepted),
                                  destroyListenerAfterParked(loop, owner));
}

/// Reads once and counts the resume, however it arrives. A count above one means
/// the same coroutine was queued twice — the use-after-free the close-wake path had
/// to be designed around.
/// @param sock The socket to park on.
/// @param count Incremented exactly once, when the read resolves or unwinds.
Task<void> countOneResume(core::net::ISocket* sock, int* count)
{
    auto buffer = std::array<std::byte, 64> {};
    try
    {
        static_cast<void>(co_await sock->read(buffer));
        ++*count;
    }
    catch (core::async::OperationCancelled const&)
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
Task<void> countOneResumeThenRequestStop(EventLoop* loop, core::net::ISocket* sock, int* count)
{
    co_await countOneResume(sock, count);
    loop->requestStop();
}

/// Accepts once and counts the resume, however it arrives.
Task<void> countOneAccept(core::net::IListener* listener, int* count)
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
                               core::net::ISocket* first,
                               core::net::ISocket* second,
                               bool* stopRequested)
{
    co_await loop->delay(std::chrono::milliseconds { 20 });
    first->close();
    second->close();
    try
    {
        co_await loop->delay(std::chrono::milliseconds { 20 });
    }
    catch (core::async::OperationCancelled const&)
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

TEST_CASE("every backend reports pipe readability", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue; // not available on this platform

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            auto observed = false;
            loop.blockOn(waitThenRead(&loop, pipe->get(), &observed));
            REQUIRE(observed);
        }
    }
}

TEST_CASE("every backend drives a socket round-trip", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto got = std::string {};
            loop.blockOn(echoOnce(pair->first.get(), pair->second.get(), &got));
            REQUIRE(got == "parity");
        }
    }
}

TEST_CASE("every backend serves a loopback listener", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());
            auto const port = (*listener)->boundPort();
            REQUIRE(port != 0);

            // accept() and connect() park on opposite readiness directions, so this
            // exercises the attach/interest-update path the reconciliation covers.
            // They must run as CONCURRENT flows: a Task is lazy, so a connect that
            // is merely created (not awaited) never runs, and accept would wait for
            // a connection nobody initiated.
            auto got = std::string {};
            loop.blockOn(echoOverListener(&loop, listener->get(), &got));
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
TEST_CASE("closing a socket resumes a parked reader on every backend", "[net][backend][parity][closehang]")
{
    // Socket_test covers this only for PollBackend, so it stayed green while the
    // native backends were broken. Driving it through the loop on every backend is
    // what catches a source that holds a descriptor the socket thinks it closed.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto resumedWithError = false;
            loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));
            CHECK(resumedWithError); // resumed at all (no hang) AND saw the close
        }
    }
}

TEST_CASE("closing a socket resumes a parked writer on every backend", "[net][backend][parity][closehang]")
{
    // The write direction of the same hazard. A writer parks on WRITABILITY, so it
    // is registered with a different interest than the reader above — and a backend
    // that only routed the read side of a close would hang here instead.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto resumedWithError = false;
            loop.blockOn(closeWhileWriteParked(&loop, pair->second.get(), &resumedWithError));
            CHECK(resumedWithError);
        }
    }
}

TEST_CASE("closing a socket resumes BOTH flows parked on it on every backend",
          "[net][backend][parity][closehang]")
{
    // Two registrations on one descriptor is the shape epoll and kqueue cannot hold
    // natively, so each keeps a private dup() for the second. A close must resume
    // both flows -- and detach both registrations, or that duplicate would keep the
    // peer's connection open past the close it was supposed to end.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto readerResumed = false;
            auto writerResumed = false;
            loop.blockOn(closeWhileBothParked(&loop, pair->second.get(), &readerResumed, &writerResumed));
            CHECK(readerResumed);
            CHECK(writerResumed);
        }
    }
}

TEST_CASE("closing a socket wakes only the flows parked on it", "[net][backend][parity][closehang]")
{
    // The wake is routed by descriptor, not broadcast: a reader on an untouched
    // socket must stay parked. A close that woke every waiter would look like it
    // worked -- until an unrelated flow resumed early and read from a live socket
    // that had nothing to give.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto closing = core::net::testing::makeSocketPair(loop);
            auto idle = core::net::testing::makeSocketPair(loop);
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

TEST_CASE("closing a socket with nothing parked leaves the loop usable", "[net][backend][parity][closehang]")
{
    // Closing an idle socket records no wake at all, and closing it twice records
    // no second one. Either mistake would leave a token queued for a registration
    // that no longer exists, and the next pump would resume whatever now answers to
    // it -- or spin delivering a wake nobody claims.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto stillPumps = false;
            loop.blockOn(closeTwiceThenKeepPumping(&loop, pair->second.get(), &stillPumps));
            CHECK(stillPumps);
        }
    }
}

TEST_CASE("a close-wake does not swallow a pending timer", "[net][backend][parity][closehang]")
{
    // The pump merges close-wakes INTO its wait outcome rather than short-circuiting
    // and returning early. Skipping the wait would strand every other thing due in
    // that pump -- a peer's EOF, or this timer -- until some later pump that may
    // never come, because blockOn exits as soon as its root flow is done.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto readerResumed = false;
            auto timerFired = false;
            loop.blockOn(closeWhileTimerPending(&loop, pair->second.get(), &readerResumed, &timerFired));
            CHECK(readerResumed);
            CHECK(timerFired);
        }
    }
}

TEST_CASE("destroying a socket under a parked reader cancels it", "[net][backend][parity][closehang]")
{
    // A destructor cannot use the same wake an explicit close() does. Resuming the
    // flow on its NORMAL path would send it back into PosixSocket::read, which
    // re-reads _closed and _fd through a `this` that has just stopped existing.
    // FdWakePolicy::Cancel makes await_resume throw instead, so the frame unwinds
    // without ever re-entering its body. Run under ASan, this case is the guard.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto cancelled = false;
            loop.blockOn(destroyWhileParked(&loop, &pair->second, &cancelled));
            CHECK(cancelled); // unwound as cancelled, not resumed into a dead object
        }
    }
}

TEST_CASE("destroying a listener under a parked accept cancels it", "[net][backend][parity][closehang]")
{
    // The listener form of the same rule, and the sharper one: acceptOne holds
    // `int const* fd` and `bool const* closed` pointing INTO the listener, and
    // re-reads both at the top of every turn. A normal-path resume would dereference
    // them after the listener was destroyed; the cancelling resume returns from the
    // catch without touching either.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto accepted = true;
            loop.blockOn(destroyListenerWhileParked(&loop, &*listener, &accepted));
            CHECK_FALSE(accepted);
        }
    }
}

TEST_CASE("a close recorded before the loop dies resumes its flow exactly once",
          "[net][backend][parity][closehang]")
{
    // close() records a wake for the next pump — but if the loop is destroyed first
    // that pump never comes. ~EventLoop must then be the ONLY thing that resumes the
    // flow, through its own request_stop()-first path. Consuming the recorded wake
    // there as well would resume the same coroutine twice: the first resume runs it
    // to completion and its owner destroys the frame, and the second calls .done()
    // on freed memory.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto resumeCount = 0;
            {
                auto loop = EventLoop { *backend };
                auto pair = core::net::testing::makeSocketPair(loop);
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
          "[net][backend][parity][closehang]")
{
    // requestDaemonShutdown's sequence, verbatim: close every listener, THEN
    // requestStop(). This is the shape that segfaulted vthost_test and
    // contour_gui_test on every platform when close() queued the parked flow itself:
    // the queued coroutine still had its cancellation callback armed, so
    // request_stop() queued it a second time.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto resumeCount = 0;
            {
                auto loop = EventLoop { *backend };
                auto listeners = std::vector<std::unique_ptr<core::net::IListener>> {};
                for ([[maybe_unused]] auto const each: std::views::iota(0, 3))
                {
                    auto listener = core::net::listen(loop, "127.0.0.1", 0);
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
          "[net][backend][parity][closehang]")
{
    // Both readers are woken by the same pump. The first to resume requests stop
    // while the second is still sitting in the ready queue, un-resumed and with its
    // cancellation callback still armed — the callback only disarms in await_resume.
    // requeueForCancellation must recognise that the sibling is already queued and
    // NOT push it again.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto resumeCount = 0;
            auto stopRequested = false;
            {
                auto loop = EventLoop { *backend };
                auto first = core::net::testing::makeSocketPair(loop);
                auto second = core::net::testing::makeSocketPair(loop);
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

TEST_CASE("a peer's close is delivered as EOF on every backend", "[net][backend][parity]")
{
    // The end-to-end form of the descriptor-ownership rule: the reader goes through
    // EventLoop and ISocket rather than touching the source directly, so a backend
    // that keeps the peer's file description alive shows up as a read that never
    // reports EOF.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            pair->first->close(); // the peer goes away

            auto outcome = -99;
            loop.blockOn(readOnce(pair->second.get(), &outcome));
            CHECK(outcome == 0); // 0 == clean EOF; -1 would be an error, -99 a hang
        }
    }
}

namespace
{

/// A registration a case owns outright, counting what the backend dispatched to it.
///
/// Every case below that talks to a backend directly goes through one of these,
/// because that is the whole shape change: a backend no longer hands back a list of
/// ready tokens for the caller to route, it CALLS what the caller registered. What a
/// case can therefore assert is which callback ran and how often — which is also what
/// the two rules worth pinning are about (one callback per registration per wait, and
/// nothing dispatched after a withdrawal).
struct Probe
{
    core::net::ReadinessHandler handler {};
    int readable = 0;
    int writable = 0;
    int failed = 0;

    /// @param handle The native handle to watch.
    explicit Probe(core::platform::NativeHandle handle) noexcept
    {
        handler = core::net::ReadinessHandler { .handle = handle,
                                                .kind = core::net::DefaultHandleKind,
                                                .owner = this,
                                                .onReadable = &Probe::readableCallback,
                                                .onWritable = &Probe::writableCallback,
                                                .onError = &Probe::errorCallback };
    }

    Probe(Probe const&) = delete;
    Probe& operator=(Probe const&) = delete;
    Probe(Probe&&) = delete;
    Probe& operator=(Probe&&) = delete;
    ~Probe() = default;

    /// @return How many callbacks of any kind this probe has had.
    [[nodiscard]] int total() const noexcept { return readable + writable + failed; }

    static void readableCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->readable;
    }

    static void writableCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->writable;
    }

    static void errorCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<Probe*>(handler.owner)->failed;
    }
};

/// Registers @p probe with @p backend and arms it, failing the case if either step is
/// refused. Both steps, because the split is the contract: `attach` says the handler
/// and the backend are usable together, and only `setInterest` says the kernel
/// accepted the registration.
/// @param backend The backend to register with.
/// @param probe The probe to register.
/// @param interest What to watch for.
void armProbe(core::net::IoBackend& backend, Probe& probe, core::net::Interest interest)
{
    REQUIRE(backend.attach(probe.handler).has_value());
    REQUIRE(backend.setInterest(probe.handler, interest).has_value());
}

/// One of a pair of registrations on their own channels, where whichever the backend
/// dispatches first detaches the other — the shape of fastcached#475 on a real kernel.
struct WithdrawingPeer
{
    std::unique_ptr<core::platform::SystemPipe> pipe;
    core::net::ReadinessHandler handler {};
    core::net::IoBackend* backend = nullptr;
    WithdrawingPeer* other = nullptr;
    bool* actedAlready = nullptr;
    int dispatched = 0;

    static void onReady(core::net::ReadinessHandler& handler) noexcept
    {
        auto* const self = static_cast<WithdrawingPeer*>(handler.owner);
        ++self->dispatched;
        if (*self->actedAlready)
            return;
        *self->actedAlready = true;
        // Exactly what a resumed coroutine dropping a socket does: detach first, which
        // is all any owner can do, and then let the object go.
        self->backend->detach(self->other->handler);
    }
};

/// Parks on @p handle and records whether a backend dispatch was in flight at the
/// instant the flow resumed. It must not be: a backend enqueues, and the loop resumes,
/// in a later step.
/// @param loop The loop to park on.
/// @param handle The handle to wait for.
/// @param insideDispatch Set to what the flow observed when it woke.
Task<void> recordDispatchStateOnResume(EventLoop* loop,
                                       core::platform::NativeHandle handle,
                                       bool* insideDispatch)
{
    co_await loop->waitReadable(handle);
    *insideDispatch = core::net::detail::readinessDispatchInFlight();
}

} // namespace

TEST_CASE("an attached handler is dispatched to, and a detached one is not", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto probe = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, probe, Interest::Read);

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            CHECK(backend->wait(std::chrono::milliseconds { 200 }).dispatched == 1);
            CHECK(probe.readable == 1);

            // After detaching, the same still-readable handle must reach nobody. The
            // bytes are deliberately left unread, so the silence below is the
            // detachment and not an idle channel.
            backend->detach(probe.handler);
            CHECK(backend->wait(core::platform::SteadyDuration::zero()).dispatched == 0);
            CHECK(probe.total() == 1);
        }
    }
}

TEST_CASE("a muted registration is dispatched to by no backend", "[net][backend][parity]")
{
    // Interest::None is public API, documented as "mute the handle without detaching
    // it". A muted registration must therefore be as silent as a detached one while
    // still being attached — and it must be silent on EVERY backend, which is what
    // this pins.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            // A byte already waiting, so the handle really IS ready: the silence below
            // is the mute and not an idle channel.
            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            auto muted = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, muted, Interest::None);

            CHECK(backend->wait(core::platform::SteadyDuration::zero()).dispatched == 0);
            CHECK(muted.total() == 0);

            // The control: the same handle, watched for Read, is dispatched to at once
            // — so the silence above is the interest and nothing else about this handle.
            auto watched = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, watched, Interest::Read);
            CHECK(backend->wait(std::chrono::milliseconds { 200 }).dispatched == 1);
            CHECK(watched.readable == 1);
            CHECK(muted.total() == 0);

            // And muting is not a one-way door: the registration that was silent arms
            // and is reached, without ever having been detached and re-attached.
            //
            // The control is detached FIRST, and that is not tidying. Whether a
            // duplicated descriptor yields one ready entry or two is the multiplexer's
            // own business -- Linux's and FreeBSD's poll(2) fill in every matching
            // pollfd, macOS's reports the descriptor once -- and IoBackend promises
            // neither. Asking about the second registration while the first is still
            // armed asks a question with no portable answer, and macOS's poll(2) is
            // where it is answered differently.
            backend->detach(watched.handler);
            REQUIRE(backend->setInterest(muted.handler, Interest::Read).has_value());
            std::ignore = backend->wait(std::chrono::milliseconds { 200 });
            CHECK(muted.readable >= 1);

            backend->detach(muted.handler);
        }
    }
}

TEST_CASE("a muted registration stays silent when its peer hangs up", "[net][backend][parity]")
{
#ifdef _WIN32
    SKIP("no portable way to hang up one end of a waitable channel: platform::SystemPipe owns both ends. The "
         "Windows backend excludes a muted registration from its wait set outright, which the portable "
         "muting case above covers");
#else
    // Where the backends actually diverged. poll(2) and epoll report HUP/ERR for a
    // registered descriptor whatever interest was asked for, so a muted descriptor
    // whose peer hung up was routed as a failure and woke the flow the caller had
    // muted — and on epoll it did so on every wait, since EPOLLHUP is level-triggered,
    // spinning the pump. Windows and kqueue reported nothing. Muting now means the
    // same thing everywhere.
    //
    // POSIX-only because there is no portable way to hang up one end of a waitable
    // channel: platform::SystemPipe owns both ends together. The Windows backend
    // excludes a muted registration from its wait set outright, which the portable
    // case above covers.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto sv = std::array<int, 2> {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

            auto muted = Probe { sv[0] };
            armProbe(*backend, muted, Interest::None);

            ::close(sv[1]); // the peer hangs up: POLLHUP / EPOLLHUP on sv[0]

            CHECK(backend->wait(core::platform::SteadyDuration::zero()).dispatched == 0);
            CHECK(muted.total() == 0);

            backend->detach(muted.handler);
            ::close(sv[0]);
        }
    }
#endif
}

TEST_CASE("a hangup over buffered data wakes the reader on every backend", "[net][backend][parity]")
{
#ifdef _WIN32
    SKIP("needs socketpair(2) and a peer that can be closed independently; SystemPipe owns both ends on "
         "Windows");
#else
    // Ruling R101's property, and the one nothing asserted before it. A peer that
    // writes and then closes leaves a socket that is BOTH readable and hung up:
    // POLLIN|POLLHUP on poll and epoll, EVFILT_READ carrying EV_EOF with data on
    // kqueue. Whatever a backend calls that, the reader must be woken, because those
    // bytes are still there and a read() is the only thing that will collect them.
    //
    // The probe sets onError, which is the configuration B6 will use and the one the
    // defect needed: selectReadinessCallback returns exactly ONE callback, so under the
    // old failure-first order poll and epoll answered onError here and the reader was
    // never woken at all. kqueue and Wfmo answered onReadable and were always right.
    //
    // Deliberately NOT asserted: whether `failed` was set. That is where the backends
    // legitimately disagree — EV_EOF on a read filter is an ordinary shutdown(WR), not
    // an error — and Readiness::Failed is documented best-effort for exactly this
    // reason. Asserting it would pin a divergence instead of the property.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto sv = std::array<int, 2> {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

            auto reader = Probe { sv[0] };
            armProbe(*backend, reader, Interest::Read);

            REQUIRE(::write(sv[1], "x", 1) == 1); // data first...
            ::close(sv[1]);                       // ...then the hangup, arriving together

            CHECK(backend->wait(std::chrono::milliseconds { 200 }).dispatched >= 1);
            CHECK(reader.readable >= 1);

            backend->detach(reader.handler);
            ::close(sv[0]);
        }
    }
#endif
}

TEST_CASE("a hangup with nothing buffered still wakes somebody", "[net][backend][parity]")
{
#ifdef _WIN32
    SKIP("needs socketpair(2) and a peer that can be closed independently; SystemPipe owns both ends on "
         "Windows");
#else
    // The other half, and the limit of what is portable. With no data to collect, the
    // backends differ in what they report -- poll and epoll set POLLHUP with no POLLIN,
    // kqueue delivers a readable EV_EOF -- so which CALLBACK fires is a divergence and
    // this case does not assert it. What every backend must do is wake the flow, or a
    // reader parked on a closed peer waits for ever while a level-triggered
    // registration re-reports the hangup on every wait: a loop at 100% CPU telling
    // nobody.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto sv = std::array<int, 2> {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

            auto reader = Probe { sv[0] };
            armProbe(*backend, reader, Interest::Read);

            ::close(sv[1]);

            CHECK(backend->wait(std::chrono::milliseconds { 200 }).dispatched >= 1);
            CHECK(reader.total() >= 1);

            backend->detach(reader.handler);
            ::close(sv[0]);
        }
    }
#endif
}

TEST_CASE("two registrations on one handle are accepted by every backend", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            // The interface permits it — the loop parks a reader beside a writer on one
            // socket — and poll(2) takes two entries, so the native backends must too:
            // an epoll set is keyed by descriptor and would otherwise refuse the second
            // with EEXIST, and a kqueue filter is keyed by (descriptor, filter) and
            // would replace rather than add.
            auto first = Probe { (*pipe)->waitHandle() };
            auto second = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, first, Interest::Read);
            armProbe(*backend, second, Interest::Read);

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            // At least one registration is dispatched to. NOT both: whether a
            // duplicated handle yields one ready entry or two is the multiplexer's own
            // business — Linux's poll(2) fills in every matching pollfd, macOS's
            // reports the descriptor once — and IoBackend deliberately does not promise
            // either. What it does promise is that a registration is accepted and that
            // readiness reaches somebody.
            std::ignore = backend->wait(std::chrono::milliseconds { 200 });
            CHECK((first.readable > 0 || second.readable > 0));

            // Detaching one must not disturb the other: they are separate kernel
            // registrations, so dropping one cannot take the survivor's with it. This
            // is the property the private dup() exists to provide, and it holds
            // everywhere regardless of how duplicates are reported above.
            backend->detach(first.handler);
            auto const firstBefore = first.total();
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());
            std::ignore = backend->wait(std::chrono::milliseconds { 200 });
            CHECK(second.readable > 0);
            CHECK(first.total() == firstBefore);

            backend->detach(second.handler);
        }
    }
}

TEST_CASE("a registration does not keep a closed descriptor's connection alive", "[net][backend][parity]")
{
#ifdef _WIN32
    SKIP("asks about a raw descriptor closed behind the backend's back, which has no Winsock equivalent");
#else
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto sv = std::array<int, 2> {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

            // Attach one end, then close it WITHOUT detaching first — the ordering a
            // cancelled flow or an early socket destructor produces. A backend that
            // registers a dup() of the descriptor keeps the underlying open file
            // description alive, so no FIN reaches the peer: its read blocks forever
            // instead of reporting EOF, and the connection leaks.
            auto probe = Probe { sv[0] };
            armProbe(*backend, probe, Interest::Read);
            ::close(sv[0]);

            // The peer must see EOF now. Read non-blocking so a backend that holds the
            // description open fails the assertion instead of hanging the suite.
            auto const flags = ::fcntl(sv[1], F_GETFL, 0);
            ::fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);
            auto buffer = std::array<char, 16> {};
            auto const got = ::read(sv[1], buffer.data(), buffer.size());
            CHECK(got == 0); // 0 == EOF; -1/EAGAIN means the FIN never arrived

            backend->detach(probe.handler);
            ::close(sv[1]);
        }
    }
#endif
}

TEST_CASE("setInterest reports the kernel's refusal when descriptors run out", "[net][backend][parity]")
{
#ifdef _WIN32
    SKIP("needs RLIMIT_NOFILE to make the kernel refuse a registration; Windows has no equivalent knob");
#else
    // fastcached#1054 and #1057, from the side a caller feels. `attach` answers that
    // the handler and the backend are usable together; whether the KERNEL accepted the
    // registration is what `setInterest` answers, and only that — which is why it
    // returns an expected rather than a bare success. A registration the caller
    // believes succeeded and the kernel never made parks a flow with nothing left to
    // resume it: no message, no stack, just a hang.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            // The first registration watches the caller's descriptor directly, so it
            // needs no descriptor of its own.
            auto first = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, first, Interest::Read);

            // A second registration on the same descriptor needs a private dup(). Lower
            // the soft descriptor limit to what is already open so that dup() must fail.
            auto limit = rlimit {};
            REQUIRE(::getrlimit(RLIMIT_NOFILE, &limit) == 0);
            auto const originalSoft = limit.rlim_cur;

            auto underPressure = Probe { (*pipe)->waitHandle() };
            auto armed = std::expected<void, core::net::NetError> {};
            {
                auto const spare = ::dup(0); // the lowest descriptor still free
                REQUIRE(spare >= 0);
                auto squeezed = limit;
                squeezed.rlim_cur = static_cast<rlim_t>(spare); // no descriptor >= spare may be opened
                REQUIRE(::setrlimit(RLIMIT_NOFILE, &squeezed) == 0);
                ::close(spare);

                // The soft limit is PROCESS-wide, so it has to come back on EVERY exit
                // from this scope, not only the one that falls through: a throw (a
                // failed Catch assertion is one) used to skip a plain restore statement
                // and leave every later case in this binary running squeezed — a cascade
                // of failures whose cause appears nowhere in their own output.
                auto const restoreLimit = core::net::detail::ScopeGuard { [originalSoft]() noexcept {
                    auto restored = rlimit {};
                    if (::getrlimit(RLIMIT_NOFILE, &restored) != 0)
                        return;
                    restored.rlim_cur = originalSoft;
                    static_cast<void>(::setrlimit(RLIMIT_NOFILE, &restored));
                } };

                REQUIRE(backend->attach(underPressure.handler).has_value());
                armed = backend->setInterest(underPressure.handler, Interest::Read);
            }
            // Descriptors are available again from here: the guard restored the limit.

            // What must hold on EVERY backend is that the answer is honest: either the
            // interest was refused, or it was genuinely armed. What must never happen is
            // a success for a registration the kernel does not have. poll(2) needs no
            // descriptor of its own, so it legitimately succeeds here; epoll and kqueue
            // must dup() and so must refuse — and their refusal carries the kernel's own
            // errno, which is the whole reason it is an error value and not a bare false.
            if (entry.kind == BackendKind::Poll)
                CHECK(armed.has_value());
            else
            {
                REQUIRE_FALSE(armed.has_value());
                CHECK(armed.error().code == core::net::NetErrorCode::SystemError);
                CHECK(armed.error().systemCode != 0);
                CHECK_FALSE(armed.error().context.empty());
            }

            // Recovery: with descriptors available again, a duplicate must work.
            auto afterRecovery = Probe { (*pipe)->waitHandle() };
            armProbe(*backend, afterRecovery, Interest::Read);

            backend->detach(afterRecovery.handler);
            backend->detach(underPressure.handler);
            backend->detach(first.handler);
        }
    }
#endif
}

TEST_CASE("an invalid handle is refused by every backend", "[net][backend][parity]")
{
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            // A refused registration must SAY so: the awaiting flow has to fail rather
            // than park on an interest the kernel never accepted, which nothing could
            // resume.
            auto probe = Probe { core::platform::InvalidHandle };
            auto const attached = backend->attach(probe.handler);
            REQUIRE_FALSE(attached.has_value());
            CHECK(attached.error().code == core::net::NetErrorCode::BadHandle);

            // And an interest on a handler that was never attached is refused too,
            // rather than silently recorded against nothing.
            auto stray = Probe { core::platform::InvalidHandle };
            CHECK_FALSE(backend->setInterest(stray.handler, Interest::Read).has_value());
        }
    }
}

TEST_CASE("a backend dispatches, and the loop resumes", "[net][backend][parity]")
{
    // Rule 1, from the side that proves it end to end. A backend's callbacks only
    // enqueue; the coroutine is resumed later, on the loop thread, out of the ready
    // queue. Resuming from inside the backend's walk over its own ready list would let
    // the resumed frame free the object whose entry the walk has not reached yet — a
    // use-after-free with no diagnostic. EventLoop::drainReadyQueue asserts the
    // negative on every path; this asserts it positively, on every backend, from the
    // flow's own frame.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());

            auto insideDispatch = true;
            loop.blockOn(recordDispatchStateOnResume(&loop, (*pipe)->waitHandle(), &insideDispatch));
            CHECK_FALSE(insideDispatch);
        }
    }
}

TEST_CASE("a handler detached from inside a dispatch is not dispatched in the same wait",
          "[net][backend][parity]")
{
    // fastcached#475 on a real kernel. Dropping a registration stops FUTURE reports and
    // does nothing about an entry the wait has already written into the batch being
    // walked, so a callback that detaches another handler and frees its owner leaves a
    // dangling entry the same walk reads. Without the withdrawal this is a
    // use-after-free rather than a failed assertion — it reports as a crash under a
    // sanitizer and can pass silently without one, which is the nature of the defect
    // and is why the case exists.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            // The CONTROL, and it is the whole difference between a case and a
            // tautology. Everything below rests on one wait dequeuing both peers into a
            // single batch; if a kernel reported only one of them, `dispatched == 1` and
            // `peers[0] + peers[1] == 1` would BOTH still hold and the withdrawal would
            // never be exercised. The case would go green having tested nothing, and
            // silently. So the premise is asserted first, on two throwaway pipes with no
            // withdrawal in them: this kernel does report two ready descriptors in one
            // wait. This task has already been bitten once by assuming how many entries
            // one wait fills.
            {
                auto controlPipes = std::array<std::unique_ptr<core::platform::SystemPipe>, 2> {};
                auto controls = std::vector<std::unique_ptr<Probe>> {};
                for (auto& slot: controlPipes)
                {
                    auto pipe = core::platform::createSystemPipe();
                    REQUIRE(pipe.has_value());
                    slot = std::move(*pipe);
                    auto probe = std::make_unique<Probe>(slot->waitHandle());
                    armProbe(*backend, *probe, Interest::Read);
                    controls.push_back(std::move(probe));
                }
                auto const byte = std::array<std::byte, 1> { std::byte { 'x' } };
                for (auto& slot: controlPipes)
                    REQUIRE(slot->write(byte.data(), byte.size()).has_value());

                REQUIRE(backend->wait(std::chrono::milliseconds { 200 }).dispatched == 2);

                for (auto const& probe: controls)
                    backend->detach(probe->handler);
            }

            auto peers = std::array<WithdrawingPeer, 2> {};
            auto actedAlready = false;
            for (auto& peer: peers)
            {
                auto pipe = core::platform::createSystemPipe();
                REQUIRE(pipe.has_value());
                peer.pipe = std::move(*pipe);
                peer.backend = backend.get();
                peer.actedAlready = &actedAlready;
                peer.handler = core::net::ReadinessHandler { .handle = peer.pipe->waitHandle(),
                                                             .kind = core::net::DefaultHandleKind,
                                                             .owner = &peer,
                                                             .onReadable = &WithdrawingPeer::onReady,
                                                             .onWritable = nullptr,
                                                             .onError = &WithdrawingPeer::onReady };
                REQUIRE(backend->attach(peer.handler).has_value());
                REQUIRE(backend->setInterest(peer.handler, Interest::Read).has_value());
            }
            peers[0].other = std::next(peers.data());
            peers[1].other = peers.data();

            // Both readable BEFORE the wait, so ONE wait dequeues both in a single
            // batch. Without this the case proves nothing — it would be two batches and
            // the window would never open.
            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            for (auto& peer: peers)
                REQUIRE(peer.pipe->write(one.data(), one.size()).has_value());

            auto const dispatched = backend->wait(std::chrono::milliseconds { 200 });

            // Exactly one of them acted, and the other was withdrawn from inside the
            // batch rather than dispatched. Whichever the kernel reported first is the
            // one that acted, so the case does not depend on that order.
            REQUIRE(actedAlready);
            CHECK(dispatched.dispatched == 1);
            CHECK(peers[0].dispatched + peers[1].dispatched == 1);

            backend->detach(peers[0].handler);
            backend->detach(peers[1].handler);
        }
    }
}

TEST_CASE("a wake ends a wait, and one raised before the wait is not lost", "[net][backend][parity]")
{
    // wake() is the one member of IoBackend another thread may call, and a lost wakeup
    // is a loop that never returns from its wait — a hang at shutdown and nowhere else.
    // Both halves are asserted: a wake raised while nothing is waiting must make the
    // NEXT wait return, and a wake raised during a wait must end it.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            // Bounded, and it says what it waits for: reaching the timeout means the
            // wake was lost, not that the machine was slow.
            constexpr auto NeverInPractice = std::chrono::seconds { 30 };
            constexpr auto SlowMachine = std::chrono::seconds { 5 };

            auto const beforeFirst = std::chrono::steady_clock::now();
            backend->wake();
            std::ignore = backend->wait(NeverInPractice);
            CHECK(std::chrono::steady_clock::now() - beforeFirst < SlowMachine);

            auto const beforeSecond = std::chrono::steady_clock::now();
            auto waker = std::thread { [&backend] {
                std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
                backend->wake();
            } };
            std::ignore = backend->wait(NeverInPractice);
            waker.join();
            CHECK(std::chrono::steady_clock::now() - beforeSecond < SlowMachine);
        }
    }
}

TEST_CASE("a dead handle in the wait set does not blind a backend to a live one",
          "[net][backend][parity][closehang]")
{
    // A handle closed while still registered is reported differently by every kernel —
    // poll(2) answers POLLNVAL, epoll drops it from the set, kqueue drops its filters,
    // and Windows fails the whole wait on it. What must be the same everywhere is that
    // the dead one does not take a LIVE registration down with it. On Windows it did:
    // WaitForMultipleObjects failed on the dead handle every round, so the wait reported
    // nothing at all and whoever was parked on the live handle hung forever. (Resuming
    // the flow parked on the dead handle is a different question, and not one a
    // readiness poller can answer — it is what EventLoop::notifyHandleClosing exists
    // for.)
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto dying = core::platform::createSystemPipe();
            auto live = core::platform::createSystemPipe();
            REQUIRE(dying.has_value());
            REQUIRE(live.has_value());

            auto dead = Probe { (*dying)->waitHandle() };
            auto alive = Probe { (*live)->waitHandle() };
            armProbe(*backend, dead, Interest::Read);
            armProbe(*backend, alive, Interest::Read);

            dying->reset(); // closed, and deliberately NOT detached

            auto const one = std::array<std::byte, 1> { std::byte { 'x' } };
            REQUIRE((*live)->write(one.data(), one.size()).has_value());

            std::ignore = backend->wait(std::chrono::milliseconds { 200 });
            CHECK(alive.readable > 0);

            backend->detach(dead.handler);
            backend->detach(alive.handler);
        }
    }
}

TEST_CASE("the default backend drives the scenarios Socket_test pins to one backend",
          "[net][backend][parity][closehang]")
{
    // Socket_test hardcoded a single backend in all of its cases, which is why two
    // native-backend defects (a registration holding the peer's connection open, and a
    // parked reader never resuming after close) passed a green suite. These run the same
    // shapes against whatever makeDefaultBackend picks — epoll on Linux, kqueue on
    // macOS/BSD, IOCP on Windows — so the backend production actually uses is exercised.
    auto const backend = core::net::makeDefaultBackend();
    REQUIRE(backend != nullptr);

    SECTION("loopback echo")
    {
        auto loop = EventLoop { *backend };
        auto listener = core::net::listen(loop, "127.0.0.1", 0);
        REQUIRE(listener.has_value());
        auto const port = (*listener)->boundPort();
        REQUIRE(port != 0);

        auto got = std::string {};
        loop.blockOn(echoOverListener(&loop, listener->get(), &got));
        CHECK(got == "parity");
    }

    SECTION("close resumes a parked reader")
    {
        auto loop = EventLoop { *backend };
        auto pair = core::net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());

        auto resumedWithError = false;
        loop.blockOn(closeWhileParked(&loop, pair->second.get(), &resumedWithError));
        CHECK(resumedWithError);
    }

    SECTION("a peer's close reads as EOF")
    {
        auto loop = EventLoop { *backend };
        auto pair = core::net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());

        pair->first->close();

        auto outcome = -99;
        loop.blockOn(readOnce(pair->second.get(), &outcome));
        CHECK(outcome == 0);
    }
}

// Same hazard as the parked reader, one layer up; it HUNG on epoll/kqueue until
// notifyHandleClosing existed.
TEST_CASE("closing a listener resumes a parked accept on every backend", "[net][backend][parity][closehang]")
{
    // Same hazard as a parked reader, one layer up: accept() parks on waitReadable, so
    // a listener closed while an accept is pending must resume it rather than leave it
    // parked on a descriptor the poller can no longer report.
    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto loop = EventLoop { *backend };
            auto listener = core::net::listen(loop, "127.0.0.1", 0);
            REQUIRE(listener.has_value());

            auto accepted = true;
            loop.blockOn(acceptThenClose(&loop, listener->get(), &accepted));
            CHECK_FALSE(accepted); // it resumed at all, and reported the close
        }
    }
}

#ifdef _WIN32
namespace
{

/// This process's console input, held for the length of one case.
///
/// `CONIN$` names the console the process is attached to whatever its standard handles were
/// redirected to — a test runner hands a test pipes. A process attached to no console cannot open
/// it, and the case SKIPs rather than passing.
///
/// A console is shared by every process attached to it, and a runner that gives each case its own
/// process runs them side by side on ONE console: this case writes a record into the input buffer,
/// and another case's flush would discard it. So the handle is taken under a named mutex — the
/// SAME name `src/core/tui/windows/TerminalInput_test.cpp` uses, deliberately, because the
/// resource is the process's console and not this binary's: two suites serialising on two
/// different names would not serialise at all.
class ConsoleInput
{
  public:
    ConsoleInput():
        _lock { CreateMutexW(nullptr, FALSE, L"Local\\core-cpp-tui-console-input-test") },
        _locked { _lock != nullptr && isAcquired(WaitForSingleObject(_lock, LockBoundMs)) },
        _handle { CreateFileW(L"CONIN$",
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr) }
    {
    }

    ~ConsoleInput()
    {
        if (_handle != INVALID_HANDLE_VALUE)
            CloseHandle(_handle);
        if (_locked)
            ReleaseMutex(_lock);
        if (_lock != nullptr)
            CloseHandle(_lock);
    }

    ConsoleInput(ConsoleInput const&) = delete;
    ConsoleInput& operator=(ConsoleInput const&) = delete;
    ConsoleInput(ConsoleInput&&) = delete;
    ConsoleInput& operator=(ConsoleInput&&) = delete;

    /// @return Whether this case holds the console for itself.
    [[nodiscard]] bool locked() const noexcept { return _locked; }

    /// @return Whether this process has a console to hold at all.
    [[nodiscard]] bool available() const noexcept { return _handle != INVALID_HANDLE_VALUE; }

    /// @return The console input handle.
    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

  private:
    /// How long a case waits for another to finish with the console. Each is well under a second,
    /// so this bound is for a wedged holder rather than for a queue.
    static constexpr DWORD LockBoundMs = 30000;

    /// An abandoned mutex — its holder died mid-case — is still acquired, and the flush each case
    /// opens with is what makes whatever that holder left harmless.
    [[nodiscard]] static bool isAcquired(DWORD waited) noexcept
    {
        return waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }

    HANDLE _lock;
    bool _locked;
    HANDLE _handle;
};

/// One console-input registration and the callbacks it counts.
struct ConsoleProbe
{
    core::net::ReadinessHandler handler {};
    int readable = 0;
    int other = 0;

    /// @param handle The console input handle to watch.
    explicit ConsoleProbe(HANDLE handle) noexcept
    {
        handler = core::net::ReadinessHandler { .handle = handle,
                                                .kind = core::net::HandleKind::Waitable,
                                                .owner = this,
                                                .onReadable = &ConsoleProbe::readableCallback,
                                                .onWritable = &ConsoleProbe::otherCallback,
                                                .onError = &ConsoleProbe::otherCallback };
    }

    ConsoleProbe(ConsoleProbe const&) = delete;
    ConsoleProbe& operator=(ConsoleProbe const&) = delete;
    ConsoleProbe(ConsoleProbe&&) = delete;
    ConsoleProbe& operator=(ConsoleProbe&&) = delete;
    ~ConsoleProbe() = default;

    /// @return How many callbacks of any kind this probe has had.
    [[nodiscard]] int total() const noexcept { return readable + other; }

    static void readableCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<ConsoleProbe*>(handler.owner)->readable;
    }

    static void otherCallback(core::net::ReadinessHandler& handler) noexcept
    {
        ++static_cast<ConsoleProbe*>(handler.owner)->other;
    }
};

/// A key-down record, which is what a keystroke puts in the input buffer.
/// @return The record to write.
[[nodiscard]] INPUT_RECORD keyRecord() noexcept
{
    auto record = INPUT_RECORD {};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = 'A';
    record.Event.KeyEvent.uChar.UnicodeChar = L'A';
    return record;
}

} // namespace

TEST_CASE("every backend reports console input readiness", "[net][backend][parity][windows]")
{
    // **This is the case Task B7 exists for.** A completion port cannot wait on a console handle;
    // fastcached kept a whole second coroutine runtime because of it, and contour's
    // `WaitForMultipleObjects` could do it but caps at 64 handles and cannot express a completion.
    // One backend serving both a server's sockets and a TUI's console input is the merge, and this
    // is where it is checked — on every Windows backend, so the property is the interface's rather
    // than IOCP's.
    //
    // Windows-only and it has no portable sibling: a console input handle is a waitable object,
    // which POSIX has no analogue of at all (there stdin is a descriptor like any other and the
    // parity cases over pipes already cover it).
    auto const console = ConsoleInput {};
    if (!console.locked())
        SKIP("could not take the console lock within its bound: another suite is wedged holding it");
    if (!console.available())
        SKIP("this process is attached to no console, so CONIN$ cannot be opened and no console "
             "input handle exists to register");

    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend)
            continue; // not built on this platform

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            // Whatever the console already holds — keys typed at it, a record another case left —
            // is not this case's input, and it would make the registration signalled before
            // anything was written.
            REQUIRE(FlushConsoleInputBuffer(console.get()) != 0);

            auto probe = ConsoleProbe { console.get() };
            REQUIRE(backend->attach(probe.handler).has_value());
            REQUIRE(backend->setInterest(probe.handler, core::net::Interest::Read).has_value());

            // An empty input buffer is not readable, and a backend that reported every
            // registration on every wait would pass the assertion below for the wrong reason.
            CHECK(backend->wait(std::chrono::milliseconds { 20 }).dispatched == 0);
            CHECK(probe.total() == 0);

            auto record = keyRecord();
            auto written = DWORD { 0 };
            REQUIRE(WriteConsoleInputW(console.get(), &record, 1, &written) != 0);
            REQUIRE(written == 1);

            // Bounded, and the bound is what it waits FOR: one dispatch of a readiness that has
            // already been raised, which is microseconds. Two seconds is four orders of magnitude
            // of slack on a cold runner, and a regression exhausts it and fails rather than
            // hanging the binary.
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };
            while (probe.total() == 0 && std::chrono::steady_clock::now() < deadline)
                std::ignore = backend->wait(std::chrono::milliseconds { 20 });

            CHECK(probe.readable >= 1);
            CHECK(probe.other == 0);

            backend->detach(probe.handler);
            std::ignore = FlushConsoleInputBuffer(console.get());
        }
    }
}
#endif

TEST_CASE("makeDefaultBackend yields a usable backend", "[net][backend]")
{
    auto const backend = core::net::makeDefaultBackend();
    REQUIRE(backend != nullptr);

    // Whatever it picked must drive a real round-trip.
    auto loop = EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());

    auto got = std::string {};
    loop.blockOn(echoOnce(pair->first.get(), pair->second.get(), &got));
    REQUIRE(got == "parity");
}

TEST_CASE("the preferred backend is constructible on this platform, and names itself", "[net][backend]")
{
    // If the platform names a native backend, it must actually build here — a silent
    // permanent fallback would mean the port is not exercised at all.
    auto const preferred = core::net::preferredBackendKind();
    auto const backend = core::net::makeBackend(preferred);
    REQUIRE(backend != nullptr);
    CHECK(backend->kind() == preferred);
    CHECK_FALSE(backend->isHostDriven());

    // And the default IS the preferred one wherever it can be built, which is what
    // makes "the parity matrix covers what production runs" true rather than hoped.
    auto const byDefault = core::net::makeDefaultBackend();
    REQUIRE(byDefault != nullptr);
    CHECK(byDefault->kind() == preferred);
}

TEST_CASE("this platform builds every backend the parity matrix expects of it", "[net][backend]")
{
    // What a green parity run does NOT otherwise tell you. The 29 cases that loop
    // BackendMatrix skip a kind this platform does not build -- `if (!backend)
    // continue;` -- which is right, because the matrix names every kind that exists
    // anywhere. But it means a platform that STOPPED building one would run all 29
    // against a smaller matrix, touch nothing it used to, and stay green. A vacuous
    // run and a real one are indistinguishable from the outside.
    //
    // That is not a general worry here, it is the specific evidence Ruling R101 rests
    // on. R101 says a watched direction beats onError BECAUSE poll and epoll were
    // wrong and kqueue was right; a macOS run that exercised only poll would confirm
    // nothing about that claim and would look exactly like one that confirmed it.
    //
    // `the preferred backend is constructible on this platform` already REQUIREs ONE
    // non-null entry, which keeps the 29 from being vacuous outright. "At least one"
    // is not the question. This case asks which.
    //
    // Named, never counted: a threshold weakens silently when nobody bumps it, while a
    // named kind that stops building fails here and says so.
    struct Expected
    {
        BackendKind kind;
        std::string_view why;
    };

#ifdef _WIN32
    // One row since 0.5.0: the completion port became the default in Task B7b, and the WFMO
    // backend it kept as a fallback for one release was then removed (core-cpp#6).
    auto const expected = std::array<Expected, 1> { {
        { BackendKind::Iocp,
          "the completion port, Windows' only backend, and the one that can serve a console handle "
          "and a socket from one wait -- a run without it covers neither bridge" },
    } };
#elifdef __linux__
    auto const expected = std::array<Expected, 2> { {
        { BackendKind::Poll, "poll(2), the portable fallback every POSIX platform builds" },
        { BackendKind::Epoll, "epoll(7), Linux's own" },
    } };
#else
    auto const expected = std::array<Expected, 2> { {
        { BackendKind::Poll, "poll(2), the portable fallback every POSIX platform builds" },
        { BackendKind::Kqueue,
          "kqueue(2) -- the backend Ruling R101 says is RIGHT where poll and "
          "epoll were wrong, so a run without it cannot confirm R101" },
    } };
#endif

    for (auto const& entry: expected)
    {
        INFO("expected backend: " << core::net::toString(entry.kind) << " -- " << entry.why);
        INFO("If this fails, the set of backends this platform builds has CHANGED. That is not "
             "necessarily a defect -- Task B7 adding IOCP will change the Windows row on purpose -- "
             "but until this table is updated, every case that loops BackendMatrix is covering less "
             "than its name claims, silently. Update the table here, then check what the parity "
             "cases still exercise.");
        CHECK(core::net::makeBackend(entry.kind) != nullptr);
    }

    // And the matrix really does offer them: a kind built here but missing from
    // BackendMatrix would be a backend no parity case ever reaches.
    for (auto const& entry: expected)
    {
        INFO("built but absent from BackendMatrix: " << core::net::toString(entry.kind));
        CHECK(std::ranges::any_of(core::net::testing::BackendMatrix,
                                  [&entry](auto const& row) { return row.kind == entry.kind; }));
    }
}

TEST_CASE("makeBackend answers null for a kind this platform does not build", "[net][backend]")
{
    // A kind that is genuinely absent here, named rather than computed, so this asks a
    // real question on each platform instead of trivially passing everywhere.
#ifdef _WIN32
    CHECK(core::net::makeBackend(BackendKind::Epoll) == nullptr);
    CHECK(core::net::makeBackend(BackendKind::Kqueue) == nullptr);
    CHECK(core::net::makeBackend(BackendKind::Poll) == nullptr);
#elifdef __linux__
    CHECK(core::net::makeBackend(BackendKind::Kqueue) == nullptr);
#else
    CHECK(core::net::makeBackend(BackendKind::Epoll) == nullptr);
#endif

    // IOCP arrived with Task B7a, on Windows and nowhere else. Asked in both directions
    // on purpose: "not built here" and "built here" are the same green when the kind is
    // only ever checked against null on the platforms that never had it.
#ifdef _WIN32
    CHECK(core::net::makeBackend(BackendKind::Iocp) != nullptr);
#else
    CHECK(core::net::makeBackend(BackendKind::Iocp) == nullptr);
#endif

    // The test doubles and the host-driven backend are reachable, and not through here:
    // a production factory is the wrong way to get a test double, and HostDrivenBackend
    // needs the IHostScheduler its host provides.
    CHECK(core::net::makeBackend(BackendKind::Scripted) == nullptr);
    CHECK(core::net::makeBackend(BackendKind::Null) == nullptr);
    CHECK(core::net::makeBackend(BackendKind::HostDriven) == nullptr);
}

namespace
{

/// A handler whose callbacks must never run: a refused registration has nothing to report.
/// @param handler Unused.
void neverCalled(core::net::ReadinessHandler& /*handler*/) noexcept
{
}

/// @param backend The backend to ask.
/// @param handle A real, open handle, so a refusal cannot be the handle's fault.
/// @return What the backend answered to a `HandleKind::Completion` registration of it.
std::expected<void, core::net::NetError> attachAsCompletion(core::net::IoBackend& backend,
                                                            core::platform::NativeHandle handle)
{
    auto handler = core::net::ReadinessHandler { .handle = handle,
                                                 .kind = core::net::HandleKind::Completion,
                                                 .owner = nullptr,
                                                 .onReadable = &neverCalled,
                                                 .onWritable = &neverCalled,
                                                 .onError = nullptr };
    auto answer = backend.attach(handler);
    if (answer)
        backend.detach(handler); // never reached on a passing run; keeps a failing one tidy
    return answer;
}

} // namespace

TEST_CASE("every backend without a completion port refuses a completion registration by name",
          "[net][backend][parity]")
{
    // `HandleKind::Completion`'s handle is the ADDRESS of an overlapped operation. A backend that
    // took it would hand that address to poll(2), epoll, kqueue or WaitForMultipleObjects as though
    // it were a descriptor or a kernel object. "Nothing there can build one" is an argument, not a
    // guarantee, so each backend that lends no port says Unsupported, and this case holds all of
    // them to it -- the matrix, plus the host-driven backend, which the matrix does not build and
    // which is the one such backend Windows has since the WFMO backend went (core-cpp#6).
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    auto const handle = (*pipe)->waitHandle();

    for (auto const& entry: BackendMatrix)
    {
        auto const backend = core::net::makeBackend(entry.kind);
        if (!backend || backend->completionPort() != nullptr)
            continue; // not built here, or the one kind that serves it
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto const answer = attachAsCompletion(*backend, handle);
            REQUIRE_FALSE(answer.has_value());
            CHECK(answer.error().code == core::net::NetErrorCode::Unsupported);
        }
    }

    SECTION("backend=host-driven")
    {
        auto host = core::net::testing::ManualHostScheduler {};
        auto clock = core::platform::ManualClock {};
        auto backend = core::net::HostDrivenBackend { host, clock };
        auto const answer = attachAsCompletion(backend, handle);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().code == core::net::NetErrorCode::Unsupported);
    }
}
