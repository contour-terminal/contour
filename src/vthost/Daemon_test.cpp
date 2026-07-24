// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>
#include <system_error>

#include <net/EventLoop.h>
#include <net/IoResult.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <vthost/Daemon.h>
#include <vthost/SocketPath.h>

namespace
{

namespace fs = std::filesystem;

/// A unique, empty directory under the system temp dir, removed on destruction.
/// Each test gets its own so concurrent runs cannot collide on a socket file.
/// Both components are kept terse on purpose: an AF_UNIX path must fit
/// sun_path's 108 bytes, and the system temp dir already eats much of that.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / std::format("contour-d-{}", std::random_device {}());

    TempDir() { fs::create_directories(path); }

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

} // namespace

TEST_CASE("ensureDaemon: a TCP endpoint is a no-op", "[vthost][daemon]")
{
    // A remote daemon cannot be auto-spawned, so this short-circuits before any
    // probe: the address below is TEST-NET-1 and is deliberately unreachable.
    auto const endpoint = vthost::AttachEndpoint { vthost::TcpEndpoint {
        .host = "192.0.2.1", .port = 1, .token = {}, .caPem = {} } };
    REQUIRE(vthost::ensureDaemon(endpoint, "contour", std::chrono::seconds { 0 }) == EXIT_SUCCESS);
}

TEST_CASE("ensureDaemon: a listening daemon is detected, nothing is spawned", "[vthost][daemon]")
{
    // The probe is the AF_UNIX connect that must work on Windows (10 1803+) as
    // well as POSIX; a runtime SKIP documents platforms lacking AF_UNIX entirely.
    auto const tmp = TempDir {};
    auto const controlSocket = tmp.path / "control";

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    // Stand in for a running daemon by binding exactly the endpoint the probe
    // looks for. No accept loop is needed: reaching the listen backlog is the
    // whole liveness signal ensureDaemon asks for.
    auto listener = net::listenUnix(loop, vthost::nativeSocketPath(controlSocket).string());
    if (!listener.has_value())
    {
        REQUIRE(listener.error().code == net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    // The binary name is bogus on purpose: taking the spawn path at all is the
    // failure this guards against, and a zero timeout leaves no room to recover.
    auto const endpoint = vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = controlSocket } };
    REQUIRE(vthost::ensureDaemon(endpoint, "contour-no-such-binary", std::chrono::seconds { 0 })
            == EXIT_SUCCESS);
}

TEST_CASE("ensureDaemon: an unreachable socket fails once the timeout elapses", "[vthost][daemon]")
{
    // Nothing is bound, so the probe must report the daemon down. The spawn that
    // follows cannot succeed (no such binary), and the zero timeout ends the poll
    // immediately — so this also pins that a failed spawn cannot report success.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    REQUIRE(vthost::ensureDaemon(endpoint, "contour-no-such-binary", std::chrono::seconds { 0 })
            == EXIT_FAILURE);
}
