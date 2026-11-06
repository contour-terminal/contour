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
#include <core/net/ICompletionPort.hpp>
#include <core/net/IListener.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/platform/WinsockInit.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

namespace
{

/// Closes a socket the case still owns when it ends, however it ends: a failed `REQUIRE` included.
class OwnedSocket
{
  public:
    explicit OwnedSocket(SOCKET socket) noexcept: _socket(socket) {}
    ~OwnedSocket()
    {
        if (_socket != core::net::detail::InvalidSocket)
            ::closesocket(_socket);
    }

    OwnedSocket(OwnedSocket const&) = delete;
    OwnedSocket& operator=(OwnedSocket const&) = delete;
    OwnedSocket(OwnedSocket&&) = delete;
    OwnedSocket& operator=(OwnedSocket&&) = delete;

    /// @return The socket, still owned here.
    [[nodiscard]] SOCKET get() const noexcept { return _socket; }

    /// @return The socket as the handle `adoptListener` takes.
    [[nodiscard]] core::platform::NativeHandle handle() const noexcept
    {
        return reinterpret_cast<core::platform::NativeHandle>(_socket);
    }

    /// Gives the socket up, to a listener that now closes it.
    void handOver() noexcept { _socket = core::net::detail::InvalidSocket; }

    /// Closes the socket now, once.
    /// @return What `closesocket` answered.
    [[nodiscard]] int close() noexcept
    {
        return ::closesocket(std::exchange(_socket, core::net::detail::InvalidSocket));
    }

  private:
    SOCKET _socket;
};

/// Closes a kernel handle (a file, a completion port) the case owns when it ends.
class OwnedHandle
{
  public:
    explicit OwnedHandle(HANDLE handle) noexcept: _handle(handle) {}
    ~OwnedHandle()
    {
        if (isOpen())
            ::CloseHandle(_handle);
    }

    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;
    OwnedHandle(OwnedHandle&&) = delete;
    OwnedHandle& operator=(OwnedHandle&&) = delete;

    /// @return The handle, still owned here.
    [[nodiscard]] HANDLE get() const noexcept { return _handle; }

    /// @return Whether a handle was opened and is not closed yet.
    [[nodiscard]] bool isOpen() const noexcept
    {
        return _handle != nullptr && _handle != INVALID_HANDLE_VALUE;
    }

    /// Closes the handle now, once.
    /// @return What `CloseHandle` answered.
    [[nodiscard]] BOOL close() noexcept { return ::CloseHandle(std::exchange(_handle, nullptr)); }

  private:
    HANDLE _handle;
};

/// A completion port that refuses every association with one Win32 error and forwards everything
/// else to a real port: the way to make `adoptListener` fail PAST the checks a socket passes, at the
/// association -- which is where it used to close the caller's socket (core-cpp#92).
class RefusingPort final: public core::net::ICompletionPort
{
  public:
    /// @param real The port everything but @c associate goes to (not owned).
    /// @param refusal The `GetLastError()` code every association fails with.
    RefusingPort(core::net::ICompletionPort& real, DWORD refusal) noexcept: _real(real), _refusal(refusal) {}

    [[nodiscard]] std::expected<void, core::net::NetError> associate(
        core::platform::NativeHandle /*handle*/) override
    {
        ++_attempts;
        return std::unexpected(core::net::makeNetError(
            core::net::NetErrorCode::SystemError, static_cast<int>(_refusal), "RefusingPort::associate"));
    }

    [[nodiscard]] bool isAssociated(core::platform::NativeHandle handle) const noexcept override
    {
        return _real.isAssociated(handle);
    }

    void forget(core::platform::NativeHandle handle) noexcept override { _real.forget(handle); }
    void beginOperation(void* operation) override { _real.beginOperation(operation); }
    void withdrawOperation(void* operation) noexcept override { _real.withdrawOperation(operation); }

    [[nodiscard]] core::platform::NativeHandle nativeHandle() const noexcept override
    {
        return _real.nativeHandle();
    }

    /// @return How many associations were asked for.
    [[nodiscard]] int attempts() const noexcept { return _attempts; }

  private:
    core::net::ICompletionPort& _real;
    DWORD _refusal;
    int _attempts = 0;
};

/// The platform's backend, with a @c RefusingPort lent in place of its own port.
class RefusingPortBackend final: public core::net::IoBackend
{
  public:
    /// @param real A backend that lends a completion port; owned from here on.
    /// @param refusal The `GetLastError()` code every association fails with.
    RefusingPortBackend(std::unique_ptr<core::net::IoBackend> real, DWORD refusal):
        _real(std::move(real)), _port(*_real->completionPort(), refusal)
    {
    }

    [[nodiscard]] core::net::BackendKind kind() const noexcept override { return _real->kind(); }

    [[nodiscard]] std::expected<void, core::net::NetError> attach(
        core::net::ReadinessHandler& handler) override
    {
        return _real->attach(handler);
    }

    [[nodiscard]] std::expected<void, core::net::NetError> setInterest(core::net::ReadinessHandler& handler,
                                                                       core::net::Interest interest) override
    {
        return _real->setInterest(handler, interest);
    }

    void detach(core::net::ReadinessHandler& handler) noexcept override { _real->detach(handler); }

    [[nodiscard]] core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        return _real->wait(timeout);
    }

    void wake() noexcept override { _real->wake(); }

    [[nodiscard]] core::net::ICompletionPort* completionPort() noexcept override { return &_port; }

    /// @return How many associations the lent port was asked for.
    [[nodiscard]] int associationAttempts() const noexcept { return _port.attempts(); }

  private:
    std::unique_ptr<core::net::IoBackend> _real;
    RefusingPort _port;
};

/// @return The platform's backend, checked to lend a completion port.
[[nodiscard]] std::unique_ptr<core::net::IoBackend> portBackend()
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    REQUIRE(backend->completionPort() != nullptr);
    return backend;
}

/// @param socket A socket.
/// @return Whether @p socket is still an open socket.
[[nodiscard]] bool isOpenSocket(SOCKET socket) noexcept
{
    auto type = 0;
    auto length = int { sizeof(type) };
    return ::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) == 0;
}

} // namespace

TEST_CASE("adopting a handle that is no socket leaves it to the caller", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    // A real, open handle that is not a socket: the adoption fails, and the contract says the
    // caller still owns the handle then -- so it must still be open, and closable exactly once.
    auto const directory = core::testing::ScopedTempDir { "core-cpp-adopt-file" };
    auto const path = directory / "not-a-socket";
    auto file = OwnedHandle { ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
    REQUIRE(file.isOpen());

    auto const refused = core::net::adoptListener(loop, file.get());
    REQUIRE_FALSE(refused.has_value());
    // Where it failed: asking the handle for `AcceptEx`, which only a socket answers.
    CHECK(refused.error().code == core::net::NetErrorCode::BadHandle);
    CHECK(refused.error().systemCode == WSAENOTSOCK);
    CHECK(refused.error().context == "WSAIoctl(AcceptEx)");

    auto flags = DWORD { 0 };
    CHECK(::GetHandleInformation(file.get(), &flags) != FALSE);
    CHECK(file.close() != FALSE);
}

TEST_CASE("a socket the loop's port refuses is left open for the caller", "[net]")
{
    // Past every check a listening socket passes, and refused at the association: the failure
    // that used to close the caller's socket (core-cpp#92).
    auto backend = RefusingPortBackend { portBackend(), ERROR_ACCESS_DENIED };
    auto loop = EventLoop { backend };
    auto socket = OwnedSocket { bindRaw().socket };

    auto const refused = core::net::adoptListener(loop, socket.handle());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().systemCode == ERROR_ACCESS_DENIED);
    // Not the error a socket another port holds gets, so nothing was released or retried.
    CHECK(backend.associationAttempts() == 1);

    CHECK(isOpenSocket(socket.get()));
    CHECK(socket.close() == 0);
}

TEST_CASE("a socket still refused once released is left open for the caller", "[net]")
{
    // Bound to another port, so the release succeeds, and refused again after it: the path where
    // adoption fails having changed the socket, which is still the caller's.
    auto backend = RefusingPortBackend { portBackend(), ERROR_INVALID_PARAMETER };
    auto loop = EventLoop { backend };
    auto socket = OwnedSocket { bindRaw().socket };
    auto const otherPort = OwnedHandle { ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1) };
    REQUIRE(otherPort.isOpen());
    REQUIRE(::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket.get()), otherPort.get(), 1, 0)
            == otherPort.get());

    auto const refused = core::net::adoptListener(loop, socket.handle());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().systemCode == ERROR_INVALID_PARAMETER);
    // Asked, released, asked again: the second refusal is what the caller is told.
    CHECK(backend.associationAttempts() == 2);

    CHECK(isOpenSocket(socket.get()));
    CHECK(socket.close() == 0);
}

TEST_CASE("a listener another completion port holds is adopted and accepts", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    // As a live upgrade hands it over: bound, listening, and bound to the completion port of the
    // process that made it -- here a port of the test's own, which the loop's is not.
    auto const raw = bindRaw();
    auto socket = OwnedSocket { raw.socket };
    auto const otherPort = OwnedHandle { ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1) };
    REQUIRE(otherPort.isOpen());
    REQUIRE(::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket.get()), otherPort.get(), 1, 0)
            == otherPort.get());

    auto adopted = core::net::adoptListener(loop, socket.handle());
    INFO((adopted.has_value() ? std::string {} : adopted.error().toString()));
    REQUIRE(adopted.has_value());
    socket.handOver();
    auto listener = std::move(*adopted);

    auto served = false;
    auto wrote = false;
    loop.blockOn(core::net::testing::allOf(acceptAndRead(listener.get(), "hello", &served),
                                           dialAndWrite(&loop, raw.port, "hello", &wrote)));
    CHECK(wrote);
    CHECK(served);
    listener->close();
}

TEST_CASE("an adopted listener is not inherited", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    // Inheritable, as a socket handed down to a child process is: the adopting process must not
    // hand it on to every process it starts in turn.
    auto socket = OwnedSocket { bindRaw().socket };
    auto* const handle = reinterpret_cast<HANDLE>(socket.get());
    REQUIRE(::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != FALSE);

    auto adopted = core::net::adoptListener(loop, handle);
    REQUIRE(adopted.has_value());
    socket.handOver();
    auto listener = std::move(*adopted);

    auto flags = DWORD { 0 };
    REQUIRE(::GetHandleInformation(handle, &flags) != FALSE);
    CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
    listener->close();
}

TEST_CASE("a listener names its native handle", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto socket = OwnedSocket { bindRaw().socket };
    auto const handle = socket.handle();
    auto adopted = core::net::adoptListener(loop, handle);
    REQUIRE(adopted.has_value());
    socket.handOver();
    auto listener = std::move(*adopted);

    CHECK(listener->nativeHandle() == handle);
    listener->close();
    CHECK(listener->nativeHandle() == core::platform::InvalidHandle);
}
