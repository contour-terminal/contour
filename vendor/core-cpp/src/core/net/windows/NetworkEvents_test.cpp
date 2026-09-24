// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in),
// so this block leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/NetworkEvents.hpp>
#include <core/platform/WinsockInit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{

/// @return A new TCP socket, Winsock initialised first, or `InvalidSocket`.
SOCKET openTcpSocket() noexcept
{
    core::platform::ensureWinsockInitialized();
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

/// A listening loopback socket with an FD_ACCEPT event, as the readiness listener built one
/// until 0.5.0 -- raw, because the race this is about lives between two Winsock calls, and
/// `NetworkEvents.cpp`, which it covers, still serves `IocpBackend`'s socket-writability bridge. Closes both
/// handles on destruction.
class AcceptFixture
{
  public:
    AcceptFixture(): _socket(openTcpSocket())
    {
        if (_socket == core::net::detail::InvalidSocket)
            return;

        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_port = 0; // an ephemeral port
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::bind(_socket, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0
            || ::listen(_socket, 8) != 0)
            return;

        auto bound = sockaddr_in {};
        auto boundLength = int { sizeof(bound) };
        if (::getsockname(_socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0)
            return;
        _port = ::ntohs(bound.sin_port);

        _event = WSACreateEvent();
        if (_event == WSA_INVALID_EVENT || WSAEventSelect(_socket, _event, FD_ACCEPT) == SOCKET_ERROR)
            _event = WSA_INVALID_EVENT;
    }

    ~AcceptFixture()
    {
        if (_client != core::net::detail::InvalidSocket)
            ::closesocket(_client);
        if (_event != WSA_INVALID_EVENT)
            WSACloseEvent(_event);
        if (_socket != core::net::detail::InvalidSocket)
            ::closesocket(_socket);
    }

    AcceptFixture(AcceptFixture const&) = delete;
    AcceptFixture& operator=(AcceptFixture const&) = delete;
    AcceptFixture(AcceptFixture&&) = delete;
    AcceptFixture& operator=(AcceptFixture&&) = delete;

    /// @return True if the listening socket and its event were both set up.
    [[nodiscard]] bool ready() const noexcept
    {
        return _socket != core::net::detail::InvalidSocket && _event != WSA_INVALID_EVENT;
    }

    [[nodiscard]] SOCKET listening() const noexcept { return _socket; }
    [[nodiscard]] WSAEVENT event() const noexcept { return _event; }

    /// Connects a client to the listening socket (blocking, loopback).
    /// @return True once connected.
    [[nodiscard]] bool connectClient()
    {
        _client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_client == core::net::detail::InvalidSocket)
            return false;
        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(_port);
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        return ::connect(_client, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0;
    }

  private:
    SOCKET _socket = core::net::detail::InvalidSocket;
    SOCKET _client = core::net::detail::InvalidSocket;
    WSAEVENT _event = WSA_INVALID_EVENT;
    std::uint16_t _port = 0;
};

/// Drives the fixture to the state the accept race ends in: a failing ::accept (nothing
/// pending yet), then a connection that lands before the caller has armed its park, so
/// Winsock has RECORDED FD_ACCEPT and signalled the event.
/// @param fixture The fixture to drive.
/// @return True once the event is signalled with a connection pending behind it.
[[nodiscard]] bool reachRecordedAcceptState(AcceptFixture& fixture)
{
    if (::accept(fixture.listening(), nullptr, nullptr) != core::net::detail::InvalidSocket)
        return false; // nothing may be pending yet, or this is not the race
    if (WSAGetLastError() != WSAEWOULDBLOCK)
        return false;
    if (!fixture.connectClient())
        return false;
    return WaitForSingleObject(fixture.event(), 2000) == WAIT_OBJECT_0;
}

} // namespace

TEST_CASE("consuming an accept indication keeps the pending connection", "[net][windows]")
{
    // WindowsListener::accept used to call WSAResetEvent before parking. A client connecting
    // between the ::accept that returned WSAEWOULDBLOCK and that reset leaves FD_ACCEPT
    // RECORDED and the event signalled; the reset then clears the event while the record
    // stands, and Winsock raises a recorded indication only once — so the park never woke and
    // the listener went silent, for that connection and every one after it.
    auto fixture = AcceptFixture {};
    REQUIRE(fixture.ready());
    REQUIRE(reachRecordedAcceptState(fixture));

    // What accept() now does before parking: it SEES the indication and retries instead.
    auto const indications = core::net::consumeNetworkEvents(fixture.listening(), fixture.event());
    CHECK((indications & FD_ACCEPT) != 0);

    auto const accepted = ::accept(fixture.listening(), nullptr, nullptr);
    CHECK(accepted != core::net::detail::InvalidSocket); // the connection the retry serves
    if (accepted != core::net::detail::InvalidSocket)
        ::closesocket(accepted);
}

TEST_CASE("resetting the event instead loses the accept wake-up", "[net][windows]")
{
    // The control arm: the same state, handled the old way. This is why the listener must not
    // reach for WSAResetEvent — the event goes dark although a connection IS pending, and
    // nothing will signal it again until accept() runs, which is precisely what the parked
    // coroutine was waiting to be told it could do.
    auto fixture = AcceptFixture {};
    REQUIRE(fixture.ready());
    REQUIRE(reachRecordedAcceptState(fixture));

    WSAResetEvent(fixture.event());
    CHECK(WaitForSingleObject(fixture.event(), 250) == WAIT_TIMEOUT);

    // And the connection really was there all along: the park would have been for ever.
    auto const accepted = ::accept(fixture.listening(), nullptr, nullptr);
    CHECK(accepted != core::net::detail::InvalidSocket);
    if (accepted != core::net::detail::InvalidSocket)
        ::closesocket(accepted);
}
