// SPDX-License-Identifier: Apache-2.0
//
// `ListenOptions::sharing` on POSIX: `PortSharing::Shared` sets SO_REUSEPORT, so one listener per
// loop can bind the same port. Windows refuses the request instead; its case is
// windows/PortSharing_test.cpp, and the default's refusal of a second bind is Socket_test.cpp's,
// on every backend.

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

using core::async::Task;
using core::net::EventLoop;

namespace
{

constexpr auto Request = std::string_view { "hello" };

/// Accepts one connection on @p listener and echoes one read back.
Task<void> echoOnce(core::net::IListener* listener, bool* served)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto buffer = std::array<std::byte, 16> {};
    auto const got = co_await (*accepted)->read(buffer);
    if (!got.has_value() || *got == 0)
        co_return;
    auto const echoed = co_await (*accepted)->write(std::span<std::byte const> { buffer }.subspan(0, *got));
    *served = echoed.has_value() && *echoed == *got;
}

/// Connects to @p listener's port, sends @ref Request and checks the echo. Closes @p listener on a
/// failed connect, so the accept beside it is not left parked.
Task<void> askOnce(EventLoop* loop, core::net::IListener* listener, bool* matched)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", listener->boundPort());
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(Request.data()), Request.size() };
    auto const sent = co_await (*connected)->write(bytes);
    if (!sent.has_value())
        co_return;
    auto buffer = std::array<std::byte, 16> {};
    auto total = std::size_t { 0 };
    while (total < Request.size())
    {
        auto const got = co_await (*connected)->read(std::span<std::byte> { buffer }.subspan(total));
        if (!got.has_value() || *got == 0)
            break;
        total += *got;
    }
    *matched = total == Request.size() && std::memcmp(buffer.data(), Request.data(), Request.size()) == 0;
}

/// Both ends of one echo.
Task<void> echoRoundTrip(EventLoop* loop, core::net::IListener* listener, bool* served, bool* matched)
{
    co_await core::async::whenAll(echoOnce(listener, served), askOnce(loop, listener, matched));
}

/// Sets @p expired once @p limit has passed on @p loop.
Task<void> expireAfter(EventLoop* loop, std::chrono::milliseconds limit, bool* expired)
{
    co_await loop->delay(limit);
    *expired = true;
}

/// Runs one echo against @p listener on @p loop, within a budget.
///
/// Bounded because the failure this guards is a connection the kernel handed to a DIFFERENT
/// listener on the same port: the echo's accept then parks with nothing coming, and an unbounded
/// wait turns that red into a hang (.agent/rules/testing.md).
/// @return Whether the echo completed, served and matched, inside the budget.
bool echoWithin(EventLoop& loop, core::net::IListener* listener)
{
    constexpr auto Budget = std::chrono::milliseconds { 10000 };
    auto served = false;
    auto matched = false;
    auto timedOut = false;
    loop.blockOn(core::net::testing::anyOf(echoRoundTrip(&loop, listener, &served, &matched),
                                           expireAfter(&loop, Budget, &timedOut)));
    INFO("waited " << Budget.count() << "ms for one echo on port " << listener->boundPort());
    CHECK_FALSE(timedOut);
    return !timedOut && served && matched;
}

} // namespace

TEST_CASE("PortSharing::Shared lets a listener per loop bind one port, and each accepts", "[net][listen]")
{
    // A daemon binds one listener per loop on the same port and lets the kernel spread the
    // connections (SO_REUSEPORT). Which listener a connection reaches is the kernel's choice -- a
    // hash on Linux, the newest listener on macOS -- so the case does not race them. The first
    // accepts while it is alone; the second binds while the first still holds the port, which is
    // the property; and once the first is closed, the second accepts.
    auto firstBackend = core::net::makeDefaultBackend();
    auto secondBackend = core::net::makeDefaultBackend();
    auto firstLoop = EventLoop { *firstBackend };
    auto secondLoop = EventLoop { *secondBackend };
    auto const shared = [](std::uint16_t port) {
        return core::net::ListenOptions { .host = "127.0.0.1",
                                          .port = port,
                                          .sharing = core::net::PortSharing::Shared };
    };

    auto first = core::net::listen(firstLoop, shared(0));
    REQUIRE(first.has_value());
    auto const port = (*first)->boundPort();
    REQUIRE(echoWithin(firstLoop, first->get()));

    auto second = core::net::listen(secondLoop, shared(port));
    REQUIRE(second.has_value());
    CHECK((*second)->boundPort() == port);

    (*first)->close();
    CHECK(echoWithin(secondLoop, second->get()));
}
