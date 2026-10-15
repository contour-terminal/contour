// SPDX-License-Identifier: Apache-2.0
//
// An AF_UNIX listener on Windows belongs to the loop it is made on, as `listen` and `adoptListener`
// already do: an IOCP loop -- the Windows default -- gets an `IocpListener` whose accepted sockets
// are `IocpSocket`s, and `connectUnix` dials an `IocpSocket` there too. `listenUnix` used to build
// the readiness listener whatever the loop was, so an IOCP loop served AF_UNIX through readiness --
// found through contour, whose daemon listens on a unix socket. Since 0.5.0 the completion port is
// the only Windows transport (core-cpp#6).

// winsock2.h MUST precede windows.h / ws2tcpip.h, and afunix.h needs what they declare.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <afunix.h>
// clang-format on

#include <core/Utf8Path.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/IocpSocket.hpp>
#include <core/platform/WinsockInit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>

using core::async::Task;
using core::net::EventLoop;
using core::net::IListener;
using core::net::ISocket;
using core::net::testing::BackendMatrix;

namespace
{

/// Which transport family a listener or socket belongs to.
enum class Family : std::uint8_t
{
    None,       ///< Not reached: the flow failed before it had one.
    Completion, ///< `IocpListener` / `IocpSocket`.
    Other,      ///< Neither.
};

[[nodiscard]] Family familyOf(IListener const* listener) noexcept
{
    if (dynamic_cast<core::net::IocpListener const*>(listener) != nullptr)
        return Family::Completion;
    return Family::Other;
}

[[nodiscard]] Family familyOf(ISocket const* socket) noexcept
{
    if (dynamic_cast<core::net::IocpSocket const*>(socket) != nullptr)
        return Family::Completion;
    return Family::Other;
}

/// What the echo observed.
struct Echo
{
    Family accepted = Family::None;
    Family connected = Family::None;
    bool served = false;
    bool matched = false;
};

/// Accepts one connection and echoes one read.
Task<void> serve(IListener* listener, Echo* echo)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto conn = std::move(*accepted);
    echo->accepted = familyOf(conn.get());
    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await conn->read(buffer);
    if (!got.has_value() || *got == 0)
        co_return;
    auto const wrote = co_await conn->write(std::span<std::byte const> { buffer }.subspan(0, *got));
    echo->served = wrote.has_value() && *wrote == *got;
}

/// Connects, sends a request, and compares the echo. Closes the listener when it cannot connect,
/// so the accept beside it is not left parked with nothing to wake it.
Task<void> dial(EventLoop* loop, IListener* listener, std::string path, Echo* echo)
{
    auto connected = co_await core::net::connectUnix(loop, path);
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto sock = std::move(*connected);
    echo->connected = familyOf(sock.get());
    auto const request = std::string_view { "unix-iocp" };
    if (auto const wrote = co_await sock->write(std::as_bytes(std::span { request })); !wrote.has_value())
        co_return;
    auto buffer = std::array<std::byte, 32> {};
    auto const got = co_await sock->read(buffer);
    if (got.has_value())
        echo->matched = std::string_view { reinterpret_cast<char const*>(buffer.data()), *got } == request;
}

Task<void> echoOnce(EventLoop* loop, IListener* listener, std::string path, Echo* echo)
{
    co_await core::async::whenAll(serve(listener, echo), dial(loop, listener, std::move(path), echo));
}

/// @param path A path, in UTF-8 as `sun_path` is.
/// @return Whether something is there. Asked of the entry itself: a bound AF_UNIX socket is a
///         reparse point, which `std::filesystem::exists` follows and then cannot answer for.
[[nodiscard]] bool entryExists(std::string const& path)
{
    return ::GetFileAttributesW(core::pathFromTrustedUtf8(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// Leaves a STALE socket file at @p path: a socket bound there and closed, as a crashed server's.
/// @return Whether the file is there.
[[nodiscard]] bool leaveStaleSocketFile(std::string const& path)
{
    core::platform::ensureWinsockInitialized();
    auto const sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == core::net::detail::InvalidSocket)
        return false;
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    auto const bound =
        ::bind(sock, reinterpret_cast<sockaddr const*>(&address), static_cast<int>(sizeof(address))) == 0;
    ::closesocket(sock);
    return bound && entryExists(path);
}

/// A fresh directory under the temp directory, removed with this.
class TempDirectory
{
  public:
    TempDirectory():
        _path(std::filesystem::temp_directory_path()
              / std::format("core-cpp-unix-{}", std::random_device {}()))
    {
        std::filesystem::create_directories(_path);
    }
    TempDirectory(TempDirectory const&) = delete;
    TempDirectory& operator=(TempDirectory const&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;
    ~TempDirectory()
    {
        auto ec = std::error_code {};
        std::filesystem::remove_all(_path, ec);
    }

    /// @param name A file name, in UTF-8.
    /// @return Its path inside this directory, in UTF-8 as `sun_path` is.
    [[nodiscard]] std::string file(std::string_view name) const
    {
        return core::pathToUtf8(_path / core::pathFromTrustedUtf8(name));
    }

  private:
    std::filesystem::path _path;
};

} // namespace

TEST_CASE("A unix listener's socket file goes with it, a stale one is reclaimed and a live one refused",
          "[net][afunix][iocp]")
{
    // The path claim (`UnixSocketPath.cpp`), through `IocpListener::bindUnix`: it claims a path
    // and deletes its file on close.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const directory = TempDirectory {};
            auto const path = directory.file("claimed.sock");

            REQUIRE(leaveStaleSocketFile(path));
            auto listener = core::net::listenUnix(loop, path);
            if (!listener.has_value() && listener.error().code == core::net::NetErrorCode::Unsupported)
                SKIP("AF_UNIX not supported on this platform");
            REQUIRE(listener.has_value()); // the stale file was reclaimed
            CHECK(entryExists(path));

            auto second = core::net::listenUnix(loop, path);
            REQUIRE_FALSE(second.has_value()); // a live server keeps its path
            CHECK(second.error().code == core::net::NetErrorCode::AddressInUse);
            CHECK(entryExists(path)); // and its file

            listener->reset();
            CHECK_FALSE(entryExists(path)); // the file went with the listener
        }
    }
}

TEST_CASE("A unix socket path beyond ASCII is bound, deleted and reclaimed by its UTF-8 name",
          "[net][afunix][iocp]")
{
    // Windows' AF_UNIX reads `sun_path` as UTF-8: measured on Windows 11 (26200) under CP-1252, the
    // bytes C3 BC bind a file named U+00FC. listenUnix() made the parent directory, and bindUnix()
    // probed for a stale file and deleted its own, in the ANSI code page instead, so under
    // C:\Users\Jürgen the socket was bound into a directory nothing had made, and a listener that did
    // bind left its file behind for the next one to trip over.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const directory = TempDirectory {};
            // The parent is not there yet: listenUnix() makes it.
            auto const path = directory.file("J\xC3\xBCrgen-\xCE\xA9/daemon.sock");

            auto listener = core::net::listenUnix(loop, path);
            if (!listener.has_value() && listener.error().code == core::net::NetErrorCode::Unsupported)
                SKIP("AF_UNIX not supported on this platform");
            REQUIRE(listener.has_value());
            CHECK(entryExists(path));

            // The probe finds a live server by the same name: a probe that looked elsewhere answered
            // "absent", and the deletion that follows it took the live server's file.
            auto second = core::net::listenUnix(loop, path);
            REQUIRE_FALSE(second.has_value());
            CHECK(second.error().code == core::net::NetErrorCode::AddressInUse);
            CHECK(entryExists(path));

            listener->reset();
            CHECK_FALSE(entryExists(path)); // the file went with the listener

            REQUIRE(leaveStaleSocketFile(path));
            auto reclaimed = core::net::listenUnix(loop, path);
            REQUIRE(reclaimed.has_value()); // the stale file was found, and reclaimed
            reclaimed->reset();
            CHECK_FALSE(entryExists(path));

            // And a file that is not a socket is never touched.
            auto const notes = directory.file("J\xC3\xBCrgen-\xCE\xA9/notes.txt");
            std::ofstream { core::pathFromTrustedUtf8(notes) } << "keep";
            auto refused = core::net::listenUnix(loop, notes);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == core::net::NetErrorCode::AddressInUse);
            CHECK(entryExists(notes));
        }
    }
}

TEST_CASE("A unix socket path that is not UTF-8 is refused as a value", "[net][afunix][iocp]")
{
    // A path that is not UTF-8 names no file AF_UNIX could bind, nor one the listener could make,
    // probe or delete. A caller's path is text from outside the program, so it is a NetError, as a
    // path too long for `sun_path` is; it threw std::system_error out of listenUnix() before.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const directory = TempDirectory {};
            // "Jürgen" in CP-1252: 0xFC starts no UTF-8 sequence. The parent is not there yet, and
            // a listener that went ahead would make it.
            auto const parent = directory.file("sockets");
            auto const path = parent + "\\J\xFCrgen.sock";

            auto const listener = core::net::listenUnix(loop, path);
            REQUIRE_FALSE(listener.has_value());
            CHECK(listener.error().code == core::net::NetErrorCode::AddressError);
            CHECK_FALSE(entryExists(parent));

            auto const bound = core::net::IocpListener::bindUnix(loop, path);
            REQUIRE_FALSE(bound.has_value());
            CHECK(bound.error().code == core::net::NetErrorCode::AddressError);
        }
    }
}

TEST_CASE("listenUnix and connectUnix belong to the loop's transport family", "[net][afunix][iocp]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto const expected = Family::Completion;

            auto const directory = std::filesystem::temp_directory_path()
                                   / std::format("core-cpp-unix-{}", std::random_device {}());
            auto const path = core::pathToUtf8(directory / "echo.sock");
            auto listener = core::net::listenUnix(loop, path);
            if (!listener.has_value())
            {
                REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
                SKIP("AF_UNIX not supported on this platform");
            }
            auto const listenerFamily = familyOf(listener->get());

            auto echo = Echo {};
            auto const finished = loop.blockOn(core::net::withTimeout(
                &loop, echoOnce(&loop, listener->get(), path, &echo), std::chrono::seconds { 10 }));
            listener->reset();
            auto ec = std::error_code {};
            std::filesystem::remove_all(directory, ec);

            CHECK(finished); // or it waited 10s for an accept or a read that never came
            CHECK(listenerFamily == expected);
            CHECK(echo.accepted == expected);
            CHECK(echo.connected == expected);
            CHECK(echo.served);
            CHECK(echo.matched);
        }
    }
}
