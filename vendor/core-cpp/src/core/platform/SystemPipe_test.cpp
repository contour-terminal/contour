// SPDX-License-Identifier: Apache-2.0

// On Windows, winsock2.h MUST precede windows.h, so this block comes before any other include.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <thread>
#include <utility>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>
#endif

using core::platform::ChannelResult;
using core::platform::createSystemPipe;
using core::platform::InvalidHandle;
using core::platform::PlatformError;
using core::platform::SystemPipe;

namespace
{

// The three results a read can produce are distinct; bytes(0) is no fourth one.
static_assert(ChannelResult {}.empty() && !ChannelResult {}.isEndOfStream());
static_assert(ChannelResult::bytes(0) == ChannelResult {});
static_assert(!ChannelResult::bytes(3).empty() && !ChannelResult::bytes(3).isEndOfStream()
              && ChannelResult::bytes(3).bytesRead() == 3);
static_assert(ChannelResult::endOfStream().isEndOfStream() && !ChannelResult::endOfStream().empty()
              && ChannelResult::endOfStream().bytesRead() == 0);

/// Waits up to a second for the channel's read end to signal readiness.
/// @return True if it did.
[[nodiscard]] bool waitReadable(SystemPipe const& pipe)
{
#ifdef _WIN32
    return WaitForSingleObject(pipe.waitHandle(), 1000) == WAIT_OBJECT_0;
#else
    auto descriptor = pollfd { .fd = pipe.waitHandle(), .events = POLLIN, .revents = 0 };
    return ::poll(&descriptor, 1, 1000) == 1;
#endif
}

/// Reads, and while the channel is still empty, waits for readiness and reads again, a few
/// times: the loopback pair behind a Windows channel delivers bytes, and the writer's close,
/// asynchronously.
[[nodiscard]] std::expected<ChannelResult, PlatformError> readWhenReady(SystemPipe& pipe,
                                                                        std::span<char> buffer)
{
    auto result = pipe.read(buffer.data(), buffer.size());
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 3))
    {
        if (!result || !result->empty() || !waitReadable(pipe))
            break;
        result = pipe.read(buffer.data(), buffer.size());
    }
    return result;
}

/// Closes the channel's write direction, as a writer that is done does, leaving its handle open.
/// @return True on success.
[[nodiscard]] bool closeWriter(SystemPipe const& pipe)
{
#ifdef _WIN32
    return ::shutdown(reinterpret_cast<SOCKET>(pipe.writeFd()), SD_SEND) == 0;
#else
    return ::shutdown(pipe.writeFd(), SHUT_WR) == 0;
#endif
}

/// Makes every further read of the channel fail, leaving the handle the channel closes valid.
/// @return True on success.
[[nodiscard]] bool breakReadEnd(SystemPipe const& pipe)
{
#ifdef _WIN32
    // A socket shut down for receiving refuses every further recv() (WSAESHUTDOWN).
    return ::shutdown(reinterpret_cast<SOCKET>(pipe.readFd()), SD_RECEIVE) == 0;
#else
    // Put a descriptor that is not a socket where the read end was: recv() fails (ENOTSOCK).
    // shutdown(SHUT_RD) would not do, because a POSIX socket reads it as the end of the stream.
    auto const notASocket = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (notASocket < 0)
        return false;
    auto const replaced = ::dup2(notASocket, pipe.readFd()) == pipe.readFd();
    ::close(notASocket);
    return replaced;
#endif
}

} // namespace

TEST_CASE("SystemPipe round-trips bytes between its ends", "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    REQUIRE((*pipe)->good());

    char const payload[] = "hello";
    auto const written = (*pipe)->write(payload, sizeof(payload));
    REQUIRE(written.has_value());
    REQUIRE(*written == sizeof(payload));

    auto buf = std::array<char, sizeof(payload)> {};
    auto const got = readWhenReady(**pipe, buf);
    REQUIRE(got.has_value());
    CHECK(*got == ChannelResult::bytes(sizeof(payload)));
    CHECK(std::memcmp(buf.data(), payload, sizeof(payload)) == 0);
}

TEST_CASE("SystemPipe read of an empty channel is empty rather than blocking", "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
#ifndef _WIN32
    REQUIRE((::fcntl((*pipe)->readFd(), F_GETFL) & O_NONBLOCK) != 0); // else this would block
#endif

    // The writer is still there, so this is not the end of the stream, and not a failure.
    auto buf = std::array<char, 16> {};
    auto const got = (*pipe)->read(buf.data(), buf.size());
    REQUIRE(got.has_value());
    CHECK(got->empty());
    CHECK(!got->isEndOfStream());
}

TEST_CASE("SystemPipe read of zero bytes is empty and leaves the channel alone", "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    char const payload[] = "x";
    REQUIRE((*pipe)->write(payload, 1) == std::size_t { 1 });

    auto buf = std::array<char, 4> {};
    CHECK((*pipe)->read(buf.data(), 0) == ChannelResult {});
    CHECK(readWhenReady(**pipe, buf) == ChannelResult::bytes(1));
}

TEST_CASE("SystemPipe reads the end of the stream once the writer closed and the bytes are read",
          "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    char const payload[] = "bye";
    REQUIRE((*pipe)->write(payload, 3) == std::size_t { 3 });
    REQUIRE(closeWriter(**pipe));

    // The bytes written before the close come first...
    auto buf = std::array<char, 16> {};
    auto const first = readWhenReady(**pipe, buf);
    REQUIRE(first.has_value());
    CHECK(*first == ChannelResult::bytes(3));

    // ...then the end of the stream, which no later read changes.
    auto const second = readWhenReady(**pipe, buf);
    REQUIRE(second.has_value());
    CHECK(second->isEndOfStream());
    CHECK(!second->empty());
    CHECK(readWhenReady(**pipe, buf) == ChannelResult::endOfStream());
}

TEST_CASE("SystemPipe reports a failed read as a PlatformError", "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    REQUIRE(breakReadEnd(**pipe));

    auto buf = std::array<char, 16> {};
    auto const got = (*pipe)->read(buf.data(), buf.size());
    REQUIRE(!got.has_value());
    CHECK(got.error() == PlatformError::IoError);
}

TEST_CASE("SystemPipe exposes a valid wait handle", "[systempipe]")
{
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    REQUIRE((*pipe)->waitHandle() != InvalidHandle);
    REQUIRE((*pipe)->readFd() != InvalidHandle);
    REQUIRE((*pipe)->writeFd() != InvalidHandle);
}

#ifndef _WIN32
TEST_CASE("SystemPipe ends are non-blocking and close-on-exec", "[systempipe]")
{
    // A producer must never stall on a full wakeup pipe, a loop's drain must never park on a
    // spurious readiness, and a child process must not inherit either end.
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());
    for (auto const fd: { (*pipe)->readFd(), (*pipe)->writeFd() })
    {
        CHECK((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    }
}
#endif

TEST_CASE("SystemPipe reports a write into a full channel as done", "[systempipe]")
{
    // Nothing drains the channel here, so the socket buffer fills up. Each further byte would
    // only have signalled a wakeup that is already pending, so the write succeeds instead of
    // blocking the producer or failing it. On Windows only the read socket used to be made
    // non-blocking, so this parked forever -- the never-stall guarantee held on POSIX alone.
    auto created = createSystemPipe();
    REQUIRE(created.has_value());

    // The writes run on a thread of their own against a bounded wait: a write end that still
    // blocks parks in send() and never comes back, and a test that hangs says less than one that
    // fails. Both the channel and the results are shared, so the parked thread keeps what it
    // touches alive.
    auto const pipe = std::shared_ptr<SystemPipe> { std::move(*created) };
    auto const finished = std::make_shared<std::atomic<bool>>(false);
    auto const everyWriteWasDone = std::make_shared<std::atomic<bool>>(true);

    auto writer = std::thread([pipe, finished, everyWriteWasDone] {
        auto const chunk = std::array<char, 4096> {};
        for ([[maybe_unused]] auto const round: std::views::iota(0, 1024)) // 4 MiB, past any buffer
        {
            auto const written = pipe->write(chunk.data(), chunk.size());
            if (!written.has_value() || *written == 0)
            {
                everyWriteWasDone->store(false);
                break;
            }
        }
        finished->store(true);
    });

    constexpr auto Bound = std::chrono::seconds { 10 };
    auto const deadline = std::chrono::steady_clock::now() + Bound;
    while (!finished->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });

    auto const producerReturned = finished->load();
    if (producerReturned)
        writer.join();
    else
        writer.detach(); // Parked in a blocking send(); what it holds outlives it.

    CHECK(producerReturned); // Within 10s: the producer never stalled on a full channel.
    CHECK(everyWriteWasDone->load());
}

#ifdef _WIN32
TEST_CASE("SystemPipe's two ends are connected to each other", "[systempipe]")
{
    // Windows has no socketpair(2), so the pair is a loopback TCP connection: bind, listen,
    // connect, accept. accept() hands back whoever connected, and between the listen() and the
    // accept() any local process can take that ephemeral port -- it is discoverable and the
    // backlog is 1. The two ends would then not be each other's, and every wakeup byte would go
    // to a stranger while the loop waited for one that never comes. This is the property that
    // rules it out; provoking the race itself needs an adversary that knows the port.
    auto pipe = createSystemPipe();
    REQUIRE(pipe.has_value());

    auto const addressOf = [](auto const& query, SOCKET socket) {
        sockaddr_in address {};
        auto length = static_cast<int>(sizeof(address));
        REQUIRE(query(socket, reinterpret_cast<sockaddr*>(&address), &length) != SOCKET_ERROR);
        return address;
    };
    auto const readSock = reinterpret_cast<SOCKET>((*pipe)->readFd());
    auto const writeSock = reinterpret_cast<SOCKET>((*pipe)->writeFd());

    CHECK(addressOf(::getsockname, readSock).sin_port == addressOf(::getpeername, writeSock).sin_port);
    CHECK(addressOf(::getsockname, writeSock).sin_port == addressOf(::getpeername, readSock).sin_port);
}
#endif
