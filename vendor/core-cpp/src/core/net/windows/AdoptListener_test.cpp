// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/platform/WinsockInit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

using core::async::Task;
using core::net::EventLoop;

namespace
{

/// Binds and listens on loopback WITHOUT going through core::net — the Windows counterpart of
/// `posix/AdoptListener_test.cpp`, and for the same reason: what `adoptListener` is for is a
/// socket this library did not make.
///
/// Deliberately BLOCKING, because that is what such a socket is; `IocpListener::adopt` is what
/// associates it with the loop's completion port.
struct RawListener
{
    SOCKET socket = core::net::detail::InvalidSocket;
    std::uint16_t port = 0;
};

[[nodiscard]] RawListener bindRaw()
{
    core::platform::ensureWinsockInitialized();

    auto raw = RawListener {};
    raw.socket = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(raw.socket != core::net::detail::InvalidSocket);

    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = htons(0); // an ephemeral port, so the case asserts the kernel's answer
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);

    REQUIRE(::bind(raw.socket, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(raw.socket, 8) == 0);

    auto bound = sockaddr_in {};
    auto length = int { sizeof(bound) };
    REQUIRE(::getsockname(raw.socket, reinterpret_cast<sockaddr*>(&bound), &length) == 0);
    raw.port = ntohs(bound.sin_port);
    REQUIRE(raw.port != 0);
    return raw;
}

/// Accepts one connection on @p listener and reads @p expected from it.
Task<void> acceptAndRead(core::net::IListener* listener, std::string_view expected, bool* ok)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto buffer = std::array<std::byte, 64> {};
    auto const read = co_await (*accepted)->read(std::span<std::byte> { buffer }.first(expected.size()));
    *ok = read.has_value() && *read == expected.size()
          && std::string_view { reinterpret_cast<char const*>(buffer.data()), expected.size() } == expected;
}

/// Dials @p port through the public connect and writes @p payload.
Task<void> dialAndWrite(EventLoop* loop, std::uint16_t port, std::string_view payload, bool* ok)
{
    auto dialled = co_await core::net::connect(loop, "127.0.0.1", port);
    if (!dialled.has_value())
        co_return;
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(payload.data()), payload.size() };
    auto const written = co_await (*dialled)->write(bytes);
    *ok = written.has_value() && *written == payload.size();
}

} // namespace

TEST_CASE("an adopted listener accepts, and reports the port it was already bound to", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto const raw = bindRaw();
    auto adopted = core::net::adoptListener(loop, reinterpret_cast<core::platform::NativeHandle>(raw.socket));
    REQUIRE(adopted.has_value());
    auto listener = std::move(*adopted);

    // The port is the KERNEL's answer, not one this library was told.
    CHECK(listener->boundPort() == raw.port);

    auto served = false;
    auto wrote = false;
    loop.blockOn(core::net::testing::allOf(acceptAndRead(listener.get(), "hello", &served),
                                           dialAndWrite(&loop, raw.port, "hello", &wrote)));
    CHECK(wrote);
    CHECK(served);
    // The socket went WITH the listener, which is what took over closing it.
    listener->close();
}

TEST_CASE("adopting a handle that is not a socket is refused rather than accepted", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto const refused = core::net::adoptListener(loop, core::platform::InvalidHandle);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == core::net::NetErrorCode::BadHandle);
}
