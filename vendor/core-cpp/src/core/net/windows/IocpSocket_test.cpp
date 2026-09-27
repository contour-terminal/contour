// SPDX-License-Identifier: Apache-2.0
///
/// `IocpSocket` and `IocpListener`: the completion-model socket's own cases.
///
/// What every socket shares is elsewhere and already reaches this one: since Task B7b the Windows
/// default backend is the completion port, and `testing::makeSocketPair`, `listen` and `connect`
/// hand out the socket the loop's backend drives -- so `Socket_test`, `WaitReadable_test`,
/// `AcceptedHalfClose_test`, `HttpServer_test` and the rest run over `IocpSocket` on the IOCP leg
/// of `BackendMatrix`. These are the cases only a
/// socket whose KERNEL performs the operation can have:
///
/// - **the operation outlives the socket**
/// ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)):
///   a receive, an accept or a gathered send still in the kernel when its owner is destroyed;
/// - **bytes already received win**
/// ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)),
///   over a `cancelRead` and over a stop of the awaiting flow alike;
/// - **an NTSTATUS is converted, not tabled**: an abort and a reset each reach the caller as the
///   code it can branch on, with the Winsock number behind it;
/// - **the receive deadline cancels and lets the completion report.**
///
/// Imported from fastcached `Net/IocpSocket_test.cpp` at `0708dd54`, rewritten onto an
/// `EventLoop`: upstream issued each operation in the VERB, so an unawaited `Read()` was already
/// in the kernel. Here an operation is issued when it is AWAITED (`ResultAwaitable`'s arm), so
/// every case parks a real flow instead -- a `DetachedTask`, which starts eagerly and so is parked
/// by the time the next line runs.

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <core/net/detail/ReadyBatch.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/net/windows/IocpBackend.hpp>
#include <core/net/windows/IocpSocket.hpp>
#include <core/net/windows/WindowsLoopback.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/WinsockInit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using core::async::DetachedTask;
using core::async::Task;
using core::net::EventLoop;
using core::net::IocpBackend;
using core::net::IocpListener;
using core::net::IocpSocket;
using core::net::NetErrorCode;

namespace
{

/// `INVALID_SOCKET` as a `SOCKET`; see `IocpBackend.cpp`.
constexpr SOCKET InvalidSocketValue = INVALID_SOCKET;

/// How long a case drives the loop for something it has already provoked: a completion that is
/// queued or about to be. Microseconds of work; the budget is for a cold runner under a sanitizer,
/// and running out of it FAILS the case rather than hanging it (`.agent/rules/testing.md`).
constexpr auto CompletionBudget = std::chrono::seconds { 5 };

/// Turns the loop until @p done holds or the budget runs out.
/// @param loop The loop to drive.
/// @param done What to wait for; says in its caller's `REQUIRE` what that was.
/// @return Whether @p done held.
[[nodiscard]] bool pumpUntil(EventLoop& loop, std::function<bool()> const& done)
{
    auto const deadline = std::chrono::steady_clock::now() + CompletionBudget;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
    }
    return true;
}

/// Waits on the port until it holds no operation an owner issued -- every completion, including
/// the aborts a teardown produced, has been dequeued and its node given back.
///
/// **The BACKEND is waited on, not the loop, and that is not a shortcut.** An operation whose
/// owner has gone has no park left, and a loop with nothing parked skips its wait (turn step 4:
/// nothing could come back from it) -- so such a packet sits in the port until the next wait that
/// something else causes, or until `~IocpBackend` drains it. Both are correct; neither is
/// observable from a case. Waiting on the backend directly is the dequeue those paths would do.
/// @param loop The loop, which must not be inside a turn.
/// @param backend Its backend.
/// @return Whether the port drained within the budget.
[[nodiscard]] bool drainOwnerOperations(EventLoop& loop, IocpBackend& backend)
{
    auto const deadline = std::chrono::steady_clock::now() + CompletionBudget;
    while (backend.issuedOperations() != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::ignore = backend.wait(std::chrono::milliseconds { 10 });
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    }
    return backend.issuedOperations() == 0;
}

/// A connected loopback pair made with plain Winsock, so nothing else is under test. Either end
/// can be surrendered to a socket under test, which then owns it.
class RawPair
{
  public:
    RawPair() noexcept
    {
        core::platform::ensureWinsockInitialized();
        if (!core::net::makeLoopbackPair(_sockets))
            _sockets = { InvalidSocketValue, InvalidSocketValue };
    }

    RawPair(RawPair const&) = delete;
    RawPair& operator=(RawPair const&) = delete;
    RawPair(RawPair&&) = delete;
    RawPair& operator=(RawPair&&) = delete;

    ~RawPair()
    {
        for (auto const socket: _sockets)
            if (socket != InvalidSocketValue)
                ::closesocket(socket);
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return _sockets[0] != InvalidSocketValue && _sockets[1] != InvalidSocketValue;
    }

    /// @return The accepted end, which this pair no longer closes.
    [[nodiscard]] SOCKET surrenderServer() noexcept { return std::exchange(_sockets[0], InvalidSocketValue); }

    /// @return The client end, still owned by this pair.
    [[nodiscard]] SOCKET client() const noexcept { return _sockets[1]; }

    /// How @c closeClient ends the connection.
    enum class Close : std::uint8_t
    {
        Graceful, ///< A FIN: the peer reads EOF.
        Reset,    ///< `SO_LINGER` zero: an RST, which the peer reads as a reset.
    };

    /// Closes the client end now.
    /// @param how Whether the peer sees EOF or a reset.
    void closeClient(Close how)
    {
        if (how == Close::Reset)
        {
            auto const abortive = ::linger { .l_onoff = 1, .l_linger = 0 };
            REQUIRE(::setsockopt(_sockets[1],
                                 SOL_SOCKET,
                                 SO_LINGER,
                                 reinterpret_cast<char const*>(&abortive),
                                 static_cast<int>(sizeof(abortive)))
                    == 0);
        }
        REQUIRE(::closesocket(_sockets[1]) == 0);
        _sockets[1] = InvalidSocketValue;
    }

    /// Sends @p text from the client end, all of it.
    void send(std::string_view text) const
    {
        REQUIRE(::send(_sockets[1], text.data(), static_cast<int>(text.size()), 0)
                == static_cast<int>(text.size()));
    }

  private:
    std::array<SOCKET, 2> _sockets {};
};

/// A loop over a real completion port, and the socket under test on one end of a raw pair.
struct Harness
{
    IocpBackend backend;
    EventLoop loop { backend };
    RawPair pair;
    std::unique_ptr<IocpSocket> socket;

    Harness()
    {
        REQUIRE(pair.valid());
        socket = std::make_unique<IocpSocket>(loop, pair.surrenderServer());
    }

    Harness(Harness const&) = delete;
    Harness& operator=(Harness const&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    /// The socket goes before the loop, and every packet it left behind is drained before the
    /// backend closes the port: the objects a loop drives die before it.
    ~Harness()
    {
        socket.reset();
        std::ignore = drainOwnerOperations(loop, backend);
    }
};

/// How a parked operation ended, or that it never did.
///
/// `code` is disengaged until the flow resumes AND on the success path, so no assertion about it
/// can pass against a flow that never resumed -- which is exactly what several cases are about.
struct Outcome
{
    bool resumed = false;
    bool abandoned = false; ///< The flow unwound through `OperationCancelled`.
    std::optional<NetErrorCode> code;
    int systemCode = 0;
    std::size_t bytes = 0;

    /// Records @p result.
    void record(core::net::IoResult const& result)
    {
        resumed = true;
        if (result.has_value())
            bytes = *result;
        else
        {
            code = result.error().code;
            systemCode = result.error().systemCode;
        }
    }
};

/// Parks a flow on a real `read` and records how it ends. A `DetachedTask` starts at once, so the
/// read is armed -- and its `WSARecv` in the kernel -- by the time this returns.
DetachedTask parkOnRead(core::net::ISocket* socket, std::array<std::byte, 64>* buffer, Outcome* out)
{
    try
    {
        out->record(co_await socket->read(std::span<std::byte> { *buffer }));
    }
    catch (core::async::OperationCancelled const&)
    {
        out->resumed = true;
        out->abandoned = true;
    }
}

/// Parks a flow on `waitReadable` and records how it ends.
DetachedTask parkOnProbe(core::net::ISocket* socket, Outcome* out)
{
    try
    {
        out->record(co_await socket->waitReadable());
    }
    catch (core::async::OperationCancelled const&)
    {
        out->resumed = true;
        out->abandoned = true;
    }
}

/// The client end of the round trip: connects, sends, reads the echo. Asserts nothing, because a
/// Catch assertion belongs to the case's thread; it reports and the case asserts after `join`.
/// Connects a plain blocking client to a listener on the loopback address.
/// @param port The listener's port.
/// @return The connected socket, which the caller closes, or `INVALID_SOCKET`.
SOCKET connectTo(std::uint16_t port)
{
    auto const client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == InvalidSocketValue)
        return InvalidSocketValue;
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(client, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
    {
        ::closesocket(client);
        return InvalidSocketValue;
    }
    return client;
}

/// @param port The listener's port.
/// @param response Where the echo goes.
/// @return Whether it connected.
bool echoClient(std::uint16_t port, std::string* response)
{
    auto const client = connectTo(port);
    if (client == InvalidSocketValue)
        return false;
    constexpr auto Message = std::string_view { "ping!" };
    std::ignore = ::send(client, Message.data(), static_cast<int>(Message.size()), 0);
    auto buffer = std::array<char, 64> {};
    if (auto const got = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0); got > 0)
        response->assign(buffer.data(), static_cast<std::size_t>(got));
    ::closesocket(client);
    return true;
}

/// Accepts one connection, echoes one read back, and records the peer it reported.
Task<void> echoOnce(core::net::IListener* listener, std::string* peer, bool* accepted)
{
    auto connection = co_await listener->accept();
    if (!connection.has_value())
        co_return;
    *accepted = true;
    *peer = (*connection)->peerAddress();
    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await (*connection)->read(std::span<std::byte> { buffer });
    if (got.has_value() && *got > 0)
        std::ignore = co_await (*connection)->write(std::span<std::byte const> { buffer.data(), *got });
    (*connection)->close();
}

/// Accepts once and records the answer.
DetachedTask acceptInto(core::net::IListener* listener, std::optional<core::net::AcceptResult>* out)
{
    *out = co_await listener->accept();
}

/// Accepts once and records the answer, or that the flow was stopped: a `Task`, for `anyOf`.
Task<void> acceptReporting(core::net::IListener* listener,
                           std::optional<core::net::AcceptResult>* out,
                           bool* abandoned)
{
    try
    {
        *out = co_await listener->accept();
    }
    catch (core::async::OperationCancelled const&)
    {
        *abandoned = true;
    }
}

/// A spawned root that owns a listener and keeps an accept of its own parked on it, until
/// teardown destroys the root and the listener with it.
Task<void> holdListener(std::unique_ptr<IocpListener> listener)
{
    std::ignore = co_await listener->accept();
}

/// The byte the pipe is filled with, which the payload under test never is at a filler offset.
constexpr auto FillerByte = 'f';

/// Fills the pipe from the socket under test to its peer until the kernel says it would block.
///
/// The socket sends what the send buffer takes WITHOUT an operation, and Windows buffers a great
/// deal for a non-blocking send -- measured: all 8MiB, at once. So a case about the OVERLAPPED
/// send fills the pipe first, and only then issues the write under test: it cannot take the fast
/// path and has to become an operation.
/// @param harness The socket under test and its peer, whose buffers this shrinks.
/// @return How many filler bytes are now queued for the peer, or 0 if the pipe never filled.
std::size_t fillThePipe(Harness& harness)
{
    auto const small = 4096;
    ::setsockopt(harness.socket->native(),
                 SOL_SOCKET,
                 SO_SNDBUF,
                 reinterpret_cast<char const*>(&small),
                 sizeof(small));
    ::setsockopt(
        harness.pair.client(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&small), sizeof(small));

    auto const filler = std::vector<char>(std::size_t { 64 } * 1024, FillerByte);
    auto queued = std::size_t { 0 };
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 100'000))
    {
        auto const sent = ::send(harness.socket->native(), filler.data(), static_cast<int>(filler.size()), 0);
        if (sent == SOCKET_ERROR)
            return ::WSAGetLastError() == WSAEWOULDBLOCK ? queued : 0;
        queued += static_cast<std::size_t>(sent);
    }
    return 0;
}

/// Reads up to @p count bytes from a blocking @p socket, each `recv` bounded by the completion
/// budget. Asserts nothing: it runs on a peer thread, and a Catch assertion belongs to the case's.
/// @return What arrived, which is short of @p count only if the peer stopped sending or the budget
///         ran out.
std::vector<char> receiveExactly(SOCKET socket, std::size_t count)
{
    auto const timeout =
        static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(CompletionBudget).count());
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeout), sizeof(timeout));
    auto received = std::vector<char>(count);
    auto got = std::size_t { 0 };
    while (got < count)
    {
        auto const room = std::span<char> { received }.subspan(got);
        auto const n =
            ::recv(socket, room.data(), static_cast<int>(std::min(room.size(), std::size_t { 65536 })), 0);
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    received.resize(got);
    return received;
}

/// The payload byte at @p offset. The period, 251, is prime and so shares no factor with any size
/// the copy has a bound at: a segment copied twice, skipped, or resumed at the wrong offset
/// changes the bytes from that point on.
std::byte patternAt(std::size_t offset)
{
    return static_cast<std::byte>(((offset * 31) + 7) % 251);
}

} // namespace

TEST_CASE("On Windows the default backend is the completion port, and the factories follow the loop",
          "[net][iocp][default]")
{
    // Task B7b's behaviour change, and the one every Windows consumer sees. Red against the
    // B7a default, which named WFMO here.
    CHECK(core::net::preferredBackendKind() == core::net::BackendKind::Iocp);
    auto const byDefault = core::net::makeDefaultBackend();
    REQUIRE(byDefault != nullptr);
    CHECK(byDefault->kind() == core::net::BackendKind::Iocp);
    CHECK(byDefault->completionPort() != nullptr);

    // And the socket a loop gets is the one its backend drives: the completion port's, which since
    // 0.5.0 is the only Windows backend (core-cpp#6).
    {
        auto loop = EventLoop { *byDefault };
        auto pair = core::net::testing::makeSocketPair(loop);
        REQUIRE(pair.has_value());
        CHECK(dynamic_cast<IocpSocket*>(pair->first.get()) != nullptr);
        auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
        REQUIRE(bound.has_value());
        CHECK(dynamic_cast<IocpListener*>(bound->get()) != nullptr);
    }
}

TEST_CASE("A Windows socket factory refuses a loop whose backend lends no completion port",
          "[net][iocp][default]")
{
    // The other half of core-cpp#6: with the readiness transport gone there is no socket a loop
    // without a port can drive, so every factory says so by name rather than handing out one that
    // would never complete. A loop over a test double is the case a consumer can reach.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    REQUIRE(loop.completionPort() == nullptr);

    auto const bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE_FALSE(bound.has_value());
    CHECK(bound.error().code == NetErrorCode::Unsupported);

    auto const unixBound = core::net::listenUnix(loop, "core-cpp-refused.sock");
    REQUIRE_FALSE(unixBound.has_value());
    CHECK(unixBound.error().code == NetErrorCode::Unsupported);

    auto const pair = core::net::testing::makeSocketPair(loop);
    REQUIRE_FALSE(pair.has_value());
    CHECK(pair.error().code == NetErrorCode::Unsupported);

    // adoptSocket closes what it was handed, refusal or not; adoptListener leaves it to the caller.
    core::platform::ensureWinsockInitialized();
    auto const socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    REQUIRE(socket != InvalidSocketValue);
    auto const adopted =
        core::net::adoptSocket(loop, reinterpret_cast<core::platform::NativeHandle>(socket), {});
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error().code == NetErrorCode::Unsupported);
    auto type = 0;
    auto size = static_cast<int>(sizeof(type));
    CHECK(::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == SOCKET_ERROR);

    auto const listening = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    REQUIRE(listening != InvalidSocketValue);
    auto const adoptedListener =
        core::net::adoptListener(loop, reinterpret_cast<core::platform::NativeHandle>(listening));
    REQUIRE_FALSE(adoptedListener.has_value());
    CHECK(adoptedListener.error().code == NetErrorCode::Unsupported);
    CHECK(::getsockopt(listening, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0);
    ::closesocket(listening);
}

namespace
{
/// Dials @p port on @p loop and records the answer.
Task<void> dialInto(EventLoop* loop, std::uint16_t port, std::optional<core::net::SocketResult>* out)
{
    *out =
        co_await core::net::connect(loop,
                                    "127.0.0.1",
                                    port,
                                    &core::net::defaultAsyncResolver(),
                                    core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } });
}
} // namespace

TEST_CASE("A Windows dial on a loop without a completion port refuses before it connects",
          "[net][iocp][default]")
{
    // The dial's half of the refusal above (review L2 of 0.5.0). It used to take the readiness path,
    // complete a real TCP handshake, and only then refuse the socket at adoption -- so the server
    // saw a connection accepted and reset per candidate. It must refuse as `dialCompletion` does:
    // up front, with nothing on the wire.
    core::platform::ensureWinsockInitialized();
    auto const listening = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    REQUIRE(listening != InvalidSocketValue);
    auto const closeListening =
        core::net::detail::ScopeGuard { [&]() noexcept { ::closesocket(listening); } };
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listening, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listening, 4) == 0);
    auto bound = sockaddr_in {};
    auto boundSize = static_cast<int>(sizeof(bound));
    REQUIRE(::getsockname(listening, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0);

    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto dialled = std::optional<core::net::SocketResult> {};
    loop.spawn(dialInto(&loop, ntohs(bound.sin_port), &dialled));
    std::ignore = loop.drain();

    // Nothing reached the listener: a pending connection would make it readable.
    auto pending = WSAPOLLFD { .fd = listening, .events = POLLRDNORM, .revents = 0 };
    CHECK(::WSAPoll(&pending, 1, 200) == 0);

    // And the answer came without a wait. Past the budget, for the run where it did not.
    clock.advance(std::chrono::seconds { 10 });
    std::ignore = loop.drain();
    REQUIRE(dialled.has_value());
    REQUIRE_FALSE(dialled->has_value());
    CHECK(dialled->error().code == NetErrorCode::Unsupported);
}

TEST_CASE("An IocpListener and IocpSocket round-trip bytes, and AcceptEx reports the peer",
          "[net][iocp][socket]")
{
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);
    auto const port = listener->boundPort();
    REQUIRE(port != 0);

    auto peer = std::string {};
    auto accepted = false;
    auto response = std::string {};
    auto connected = false;
    auto client = std::jthread { [port, &response, &connected] { connected = echoClient(port, &response); } };
    loop.blockOn(echoOnce(listener.get(), &peer, &accepted));
    client.join();

    REQUIRE(connected);
    REQUIRE(accepted);
    CHECK(response == "ping!");
    // Parsed out of AcceptEx's own address block by GetAcceptExSockaddrs, with no syscall.
    CHECK(peer == "127.0.0.1");
}

TEST_CASE("An IocpListener destroyed without close releases its listening socket", "[net][iocp][listener]")
{
    // Asserted through the PORT: the listener claims its address exclusively, so a listening
    // socket that outlived its owner would refuse the second bind with WSAEADDRINUSE.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto port = std::uint16_t { 0 };
    {
        auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
        REQUIRE(bound.has_value());
        port = (*bound)->boundPort();
        REQUIRE(port != 0);
        // Deliberately no close(): that omission is the whole question.
    }
    auto rebound = IocpListener::bind(loop, "127.0.0.1", port);
    INFO("rebinding port " << port);
    REQUIRE(rebound.has_value());
}

TEST_CASE("An IocpListener destroyed with an AcceptEx in flight survives the completion",
          "[net][iocp][listener]")
{
    // fastcached#465, the probe that established it: the kernel holds the accept's OVERLAPPED
    // and its address block, and closing the listener does not retract them -- it makes the
    // operation complete, later, into whatever is there. Under ASan a regression is a
    // use-after-free report; without one it is a crash or nothing.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    auto answer = std::optional<core::net::AcceptResult> {};
    auto accepting = [](core::net::IListener* from,
                        std::optional<core::net::AcceptResult>* out) -> DetachedTask {
        *out = co_await from->accept();
    };
    accepting(listener.get(), &answer);
    // Genuinely outstanding: nothing connected, and the port holds the AcceptEx.
    REQUIRE_FALSE(answer.has_value());
    REQUIRE(backend.issuedOperations() == 1);

    SECTION("destroyed without close")
    {
    }
    SECTION("closed first, which is not what makes it safe")
    {
        // The distinction upstream pinned: close() is what ABORTS the operation; it does not keep
        // its storage alive. If that reasoning ever comes back, both sections fail together.
        listener->close();
    }
    listener.reset();

    REQUIRE(pumpUntil(loop, [&answer] { return answer.has_value(); }));
    REQUIRE_FALSE(answer->has_value());
    CHECK(answer->error().code == NetErrorCode::Cancelled);
    CHECK(drainOwnerOperations(loop, backend));
}

TEST_CASE("An IocpSocket destroyed with a WSARecv in flight survives the completion", "[net][iocp][socket]")
{
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};
    parkOnRead(harness.socket.get(), &buffer, &read);
    // Nothing was sent, so the receive is genuinely the kernel's.
    REQUIRE_FALSE(read.resumed);
    REQUIRE(harness.backend.issuedOperations() == 1);

    harness.socket.reset();

    // The destructor ABANDONS the parked read -- the flow unwinds, because a socket that is gone
    // has nothing for it to look at -- and the receive completes later into its own node. The
    // loop resumes the flow, not the destructor (G2), so it has not run yet.
    CHECK_FALSE(read.resumed);
    std::ignore = harness.loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(read.abandoned);
    CHECK(drainOwnerOperations(harness.loop, harness.backend));
}

TEST_CASE("An IocpSocket destroyed mid-write holds the payload until the kernel is done",
          "[net][iocp][socket]")
{
    // The second defect #465 turned up, and not a crash: upstream's WSASend referenced the payload
    // rather than copying it, so a payload released at socket teardown was read by the kernel after
    // it was freed -- corruption on the wire. Here an overlapped send reads a copy its operation
    // owns, and the `keepAlive` a caller hands `writeVectored` is still held by that operation
    // until the kernel is done, which is what the interface promises. Asserted through OWNERSHIP
    // rather than by racing the wire.
    auto harness = Harness {};
    REQUIRE(fillThePipe(harness) > 0);

    auto payload = std::make_shared<std::vector<std::byte>>(8U * 1024U * 1024U, std::byte { 0xAB });
    auto const observer = std::weak_ptr<void const> { payload };
    auto const segments =
        std::array<std::span<std::byte const>, 1> { std::span<std::byte const> { *payload } };

    auto written = Outcome {};
    auto writing = [](core::net::ISocket* socket,
                      std::span<std::span<std::byte const> const> gather,
                      std::shared_ptr<void const> keep,
                      Outcome* out) -> DetachedTask {
        try
        {
            out->record(co_await socket->writeVectored(gather, std::move(keep)));
        }
        catch (core::async::OperationCancelled const&)
        {
            out->resumed = true;
            out->abandoned = true;
        }
    };
    writing(harness.socket.get(), segments, payload, &written);
    payload.reset();
    // Genuinely asynchronous, or the case proves nothing: the peer never reads 8MiB.
    REQUIRE_FALSE(written.resumed);
    REQUIRE(harness.backend.issuedOperations() == 1);
    REQUIRE_FALSE(observer.expired()); // the operation holds it now

    harness.socket.reset();

    // The moment that matters: the socket is gone and the kernel may still be reading. The loop
    // resumes the abandoned flow on its next drain.
    std::ignore = harness.loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(written.abandoned);
    REQUIRE_FALSE(observer.expired());

    // And it is released once the completion has been dequeued -- kept exactly as long as it
    // must be, rather than leaked.
    REQUIRE(drainOwnerOperations(harness.loop, harness.backend));
    CHECK(observer.expired());
}

TEST_CASE("cancelRead settles a parked probe before it returns", "[net][iocp][socket][cancelread]")
{
    // A probe is a ZERO-byte receive: it carries nothing, so it is settled at once, which is what
    // ISocket::cancelRead describes -- and its flow is resumed by the loop, on the next drain, not
    // inside the call (G2). No kernel completion stands between the call and the resumption.
    auto harness = Harness {};
    auto watch = Outcome {};
    parkOnProbe(harness.socket.get(), &watch);
    REQUIRE_FALSE(watch.resumed);

    harness.socket->cancelRead();

    CHECK_FALSE(watch.resumed);
    std::ignore = harness.loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(watch.resumed);
    REQUIRE(watch.code.has_value());
    CHECK(*watch.code == NetErrorCode::Cancelled);
    CHECK(drainOwnerOperations(harness.loop, harness.backend));
}

TEST_CASE("A real read retired by cancelRead settles, and its abort is converted rather than tabled",
          "[net][iocp][socket][cancelread][ntstatus]")
{
    // The other side of the asymmetry. Nothing was sent, so this receive is genuinely pending and
    // retiring it inline would have been safe -- but nothing at the moment of the call can tell it
    // from one that has already completed with bytes, so it settles unconditionally. If somebody
    // reintroduces inline retirement for the "obviously safe" case, only this goes red.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};
    parkOnRead(harness.socket.get(), &buffer, &read);
    REQUIRE_FALSE(read.resumed);

    harness.socket->cancelRead();
    CHECK_FALSE(read.resumed);

    // The slot is free even so, which is the half that IS synchronous: a read armed now gets its
    // own operation. In a Debug build a regression aborts on the read-slot guard instead.
    auto secondBuffer = std::array<std::byte, 64> {};
    auto second = Outcome {};
    parkOnRead(harness.socket.get(), &secondBuffer, &second);
    REQUIRE_FALSE(second.resumed);
    REQUIRE(harness.backend.issuedOperations() == 2);

    REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
    REQUIRE(read.code.has_value());
    // Both halves, because they are two claims: the code is what a caller branches on, and the
    // system code says the NTSTATUS the kernel wrote (STATUS_CANCELLED) went through
    // WSAGetOverlappedResult rather than through a table that grew a row for it.
    CAPTURE(read.systemCode);
    CHECK(*read.code == NetErrorCode::Cancelled);
    CHECK(read.systemCode == static_cast<int>(ERROR_OPERATION_ABORTED));
    CHECK_FALSE(second.resumed);
}

TEST_CASE("A reset IOCP read is reported as ConnReset, not Cancelled", "[net][iocp][socket][ntstatus]")
{
    // STATUS_CONNECTION_RESET on a socket that is still open, so the conversion has to ask
    // Winsock and cannot take the closed-socket shortcut. An abortive close sends an RST; a FIN
    // would complete the read with zero bytes, which is EOF and a different answer entirely.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};
    parkOnRead(harness.socket.get(), &buffer, &read);
    REQUIRE_FALSE(read.resumed);

    harness.pair.closeClient(RawPair::Close::Reset);

    REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
    REQUIRE(read.code.has_value());
    CAPTURE(read.systemCode);
    CHECK(*read.code == NetErrorCode::ConnReset);
    CHECK(read.systemCode == WSAECONNRESET);
}

TEST_CASE("A read armed in the same turn as cancelRead gets its own completion",
          "[net][iocp][socket][cancelread]")
{
    // fastcached#884's reuse, asserted as the property that replaced it. Upstream, the second
    // read reused the OVERLAPPED the kernel still owned and CLEARED the STATUS_CANCELLED it had
    // just written, so the abort dispatched as a successful read of zero bytes -- a spurious EOF
    // delivered to a reader whose own bytes then went nowhere. The byte count is the assertion:
    // "resumed successfully" is true of the defect too.
    auto harness = Harness {};
    auto firstBuffer = std::array<std::byte, 64> {};
    auto secondBuffer = std::array<std::byte, 64> {};
    auto first = Outcome {};
    auto second = Outcome {};

    parkOnRead(harness.socket.get(), &firstBuffer, &first);
    REQUIRE_FALSE(first.resumed);
    harness.socket->cancelRead();
    parkOnRead(harness.socket.get(), &secondBuffer, &second);
    REQUIRE_FALSE(second.resumed);

    harness.pair.send("fc");

    REQUIRE(pumpUntil(harness.loop, [&] { return first.resumed && second.resumed; }));
    CHECK_FALSE(second.code.has_value());
    CHECK(second.bytes == 2);
    // The first read's coroutine was the casualty upstream: never resumed, never freed.
    REQUIRE(first.code.has_value());
    CHECK(*first.code == NetErrorCode::Cancelled);
}

TEST_CASE("cancelRead over an already-completed receive keeps the bytes", "[net][iocp][socket][cancelread]")
{
    // CancelIoEx cannot un-receive. The race is FORCED rather than waited for: the peer sends and
    // nothing drives the loop, so when cancelRead runs the receive has completed in the kernel and
    // its packet is queued. The sleep is the rendezvous and nothing can assert it --
    // WSAGetOverlappedResult answers WSA_IO_INCOMPLETE for a pending operation and for a
    // completed-but-not-dequeued one alike (measured upstream) -- so on a loaded runner the sleep
    // can lose, cancelRead then aborts a still-pending read, and the case fails naming the bytes.
    auto harness = Harness {};
    auto firstBuffer = std::array<std::byte, 64> {};
    auto secondBuffer = std::array<std::byte, 64> {};
    auto first = Outcome {};
    auto second = Outcome {};

    parkOnRead(harness.socket.get(), &firstBuffer, &first);
    REQUIRE_FALSE(first.resumed);
    harness.pair.send("HELLO");
    std::this_thread::sleep_for(std::chrono::milliseconds { 300 });
    REQUIRE_FALSE(first.resumed);

    harness.socket->cancelRead();
    parkOnRead(harness.socket.get(), &secondBuffer, &second);
    harness.pair.send("hi");

    REQUIRE(pumpUntil(harness.loop, [&] { return first.resumed && second.resumed; }));
    // The first read keeps what its own receive transferred; under the defect it resumes
    // Cancelled with the five bytes gone from the stream.
    CHECK_FALSE(first.code.has_value());
    CHECK(first.bytes == 5);
    CHECK_FALSE(second.code.has_value());
    CHECK(second.bytes == 2);
}

namespace
{
/// Reads once into @p buffer. A lazy `Task`, so a case can start it by hand and then DESTROY it
/// while the read is parked -- the one abandon path that frees the awaiting frame without the
/// socket going away.
Task<void> readInto(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    std::ignore = co_await socket->read(buffer);
}

/// @param buffer What to inspect.
/// @return How many bytes of @p buffer are no longer the zero it was filled with.
std::size_t bytesWritten(std::span<std::byte const> buffer)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(buffer, [](std::byte b) { return b != std::byte { 0 }; }));
}
} // namespace

TEST_CASE("An abandoned read never lets the kernel write into the caller's buffer",
          "[net][iocp][socket][abandon]")
{
    // **The caller's buffer is borrowed for exactly as long as its operation is awaited**
    // (`ISocket`'s own contract), and on every abandon path that ends before the kernel is done
    // with the receive: an awaitable destroyed while parked, a socket destroyed under one, a loop
    // torn down. A receive still in the kernel then completes -- the peer writes after the frame is
    // gone -- into memory the caller has since freed or reused.
    //
    // **Observed directly rather than through AddressSanitizer, because ASan cannot see it.** The
    // kernel's copy into a user buffer is not an instrumented access, so a freed buffer the kernel
    // writes into reports nothing. The buffer here therefore OUTLIVES the read on purpose, filled
    // with zeroes, and the case asserts that the peer's bytes never landed in it: a byte that did
    // is exactly the write that would have gone into freed memory.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};

    SECTION("the awaitable is destroyed while its WSARecv is in the kernel")
    {
        auto task = readInto(harness.socket.get(), std::span<std::byte> { buffer });
        task.handle().resume(); // parks on the read: nothing has been sent
        REQUIRE(harness.backend.issuedOperations() == 1);
        {
            auto const discard = std::move(task); // the frame goes, and the awaitable with it
        }
        harness.pair.send("XYZ");
        REQUIRE(drainOwnerOperations(harness.loop, harness.backend));
    }
    SECTION("the socket is destroyed under a parked read")
    {
        auto read = Outcome {};
        parkOnRead(harness.socket.get(), &buffer, &read);
        REQUIRE(harness.backend.issuedOperations() == 1);
        harness.socket.reset();
        std::ignore = harness.loop.runOnce(std::chrono::milliseconds { 0 });
        CHECK(read.abandoned);
        // The peer's send may be refused outright here -- the close has already reset the
        // connection -- and that is fine: what matters is that nothing lands in the buffer.
        std::ignore = ::send(harness.pair.client(), "XYZ", 3, 0);
        REQUIRE(drainOwnerOperations(harness.loop, harness.backend));
    }

    CHECK(bytesWritten(buffer) == 0);
}

TEST_CASE("close() settles a parked read with a Cancelled VALUE, at once", "[net][iocp][socket]")
{
    // ISocket::close's contract, which the completion model does not relax: the operation is
    // settled HERE, not when its abort comes back, because the socket is gone and there is
    // nothing left for the kernel to hand the buffer to. Its flow is resumed by the loop's next
    // drain, not inside close() (G2), and without waiting for the abort.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};
    parkOnRead(harness.socket.get(), &buffer, &read);
    REQUIRE_FALSE(read.resumed);

    harness.socket->close();

    CHECK_FALSE(read.resumed);
    std::ignore = harness.loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(read.resumed);
    CHECK_FALSE(read.abandoned);
    REQUIRE(read.code.has_value());
    CHECK(*read.code == NetErrorCode::Cancelled);
    CHECK(harness.socket->isClosed());
    CHECK(drainOwnerOperations(harness.loop, harness.backend));
}

namespace
{
/// Reads once, reporting how it ended -- including a stop, which unwinds.
Task<void> readReporting(core::net::ISocket* socket, std::array<std::byte, 64>* buffer, Outcome* out)
{
    try
    {
        out->record(co_await socket->read(std::span<std::byte> { *buffer }));
    }
    catch (core::async::OperationCancelled const&)
    {
        out->resumed = true;
        out->abandoned = true;
    }
}

/// The winner of a race: optionally lets a receive complete in the kernel first, then returns,
/// which stops every loser.
Task<void> winAfter(RawPair* pair, std::string_view send, std::chrono::milliseconds settle)
{
    if (!send.empty())
    {
        pair->send(send);
        // Blocking the loop's thread on purpose: nothing may dequeue the receive's completion
        // before the stop reaches it, or this would test the ordinary path.
        std::this_thread::sleep_for(settle);
    }
    co_return;
}
} // namespace

TEST_CASE("A stop of the awaiting flow takes a pending read back and lets the completion report",
          "[net][iocp][socket][stop]")
{
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};

    SECTION("nothing had arrived: the flow unwinds once the abort is dequeued")
    {
        // `whenAny` is the one stop that reaches the operation's own route: requestStop would
        // unpark everything and prove nothing about the socket.
        harness.loop.blockOn(core::net::testing::anyOf(readReporting(harness.socket.get(), &buffer, &read),
                                                       winAfter(&harness.pair, {}, {})));
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        CHECK(read.abandoned);
    }
    SECTION("the bytes had arrived: they win over the stop (fastcached#884)")
    {
        // The stop is requested BEFORE the completion is dequeued, which is the only order that
        // tests anything: complete-then-stop resumes the flow before the stop and asserts nothing.
        harness.loop.blockOn(
            core::net::testing::anyOf(readReporting(harness.socket.get(), &buffer, &read),
                                      winAfter(&harness.pair, "HELLO", std::chrono::milliseconds { 300 })));
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        // A value beats a stop that arrived after it: those five bytes exist nowhere else.
        CHECK_FALSE(read.abandoned);
        CHECK_FALSE(read.code.has_value());
        CHECK(read.bytes == 5);
    }
    CHECK(drainOwnerOperations(harness.loop, harness.backend));
}

TEST_CASE("An IocpSocket honours the settled receive-deadline contract", "[net][iocp][socket][deadline]")
{
    // Found missing on WindowsSocket by Task B9's parity case; this socket implements it from the
    // start. d > 0 bounds reads started after the call, d <= 0 REMOVES the bound, and a read that
    // is already parked keeps the timer it armed.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};

    SECTION("a positive bound times a silent read out, through the completion")
    {
        harness.socket->setReceiveDeadline(std::chrono::milliseconds { 50 });
        parkOnRead(harness.socket.get(), &buffer, &read);
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        REQUIRE(read.code.has_value());
        CHECK(*read.code == NetErrorCode::Timeout);
    }
    SECTION("a non-positive one removes it")
    {
        harness.socket->setReceiveDeadline(std::chrono::milliseconds { 50 });
        harness.socket->setReceiveDeadline(std::chrono::milliseconds { 0 });
        parkOnRead(harness.socket.get(), &buffer, &read);
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds { 200 };
        std::ignore = pumpUntil(harness.loop,
                                [&] { return read.resumed || std::chrono::steady_clock::now() >= until; });
        REQUIRE_FALSE(read.resumed); // four deadlines' worth, and still parked
        harness.pair.send("late");
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        CHECK_FALSE(read.code.has_value());
        CHECK(read.bytes == 4);
    }
    SECTION("a read already parked keeps the timer it armed")
    {
        harness.socket->setReceiveDeadline(std::chrono::milliseconds { 50 });
        parkOnRead(harness.socket.get(), &buffer, &read);
        harness.socket->setReceiveDeadline(std::chrono::milliseconds { -1 });
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        REQUIRE(read.code.has_value());
        CHECK(*read.code == NetErrorCode::Timeout);
    }
}

TEST_CASE("A completion resumes its flow in the loop's turn, never inside the backend's walk",
          "[net][iocp][socket]")
{
    // Rule 1 for completions (G2): the backend reports the park and the LOOP resumes, so a flow
    // woken by its read finds no readiness dispatch in flight. A socket that completed its
    // awaitable from the port's dequeue would resume it right there.
    auto harness = Harness {};
    auto buffer = std::array<std::byte, 64> {};
    auto insideDispatch = std::optional<bool> {};
    auto reading = [](core::net::ISocket* socket,
                      std::array<std::byte, 64>* into,
                      std::optional<bool>* flag) -> DetachedTask {
        std::ignore = co_await socket->read(std::span<std::byte> { *into });
        *flag = core::net::detail::readinessDispatchInFlight();
    };
    reading(harness.socket.get(), &buffer, &insideDispatch);
    harness.pair.send("x");
    REQUIRE(pumpUntil(harness.loop, [&insideDispatch] { return insideDispatch.has_value(); }));
    CHECK_FALSE(*insideDispatch);
}

TEST_CASE("waitReadable measures what a zero-byte completion found", "[net][iocp][socket][waitreadable]")
{
    // A zero-byte receive completes with zero bytes whether data is waiting or the peer has gone,
    // so the answer is a MSG_PEEK (fastcached `Net/IocpSocket.cpp:327-334`, fastcached#677).
    auto harness = Harness {};
    auto watch = Outcome {};

    SECTION("data pending answers > 0 and consumes none of it")
    {
        parkOnProbe(harness.socket.get(), &watch);
        harness.pair.send("abc");
        REQUIRE(pumpUntil(harness.loop, [&watch] { return watch.resumed; }));
        CHECK_FALSE(watch.code.has_value());
        CHECK(watch.bytes > 0);

        auto buffer = std::array<std::byte, 64> {};
        auto read = Outcome {};
        parkOnRead(harness.socket.get(), &buffer, &read);
        REQUIRE(pumpUntil(harness.loop, [&read] { return read.resumed; }));
        CHECK(read.bytes == 3);
    }
    SECTION("a graceful close answers 0")
    {
        parkOnProbe(harness.socket.get(), &watch);
        harness.pair.closeClient(RawPair::Close::Graceful);
        REQUIRE(pumpUntil(harness.loop, [&watch] { return watch.resumed; }));
        CHECK_FALSE(watch.code.has_value());
        CHECK(watch.bytes == 0);
    }
}

TEST_CASE("An accept destroyed while parked leaves the next client to the next accept",
          "[net][iocp][listener][abandon]")
{
    // An accept issues its AcceptEx into a socket the accepting FRAME creates. A frame destroyed
    // while parked -- a lazy `Task` dropped, a chain whose root is freed -- must close that socket,
    // because closing it is what aborts the AcceptEx: left armed, it takes the next client, which
    // completes its handshake with nobody while the next accept() waits for a connection that has
    // already been used up.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    {
        auto first = listener->accept();
        first.handle().resume(); // parks on the AcceptEx: nothing has connected
        REQUIRE_FALSE(first.done());
        REQUIRE(backend.issuedOperations() == 1);
    } // the frame goes while parked, and with it the only reference to the accept socket

    auto const client = connectTo(listener->boundPort());
    REQUIRE(client != InvalidSocketValue);
    auto const hangUp = core::net::detail::ScopeGuard { [client]() noexcept { ::closesocket(client); } };

    auto answer = std::optional<core::net::AcceptResult> {};
    acceptInto(listener.get(), &answer);
    REQUIRE(pumpUntil(loop, [&answer] { return answer.has_value(); }));
    REQUIRE(answer->has_value());

    // Served, not merely accepted: bytes from THIS client reach the socket the accept handed out.
    REQUIRE(::send(client, "ping", 4, 0) == 4);
    auto buffer = std::array<std::byte, 64> {};
    auto read = Outcome {};
    parkOnRead(answer->value().get(), &buffer, &read);
    REQUIRE(pumpUntil(loop, [&read] { return read.resumed; }));
    CHECK_FALSE(read.code.has_value());
    CHECK(read.bytes == 4);

    answer.reset();
    listener.reset();
    CHECK(drainOwnerOperations(loop, backend));
}

TEST_CASE("A stop of the accepting flow takes the AcceptEx back, and the listener still serves",
          "[net][iocp][listener][stop]")
{
    // `CompletionWait` on an accept: the stop asks for the AcceptEx back and the abort completion
    // answers, which the listener converts -- as a Cancelled VALUE, the same answer a closed
    // listener gives -- rather than letting the frame unwind before the kernel is done with it.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    auto stopped = std::optional<core::net::AcceptResult> {};
    auto abandoned = false;
    loop.blockOn(core::net::testing::anyOf(acceptReporting(listener.get(), &stopped, &abandoned),
                                           winAfter(nullptr, {}, {})));
    REQUIRE(pumpUntil(loop, [&] { return stopped.has_value() || abandoned; }));
    REQUIRE(stopped.has_value());
    REQUIRE_FALSE(stopped->has_value());
    CHECK(stopped->error().code == NetErrorCode::Cancelled);

    // The stopped accept's socket went with it: the next client is the next accept's.
    auto const client = connectTo(listener->boundPort());
    REQUIRE(client != InvalidSocketValue);
    auto const hangUp = core::net::detail::ScopeGuard { [client]() noexcept { ::closesocket(client); } };
    auto answer = std::optional<core::net::AcceptResult> {};
    acceptInto(listener.get(), &answer);
    REQUIRE(pumpUntil(loop, [&answer] { return answer.has_value(); }));
    CHECK(answer->has_value());

    answer.reset();
    listener.reset();
    CHECK(drainOwnerOperations(loop, backend));
}

TEST_CASE("An accepting frame destroyed while close() has its waiter queued is taken out of the ready queue",
          "[net][iocp][listener][resume]")
{
    // `CompletionWait`'s destructor branch. `close()` settles the wait and hands its frame to the
    // loop; the frame's owner destroys it before the loop gets there. The ready queue must lose the
    // handle with the frame, or the next drain resumes freed storage.
    auto backend = IocpBackend {};
    auto loop = EventLoop { backend };
    auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    // BORROWED: the case holds the frame, so the case may destroy it.
    auto answer = std::optional<core::net::AcceptResult> {};
    auto abandoned = false;
    auto flow = acceptReporting(listener.get(), &answer, &abandoned);
    flow.handle().resume();
    REQUIRE_FALSE(flow.handle().done());

    auto const before = loop.readyCount();
    listener->close();
    REQUIRE(loop.readyCount() == before + 1); // settled Closed, and queued rather than resumed

    flow = {};
    CHECK(loop.readyCount() == before); // taken out with the frame

    for ([[maybe_unused]] auto const turn: { 0, 1, 2 })
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
    CHECK_FALSE(answer.has_value()); // nothing resumed the destroyed frame
    CHECK_FALSE(abandoned);

    listener.reset();
    CHECK(drainOwnerOperations(loop, backend));
}

TEST_CASE("Teardown resumes a borrowed accept whose listener destroying a spawned root closed",
          "[net][iocp][listener][resume][teardown]")
{
    // `~EventLoop` step 5 destroys the spawned roots. A root owning the listener closes it on the
    // way out, which settles a BORROWED accept's `CompletionWait` and queues its frame -- after the
    // last drain, so it stayed suspended with the wait still naming the loop, and destroying it
    // after the loop called `cancelPending` on freed storage. The fix drains what step 5 queues.
    auto backend = IocpBackend {};
    // Declared before the loop, so it outlives it, as a fixture's member would.
    auto answer = std::optional<core::net::AcceptResult> {};
    auto abandoned = false;
    auto flow = Task<void> {};
    {
        auto loop = EventLoop { backend };
        auto bound = IocpListener::bind(loop, "127.0.0.1", 0);
        REQUIRE(bound.has_value());

        flow = acceptReporting(bound->get(), &answer, &abandoned);
        flow.handle().resume();
        REQUIRE_FALSE(flow.handle().done());

        loop.spawn(holdListener(std::move(*bound)));
        for ([[maybe_unused]] auto const turn: { 0, 1, 2 })
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
        REQUIRE_FALSE(answer.has_value());
    }
    auto const done = flow.handle().done();
    CHECK(done);
    // Leaked rather than destroyed where the teardown left it suspended: its destructor would call
    // into the destroyed loop, the defect the CHECK above reports.
    if (!done)
        std::ignore = flow.release();
    // The listener was closed under it: the Cancelled VALUE `IListener::close` promises.
    REQUIRE(answer.has_value());
    REQUIRE_FALSE(answer->has_value());
    CHECK(answer->error().code == NetErrorCode::Cancelled);
    CHECK_FALSE(abandoned);
}

TEST_CASE("An overlapped write puts the caller's bytes on the wire exactly, in order, across the copy bound",
          "[net][iocp][socket][write]")
{
    // The copy-in path and every boundary it has. The pipe is filled first, so the writes cannot
    // take the fast path and are sent from copies their operations own (`copyOwed`), at most
    // 256KiB per operation, re-issued from the cursor each completion leaves:
    //
    // - a gathered write of 301'076 bytes, so it takes more than one operation, with the bound
    //   falling part-way THROUGH a segment (31'070 bytes into the fourth) and an empty segment the
    //   flattening must step over;
    // - a flat write of 300'007 bytes after it, the other branch of the copy. By the time it runs
    //   the peer is draining, so it may send part of itself inline and the rest overlapped, from a
    //   cursor that is not at zero.
    //
    // The peer asserts the count, the content and the order, byte for byte.
    auto harness = Harness {};
    auto const filler = fillThePipe(harness);
    REQUIRE(filler > 0);

    constexpr auto SegmentSizes = std::array<std::size_t, 5> { 100'003, 0, 131'071, 70'001, 1 };
    constexpr auto FlatSize = std::size_t { 300'007 };
    auto const gatheredSize = std::ranges::fold_left(SegmentSizes, std::size_t { 0 }, std::plus {});
    auto payload = std::vector<std::byte>(gatheredSize + FlatSize);
    for (auto const offset: std::views::iota(std::size_t { 0 }, payload.size()))
        payload[offset] = patternAt(offset);

    auto const whole = std::span<std::byte const> { payload };
    auto segments = std::vector<std::span<std::byte const>> {};
    auto cursor = std::size_t { 0 };
    for (auto const size: SegmentSizes)
    {
        segments.push_back(whole.subspan(cursor, size));
        cursor += size;
    }

    auto gathered = Outcome {};
    auto flat = Outcome {};
    auto writing = [](core::net::ISocket* socket,
                      std::span<std::span<std::byte const> const> gather,
                      std::span<std::byte const> rest,
                      Outcome* first,
                      Outcome* second) -> DetachedTask {
        first->record(co_await socket->writeVectored(gather, nullptr));
        second->record(co_await socket->write(rest));
    };
    writing(harness.socket.get(), segments, whole.subspan(gatheredSize), &gathered, &flat);
    // Genuinely overlapped, or the case proves nothing about the copy.
    REQUIRE_FALSE(gathered.resumed);
    REQUIRE(harness.backend.issuedOperations() == 1);

    auto const expected = filler + payload.size();
    auto received = std::vector<char> {};
    auto peer = std::jthread { [client = harness.pair.client(), expected, &received] {
        received = receiveExactly(client, expected);
    } };
    REQUIRE(pumpUntil(harness.loop, [&flat] { return flat.resumed; }));
    peer.join();

    CHECK_FALSE(gathered.code.has_value());
    CHECK(gathered.bytes == gatheredSize);
    CHECK_FALSE(flat.code.has_value());
    CHECK(flat.bytes == FlatSize);
    REQUIRE(received.size() == expected);
    auto const wire = std::span<char const> { received };
    CHECK(std::ranges::all_of(wire.first(filler), [](char c) { return c == FillerByte; }));
    auto const sent = wire.subspan(filler);
    auto const differs = std::ranges::mismatch(
        sent, payload, [](char c, std::byte b) { return static_cast<std::byte>(c) == b; });
    auto const firstDifference = static_cast<std::size_t>(differs.in1 - sent.begin());
    INFO("the first byte that differs is at payload offset " << firstDifference);
    CHECK(firstDifference == payload.size());
}

// A write armed over a parked one -- which the case that stood here drove under NDEBUG, where the
// guard was compiled out and the socket kept the orphan reachable -- now ends the process in every
// build (core/net/SocketContract.hpp): `core-cpp.socket-contract-canary.write-slot` watches it.
