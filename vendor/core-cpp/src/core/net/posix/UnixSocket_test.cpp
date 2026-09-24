// SPDX-License-Identifier: Apache-2.0

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/posix/FdUtils.hpp>
#include <core/net/posix/UnixListener.hpp>

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

#include <fcntl.h>
#include <unistd.h>

using core::async::Task;
using core::net::EventLoop;

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

/// Enters a directory for the duration of a test and returns to the previous one on destruction,
/// so a failing assertion cannot leave the whole binary running from a directory that is about to
/// be deleted.
struct ScopedWorkingDirectory
{
    fs::path previous = fs::current_path();

    explicit ScopedWorkingDirectory(fs::path const& directory) { fs::current_path(directory); }

    ~ScopedWorkingDirectory()
    {
        auto ec = std::error_code {};
        fs::current_path(previous, ec);
    }

    ScopedWorkingDirectory(ScopedWorkingDirectory const&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory const&) = delete;
    ScopedWorkingDirectory(ScopedWorkingDirectory&&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory&&) = delete;
};

/// The server flow: accept connections until one carries a request, then echo it
/// back. A connection that closes without sending (e.g. a liveness probe) is
/// drained and ignored, so it never counts as the one real request.
Task<void> echoOnce(core::net::IListener* listener, bool* served)
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
///
/// Closes @p listener on EVERY branch that gives up, because its whenAll sibling @c echoOnce is a
/// draining loop: it returns only once it has served a request or the accept fails, so nothing this
/// arm can do to its own socket ends it. An arm that returns without that close leaves the sibling
/// parked in accept() for ever, which turns the case's red into a hang just as surely as a REQUIRE
/// here would (.agent/rules/testing.md).
Task<void> connectAndProbe(EventLoop* loop, core::net::IListener* listener, std::string path, bool* matched)
{
    auto connected = co_await core::net::connectUnix(loop, path);
    if (!connected.has_value())
    {
        listener->close();
        co_return;
    }
    auto sock = std::move(*connected);

    auto const probe = std::string_view { "probe" };
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(probe.data()), probe.size() };
    // Closes the listener here too, and the comment this replaces said why it thought it need not:
    // "the server already accepted; its arm sees this socket close and finishes". It does see the
    // close — and that is not the same as finishing. echoOnce is a DRAINING LOOP: a read of 0 takes
    // its `continue`, not a `co_return`, so it goes straight back into accept() with *served still
    // false and parks there for ever. A sibling's shape, not the peer's observation, is what
    // decides whether an early return is safe.
    if (auto const wrote = co_await sock->write(bytes); !wrote.has_value())
    {
        listener->close();
        co_return;
    }

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

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto listener = core::net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());

    // The hardened directory was created owner-only.
    struct stat info {};
    REQUIRE(::lstat(socketDir.c_str(), &info) == 0);
    REQUIRE((info.st_mode & 0777) == 0700);

    auto served = false;
    auto matched = false;
    auto run = [](core::net::IListener* l, EventLoop* lp, std::string p, bool* s, bool* m) -> Task<void> {
        co_await core::async::whenAll(echoOnce(l, s), connectAndProbe(lp, l, std::move(p), m));
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, socketPath);
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

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto first = core::net::listenUnix(loop, socketPath);
    REQUIRE(first.has_value());

    // The path is live: a second bind is refused with AddressInUse, and the
    // incumbent's socket file is left intact.
    auto second = core::net::listenUnix(loop, socketPath);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().code == core::net::NetErrorCode::AddressInUse);
    REQUIRE(std::filesystem::exists(socketPath));

    // The first listener still serves afterwards: a client connects and gets its
    // probe echoed back. (The refused bind's liveness probe left a dropped
    // connection queued, which echoOnce drains before the real request.)
    auto served = false;
    auto matched = false;
    auto run = [](core::net::IListener* l, EventLoop* lp, std::string p, bool* s, bool* m) -> Task<void> {
        co_await core::async::whenAll(echoOnce(l, s), connectAndProbe(lp, l, std::move(p), m));
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

    auto const check = core::net::ensureOwnedPrivateDirectory(socketDir);
    REQUIRE_FALSE(check.has_value());

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, (socketDir / "default").string());
    REQUIRE_FALSE(listener.has_value());
}

TEST_CASE("a group-accessible socket directory is permitted, mirroring tmux", "[net][unix]")
{
    // tmux's TMUX_SOCK_PERM masks only o+rwx: group bits do not disqualify a
    // directory. Mirror that exactly rather than inventing a stricter policy.
    auto const tmp = TempDir {};
    auto const socketDir = tmp.path / "grouped";
    REQUIRE(::mkdir(socketDir.c_str(), 0770) == 0);

    REQUIRE(core::net::ensureOwnedPrivateDirectory(socketDir).has_value());
}

TEST_CASE("a socket path with no directory component binds in the current directory", "[net][unix]")
{
    // `--socket=contour.sock` names a socket in the CURRENT directory, whose parent_path() is empty
    // — and `mkdir("")` fails with ENOENT, not EEXIST, so hardening it unconditionally refused the
    // path before bind() was ever attempted, with a diagnostic naming a directory operation the
    // user never asked for. Nor is the working directory ours to create or re-permission: it is the
    // caller's own and routinely group- or world-readable.
    auto const tmp = TempDir {};
    auto const cwd = ScopedWorkingDirectory { tmp.path };

    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto listener = core::net::listenUnix(loop, "relative.sock");
    REQUIRE(listener.has_value());

    // The socket file itself still carries the owner-only mode that gates access to it — the
    // directory check the empty parent skips was never what protected the socket.
    struct stat info {};
    REQUIRE(::lstat("relative.sock", &info) == 0);
    CHECK((info.st_mode & (S_IRWXG | S_IRWXO)) == 0);

    listener->reset(); // unlinks the socket before the temp directory goes away
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

    REQUIRE_FALSE(core::net::ensureOwnedPrivateDirectory(filePath).has_value());
}

TEST_CASE("connectUnix to a missing socket reports connection refused", "[net][unix]")
{
    auto const tmp = TempDir {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };

    auto failed = false;
    auto tryConnect = [](EventLoop* lp, std::string p, bool* out) -> Task<void> {
        auto connected = co_await core::net::connectUnix(lp, std::move(p));
        *out = !connected.has_value();
    };
    loop.blockOn(tryConnect(&loop, (tmp.path / "nothing-here").string(), &failed));

    REQUIRE(failed);
}

TEST_CASE("makeStreamSocket hands back a non-blocking, close-on-exec descriptor", "[net][unix]")
{
    // The helper both listeners now create their socket with, rather than a bare ::socket()
    // followed by fcntl: on Linux the flags come from socket(2) itself, so there is no window
    // in which a fork+exec from another thread inherits a listening descriptor and keeps the
    // port — or the socket file — claimed after this process exits. What is assertable from
    // here is the outcome the listeners depend on; the window itself is only visible to a
    // concurrent exec.
    auto const fd = core::net::makeStreamSocket(AF_UNIX, 0);
    REQUIRE(fd >= 0);

    auto const status = ::fcntl(fd, F_GETFL, 0);
    auto const descriptorFlags = ::fcntl(fd, F_GETFD, 0);
    ::close(fd);

    REQUIRE(status >= 0);
    REQUIRE(descriptorFlags >= 0);
    CHECK((status & O_NONBLOCK) != 0);
    CHECK((descriptorFlags & FD_CLOEXEC) != 0);
}
