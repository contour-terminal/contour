// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>

    #include <array>
    #include <cstddef>
    #include <cstdio>
    #include <cstring>
    #include <filesystem>
    #include <span>
    #include <string>

    #include <unistd.h>

    #include <coro/Task.hpp>
    #include <coro/WhenAll.hpp>
    #include <net/EventLoop.h>
    #include <net/PollEventSource.h>
    #include <net/Sockets.h>
    #include <net/posix/UnixListener.h>

using coro::Task;
using net::EventLoop;

namespace
{

namespace fs = std::filesystem;

/// A unique per-test directory under the system temp dir, removed on destruction.
struct TempDir
{
    fs::path path;

    TempDir()
    {
        auto templ = (fs::temp_directory_path() / "contour-mux-test-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path = templ;
    }

    ~TempDir()
    {
        auto ec = std::error_code {};
        fs::remove_all(path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

/// The server flow: accept connections until one carries a request, then echo it
/// back. A connection that closes without sending (e.g. a liveness probe) is
/// drained and ignored, so it never counts as the one real request.
Task<void> echoOnce(net::IListener* listener, bool* served)
{
    while (!*served)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
            co_return;
        auto conn = std::move(*accepted);

        auto buffer = std::array<std::byte, 64> {};
        auto const got = co_await conn->read(buffer);
        if (!got.has_value() || *got == 0)
            continue; // a dropped/empty connection: keep waiting for a real request
        auto const echoed = co_await conn->write(std::span<std::byte const> { buffer }.subspan(0, *got));
        *served = echoed.has_value() && *echoed == *got;
    }
}

/// The client flow: connect to @p path, send a probe, read the echo back.
Task<void> connectAndProbe(EventLoop* loop, std::string path, bool* matched)
{
    auto connected = co_await net::connectUnix(loop, path);
    if (!connected.has_value())
        co_return;
    auto sock = std::move(*connected);

    auto const probe = std::string_view { "probe" };
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(probe.data()), probe.size() };
    if (auto const wrote = co_await sock->write(bytes); !wrote.has_value())
        co_return;

    auto buffer = std::array<std::byte, 64> {};
    auto const got = co_await sock->read(buffer);
    *matched = got.has_value() && *got == probe.size()
               && std::memcmp(buffer.data(), probe.data(), probe.size()) == 0;
}

} // namespace

TEST_CASE("listenUnix + connectUnix echo over a socket file", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "sockets";
    auto const socketPath = (socketDir / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    // The hardened directory was created owner-only.
    struct stat info {};
    REQUIRE(::lstat(socketDir.c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0700);

    auto served = false;
    auto matched = false;
    auto run = [](net::IListener* l, EventLoop* lp, std::string p, bool* s, bool* m) -> Task<void> {
        co_await coro::whenAll(echoOnce(l, s), connectAndProbe(lp, std::move(p), m));
    };
    loop.blockOn(run(listener->get(), &loop, socketPath, &served, &matched));

    REQUIRE(served);
    REQUIRE(matched);
}

TEST_CASE("a stale socket file is unlinked before rebinding", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "run";
    REQUIRE(::mkdir(socketDir.c_str(), 0700) == 0);
    auto const socketPath = (socketDir / "default").string();

    // Plant a stale socket file the way a crashed server leaves one: bind a raw
    // AF_UNIX socket, then close the fd WITHOUT unlinking the path.
    {
        auto address = sockaddr_un {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
        auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
        ::close(fd);
    }
    REQUIRE(std::filesystem::exists(socketPath)); // the corpse is in the way

    // Without the pre-bind unlink, bind() would fail with EADDRINUSE here.
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
}

TEST_CASE("a live server on the path is not hijacked", "[net][unix]")
{
    // The mirror image of the stale case: when a live server DOES answer the
    // path, a second bind must be refused rather than unlink the live socket out
    // from under it (tmux's connect-first policy). Otherwise the incumbent keeps
    // all its sessions but becomes unreachable forever.
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "run";
    auto const socketPath = (socketDir / "default").string();

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto first = net::listenUnix(loop, socketPath);
    REQUIRE(first.has_value());

    // The path is live: a second bind is refused with AddressInUse, and the
    // incumbent's socket file is left intact.
    auto second = net::listenUnix(loop, socketPath);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().code == net::NetErrorCode::AddressInUse);
    REQUIRE(std::filesystem::exists(socketPath));

    // The first listener still serves afterwards: a client connects and gets its
    // probe echoed back. (The refused bind's liveness probe left a dropped
    // connection queued, which echoOnce drains before the real request.)
    auto served = false;
    auto matched = false;
    auto run = [](net::IListener* l, EventLoop* lp, std::string p, bool* s, bool* m) -> Task<void> {
        co_await coro::whenAll(echoOnce(l, s), connectAndProbe(lp, std::move(p), m));
    };
    loop.blockOn(run(first->get(), &loop, socketPath, &served, &matched));

    REQUIRE(served);
    REQUIRE(matched);
}

TEST_CASE("a world-accessible socket directory is refused", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "exposed";
    REQUIRE(::mkdir(socketDir.c_str(), 0707) == 0); // world rwx: unsafe

    auto const check = net::ensureOwnedPrivateDirectory(socketDir);
    REQUIRE_FALSE(check.has_value());

    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };
    auto listener = net::listenUnix(loop, (socketDir / "default").string());
    REQUIRE_FALSE(listener.has_value());
}

TEST_CASE("a group-accessible socket directory is permitted, mirroring tmux", "[net][unix]")
{
    // tmux's TMUX_SOCK_PERM masks only o+rwx: group bits do not disqualify a
    // directory. Mirror that exactly rather than inventing a stricter policy.
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "grouped";
    REQUIRE(::mkdir(socketDir.c_str(), 0770) == 0);

    REQUIRE(net::ensureOwnedPrivateDirectory(socketDir).has_value());
}

TEST_CASE("a non-directory socket parent is refused", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto const filePath = tmp.path / "not-a-dir";
    { // create a plain file where the directory should be
        auto* f = ::fopen(filePath.c_str(), "w");
        REQUIRE(f != nullptr);
        ::fclose(f);
    }

    REQUIRE_FALSE(net::ensureOwnedPrivateDirectory(filePath).has_value());
}

TEST_CASE("connectUnix to a missing socket reports connection refused", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto source = net::PollEventSource {};
    auto loop = EventLoop { source };

    auto failed = false;
    auto tryConnect = [](EventLoop* lp, std::string p, bool* out) -> Task<void> {
        auto connected = co_await net::connectUnix(lp, std::move(p));
        *out = !connected.has_value();
    };
    loop.blockOn(tryConnect(&loop, (tmp.path / "nothing-here").string(), &failed));

    REQUIRE(failed);
}

#endif // !_WIN32
