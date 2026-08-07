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

            // Both registrations must observe the readiness, not just one.
            auto const ready = source->wait(200);
            CHECK(std::ranges::find(ready.readyRead, first) != ready.readyRead.end());
            CHECK(std::ranges::find(ready.readyRead, second) != ready.readyRead.end());

            // Detaching one must not disturb the other: they are separate kernel
            // registrations, so dropping one cannot take the survivor's with it.
            source->detach(first);
            REQUIRE((*pipe)->write(one.data(), one.size()).has_value());
            auto const afterOne = source->wait(200);
            CHECK(std::ranges::find(afterOne.readyRead, second) != afterOne.readyRead.end());
            CHECK(std::ranges::find(afterOne.readyRead, first) == afterOne.readyRead.end());

            source->detach(second);
        }
    }
}

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
