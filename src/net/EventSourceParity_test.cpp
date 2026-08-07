// SPDX-License-Identifier: Apache-2.0
///
/// Every @c EventSource backend must be behaviourally interchangeable — the whole
/// point of the interface is that swapping poll(2) for epoll/kqueue changes only
/// what a wait costs. These cases therefore run the SAME scenario against every
/// backend available on this platform, so a divergence fails here rather than
/// surfacing as a hang in whatever happens to use the native source.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>
#include <net/DefaultEventSource.hpp>
#include <net/EventLoop.hpp>
#include <net/EventSource.hpp>
#include <net/ISocket.hpp>
#include <net/Sockets.hpp>
#include <net/platform/SystemPipe.hpp>
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

namespace
{

/// One backend to exercise, with a label so a failure names which one broke.
struct Backend
{
    EventSourceKind kind;  ///< The backend to construct.
    std::string_view name; ///< Its name, for the section label.
};

constexpr auto AllBackends = std::array {
    Backend { EventSourceKind::Poll, "poll" },
    Backend { EventSourceKind::Epoll, "epoll" },
    Backend { EventSourceKind::Kqueue, "kqueue" },
};

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

TEST_CASE("closing a socket resumes a parked reader on every event source", "[net][eventsource][parity]")
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

TEST_CASE("the default source drives the scenarios Socket_test pins to poll", "[net][eventsource][parity]")
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

TEST_CASE("closing a listener resumes a parked accept on every event source", "[net][eventsource][parity]")
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
