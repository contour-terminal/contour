// SPDX-License-Identifier: Apache-2.0
///
/// The `ConnectEx` dial (`detail::dialCompletion`): the completion-model counterpart of
/// `ReadinessDial_test`, with the two rules that are the opposite of the readiness dial's
/// (`task-B7b-handoff-from-B8.md`): the outcome is the completion's STATUS, and a deadline or a
/// stop CANCELS the operation and lets the completion report.
///
/// **Every case here dials a closed loopback port, and that is an arrangement, not a
/// convenience.** Windows does not refuse such a connect at once: it retries the SYN for about two
/// seconds before `ConnectEx` completes with WSAECONNREFUSED. So the dial is genuinely outstanding
/// for long enough to be cancelled, timed out or observed, with no listener to saturate -- and the
/// refusal case pays those two seconds on purpose, because it is the one that must reach the end.
///
/// Imported in spirit from fastcached `Net/IocpConnector_test.cpp` at `0708dd54`.

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/KeepAlive.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/windows/IocpBackend.hpp>
#include <core/net/windows/IocpSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

using core::async::Task;
using core::net::EventLoop;
using core::net::IocpBackend;
using core::net::KeepAlive;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::SocketResult;
using core::net::detail::dialCompletion;
using core::net::detail::StreamSocketOptions;
using core::platform::SteadyTimePoint;

namespace
{

/// @param port A loopback port.
/// @return Its resolved endpoint.
///
/// **Called into a named local before the `co_await`, never inside its full-expression**: with the
/// call inside it, MSVC 14.51 at /O2 cannot emit the tail call a symmetric transfer requires and
/// reports C4737, which /WX makes fatal (the same shape `ReadinessDial_test.cpp` documents).
ResolvedEndpoint loopbackEndpoint(std::uint16_t port)
{
    auto resolver = core::net::SystemAddressResolver {};
    auto resolved = resolver.resolve("127.0.0.1", port);
    REQUIRE(resolved.has_value());
    REQUIRE_FALSE(resolved->empty());
    return resolved->front();
}

/// @param loop The loop to bind on.
/// @return A loopback port that WAS listening and no longer is.
std::uint16_t closedPort(EventLoop& loop)
{
    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE(bound.has_value());
    auto const port = (*bound)->boundPort();
    REQUIRE(port != 0);
    return port;
}

/// Dials and reports: the answer, or that the flow unwound.
Task<void> dialReporting(
    EventLoop* loop, std::uint16_t port, SteadyTimePoint deadline, SocketResult* out, bool* threw, bool* done)
{
    try
    {
        auto const endpoint = loopbackEndpoint(port);
        *out = co_await dialCompletion(loop, endpoint, deadline, StreamSocketOptions {});
    }
    catch (core::async::OperationCancelled const&)
    {
        *threw = true;
    }
    *done = true;
}

/// Turns @p loop until @p done or a generous bound; the refused dial alone takes two seconds.
[[nodiscard]] bool pumpUntil(EventLoop& loop, bool const& done)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (!done && std::chrono::steady_clock::now() < deadline)
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
    return done;
}

} // namespace

TEST_CASE("A ConnectEx dial connects, and its socket is usable for everything a connected one is",
          "[net][iocp][dial]")
{
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    auto dialled = SocketResult {};
    auto served = std::string {};
    auto client = [](EventLoop* lp, std::uint16_t port, SocketResult* out) -> Task<void> {
        auto const endpoint = loopbackEndpoint(port);
        *out = co_await dialCompletion(
            lp, endpoint, SteadyTimePoint::max(), StreamSocketOptions { .keepAlive = KeepAlive::Yes });
        if (!out->has_value())
            co_return;
        constexpr auto Payload = std::string_view { "hello" };
        std::ignore = co_await (**out)->write(std::span<std::byte const> {
            reinterpret_cast<std::byte const*>(Payload.data()), Payload.size() });
        // SO_UPDATE_CONNECT_CONTEXT is what makes this reach the peer: without it `shutdown`
        // fails on a ConnectEx socket with WSAENOTCONN, and the server below never sees EOF.
        std::ignore = co_await (**out)->shutdownWrite();
    };
    auto server = [](core::net::IListener* from, std::string* into) -> Task<void> {
        auto accepted = co_await from->accept();
        if (!accepted.has_value())
            co_return;
        auto buffer = std::array<std::byte, 64> {};
        while (true)
        {
            auto const got = co_await (*accepted)->read(std::span<std::byte> { buffer });
            if (!got.has_value() || *got == 0)
                co_return; // EOF: the dialled side's shutdownWrite arrived
            into->append(reinterpret_cast<char const*>(buffer.data()), *got);
        }
    };
    loop.blockOn(core::net::testing::allOf(server(listener.get(), &served),
                                           client(&loop, listener->boundPort(), &dialled)));

    REQUIRE(dialled.has_value());
    CHECK(dynamic_cast<core::net::IocpSocket*>(dialled->get()) != nullptr);
    CHECK((*dialled)->peerAddress() == "127.0.0.1");
    CHECK(served == "hello");
}

TEST_CASE("A refused ConnectEx is reported by its completion as ConnRefused", "[net][iocp][dial]")
{
    // The IOCP leg B8's hand-off asked for, and the equivalent of the readiness dial saying which
    // path a refusal took: the port held the ConnectEx while the dial was outstanding, so the
    // refusal came back as that operation's completion status -- never through SO_ERROR.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto const port = closedPort(loop);

    auto answer = SocketResult {};
    auto threw = false;
    auto done = false;
    auto held = std::size_t { 0 };
    loop.spawn(dialReporting(&loop, port, SteadyTimePoint::max(), &answer, &threw, &done));
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        held = std::max(held, backend.issuedOperations());
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
    }

    REQUIRE(done);
    REQUIRE_FALSE(threw);
    CHECK(held == 1);
    REQUIRE_FALSE(answer.has_value());
    INFO(answer.error().context);
    CHECK(answer.error().code == NetErrorCode::ConnRefused);
    CHECK(answer.error().systemCode == WSAECONNREFUSED);
    CHECK(backend.issuedOperations() == 0);
}

TEST_CASE("A ConnectEx dial's deadline cancels it, and the completion reports Timeout", "[net][iocp][dial]")
{
    // The completion is the single writer: the deadline asks for the ConnectEx back and the abort
    // that follows is what settles the dial. Settling at the deadline instead would leave the
    // real completion to arrive later into a wait that is gone.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto const port = closedPort(loop);

    auto answer = SocketResult {};
    auto threw = false;
    auto done = false;
    auto const started = std::chrono::steady_clock::now();
    loop.spawn(dialReporting(
        &loop, port, loop.clock().now() + std::chrono::milliseconds { 100 }, &answer, &threw, &done));
    REQUIRE(pumpUntil(loop, done));
    auto const took = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(threw);
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code == NetErrorCode::Timeout);
    // Well inside the two seconds the refusal would have taken: it was the deadline that ended it.
    CHECK(took < std::chrono::milliseconds { 1500 });
    // And the operation came home: nothing is left in the kernel naming it.
    CHECK(backend.issuedOperations() == 0);
}

TEST_CASE("A stop cancels a ConnectEx dial in flight, by every route", "[net][iocp][dial][stop]")
{
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto const port = closedPort(loop);

    auto answer = SocketResult {};
    auto threw = false;
    auto done = false;

    SECTION("as a whenAny loser, which is the only route that reaches the dial's own stop callback")
    {
        // requestStop also unparks everything, so it proves nothing about the dial's own
        // cancellation; a loser is stopped through the token and nothing else.
        auto held = std::size_t { 0 };
        auto winner = [](EventLoop* lp, IocpBackend* port, std::size_t* observed) -> Task<void> {
            co_await lp->delay(std::chrono::milliseconds { 100 });
            *observed = port->issuedOperations();
        };
        loop.blockOn(core::net::testing::anyOf(
            dialReporting(&loop, port, SteadyTimePoint::max(), &answer, &threw, &done),
            winner(&loop, &backend, &held)));
        REQUIRE(pumpUntil(loop, done));
        // It was genuinely parked on the ConnectEx when the stop arrived.
        CHECK(held == 1);
    }
    SECTION("through the loop's root stop")
    {
        auto stopAfter = [](EventLoop* lp) -> Task<void> {
            co_await lp->delay(std::chrono::milliseconds { 100 });
            lp->requestStop();
        };
        loop.blockOn(core::net::testing::allOf(
            dialReporting(&loop, port, SteadyTimePoint::max(), &answer, &threw, &done), stopAfter(&loop)));
    }
    SECTION("from ANOTHER thread")
    {
        auto stopper = std::thread { [&loop] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
            loop.rootStopSource().request_stop();
        } };
        auto const join = core::net::detail::ScopeGuard { [&stopper]() noexcept {
            if (stopper.joinable())
                stopper.join();
        } };
        loop.blockOn(dialReporting(&loop, port, SteadyTimePoint::max(), &answer, &threw, &done));
    }

    // A cancel from the FLOW unwinds, as the readiness dial's does -- and it unwinds only once the
    // abort came home, so nothing is left in the kernel.
    CHECK(threw);
    CHECK(backend.issuedOperations() == 0);
}
