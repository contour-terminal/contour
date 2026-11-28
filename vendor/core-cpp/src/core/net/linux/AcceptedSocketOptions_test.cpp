// SPDX-License-Identifier: Apache-2.0
//
// What a Linux listener's accepted connections inherit from it: every `SocketOption` set on the
// listening socket. Linux copies the listener's socket (`sk_clone_lock`) into each one it accepts,
// so this is a kernel property this library relies on, not one it implements.
//
// Linux only, by its source list. macOS does not carry the TCP keepalive intervals over to an
// accepted socket, and wasmtime re-applies some options to accepted sockets by hand for that reason;
// whether the WASI host must do the same is a P14 question.
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITcpSocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

using core::async::Task;
using core::net::AddressFamily;
using core::net::IpSocketAddress;
using core::net::SocketOption;
using core::net::SocketResult;

namespace
{

/// One option and the value the listener is given; none is a Linux default.
struct Inherited
{
    SocketOption option;
    std::uint64_t value;
};

constexpr auto InheritedOptions = std::array {
    Inherited { .option = SocketOption::ReceiveBufferSize, .value = 48'000 },
    Inherited { .option = SocketOption::SendBufferSize, .value = 40'000 },
    Inherited { .option = SocketOption::UnicastHopLimit, .value = 17 },
    Inherited { .option = SocketOption::KeepAliveEnabled, .value = 1 },
    Inherited { .option = SocketOption::KeepAliveIdleTime, .value = 43 },
    Inherited { .option = SocketOption::KeepAliveInterval, .value = 13 },
    Inherited { .option = SocketOption::KeepAliveCount, .value = 4 },
};

/// Connects @p client to @p remote and accepts it on @p listener.
Task<void> connectAndAccept(core::net::ITcpSocket* client,
                            core::net::ResolvedEndpoint remote,
                            core::net::IListener* listener,
                            SocketResult* connected,
                            SocketResult* accepted)
{
    *connected = co_await client->connect(remote);
    if (connected->has_value())
        *accepted = co_await listener->accept();
}

} // namespace

TEST_CASE("A Linux listener's accepted connections inherit its socket options",
          "[net][tcp-socket][socket-option][loopback]")
{
    auto const family = GENERATE(AddressFamily::Ipv4, AddressFamily::Ipv6);
    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const loopback =
        family == AddressFamily::Ipv6
            ? IpSocketAddress { .address = core::net::Ipv6Address { { 0, 0, 0, 0, 0, 0, 0, 1 } } }
            : IpSocketAddress { .address = core::net::Ipv4Address { { 127, 0, 0, 1 } } };

    auto server = core::net::openTcpSocket(loop, family);
    REQUIRE(server.has_value());
    auto const bound = (*server)->bind(core::net::toEndpoint(loopback));
    if (!bound.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6 loopback: " << bound.error().toString());
    REQUIRE(bound.has_value());
    for (auto const& [option, value]: InheritedOptions)
        REQUIRE((*server)->setOption(option, value).has_value());
    auto listener = (*server)->listen(4);
    REQUIRE(listener.has_value());

    auto client = core::net::openTcpSocket(loop, family);
    REQUIRE(client.has_value());
    auto remote = loopback;
    remote.port = (*listener)->boundPort();
    auto connected = SocketResult {};
    auto accepted = SocketResult {};
    loop.blockOn(core::net::testing::anyOf(
        connectAndAccept(
            client->get(), core::net::toEndpoint(remote), listener->get(), &connected, &accepted),
        core::net::testing::sleepFor(&loop, std::chrono::seconds { 5 })));
    REQUIRE(accepted.has_value());

    for (auto const& [option, value]: InheritedOptions)
    {
        INFO("option " << static_cast<int>(option));
        CHECK((*accepted)->option(option) == value);
    }
}
