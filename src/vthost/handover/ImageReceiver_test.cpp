// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/ImageReceiver.hpp>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <array>
    #include <chrono>
    #include <csignal>
    #include <thread>
    #include <tuple>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vthost/handover/HandoverCodec.hpp>
#endif

using namespace vthost::handover;

namespace
{
// RefusedHere: what a daemon named a handover it cannot take over does on this platform.
#ifdef _WIN32
constexpr auto RefusedHere = TakeoverFailure::EndWithoutServing;
#else
constexpr auto RefusedHere = TakeoverFailure::StartAfresh;
#endif

/// @return A token only the other platform's receiver takes.
[[nodiscard]] std::string foreignToken()
{
#ifdef _WIN32
    return handoverToken(vtpty::CrossingHandle { 3 });
#else
    return handoverToken(SuccessorHandles { .image = vtpty::CrossingHandle { 1 },
                                            .ready = vtpty::CrossingHandle { 2 },
                                            .commit = vtpty::CrossingHandle { 3 },
                                            .predecessor = vtpty::CrossingHandle { 4 } });
#endif
}
} // namespace

TEST_CASE("no token starts afresh without failing", "[vthost][handover]")
{
    auto const memory = makeNativeHandoverMemory();
    auto const taken = makeNativeImageReceiver(*memory)->takeOver("");
    REQUIRE(taken.has_value());
    CHECK(*taken == std::nullopt);
}

TEST_CASE("a token for the other platform is refused", "[vthost][handover]")
{
    auto const memory = makeNativeHandoverMemory();
    auto const taken = makeNativeImageReceiver(*memory)->takeOver(foreignToken());
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == RefusedHere);
}

TEST_CASE("a malformed token is refused", "[vthost][handover]")
{
    auto const memory = makeNativeHandoverMemory();
    auto const taken = makeNativeImageReceiver(*memory)->takeOver("x");
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == RefusedHere);
}

#ifndef _WIN32
namespace
{
/// How long the child that takes an image over is waited for, and how often it is looked at meanwhile.
constexpr auto ChildDeadline = std::chrono::seconds { 30 };
constexpr auto ChildPollInterval = std::chrono::milliseconds { 10 };

/// What the child that takes an image over tells its parent, one byte through a pipe.
enum class TakeoverOutcome : char
{
    TakenOver = 'y', ///< The image it took over is the one written.
    Failed = 'n',    ///< It took none over, or another one.
};

/// @return The exit status of @p child once it ended; nullopt where it did not end by @p deadline, after
/// which
///         it is killed and reaped.
[[nodiscard]] std::optional<int> waitFor(pid_t child, std::chrono::steady_clock::time_point deadline)
{
    auto status = 0;
    while (::waitpid(child, &status, WNOHANG) == 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ::kill(child, SIGKILL);
            std::ignore = ::waitpid(child, &status, 0);
            return std::nullopt;
        }
        std::this_thread::sleep_for(ChildPollInterval);
    }
    return status;
}
} // namespace

TEST_CASE("an image written to memory is taken over", "[vthost][handover]")
{
    auto image = HandoverImage {};
    image.daemonInstance = 0xBEEF;
    image.fromRelease = "0.9.0";

    // Everything is prepared before the fork: the child of this multithreaded binary runs as little as it
    // can, which is the takeover itself, and tells the outcome through a pipe.
    auto const memory = makeNativeHandoverMemory();
    auto const handle = memory->write(encodeHandover(image));
    REQUIRE(handle.has_value());
    auto const token = handoverToken(*handle);
    auto const receiver = makeNativeImageReceiver(*memory);
    auto outcome = std::array<int, 2> {};
    REQUIRE(::pipe(outcome.data()) == 0);

    // Run in a child: taking an image over closes every descriptor it does not name, and keeps the standard
    // streams, so the pipe is the child's standard output.
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        if (::dup2(outcome[1], STDOUT_FILENO) == -1)
            ::_exit(2);
        auto const taken = receiver->takeOver(token);
        auto const told = taken && *taken && (*taken)->daemonInstance == image.daemonInstance
                                  && (*taken)->fromRelease == image.fromRelease
                              ? TakeoverOutcome::TakenOver
                              : TakeoverOutcome::Failed;
        ::_exit(::write(STDOUT_FILENO, &told, 1) == 1 ? 0 : 3);
    }
    ::close(outcome[1]);
    ::close(handle->native());

    auto const status = waitFor(child, std::chrono::steady_clock::now() + ChildDeadline);
    auto told = TakeoverOutcome::Failed;
    auto const read = ::read(outcome[0], &told, 1);
    ::close(outcome[0]);
    if (!status)
        FAIL("the child taking the image over did not end in time");
    CHECK(WIFEXITED(*status));
    CHECK(WEXITSTATUS(*status) == 0);
    CHECK(read == 1);
    CHECK(told == TakeoverOutcome::TakenOver);
}

namespace
{
/// Takes over @p token behind a keeper in a child, the descriptors the old image left beside the keeper's
/// channel and @p named.
/// @return Whether it started afresh with the channel alone kept open, @p named and the leftover closed.
[[nodiscard]] bool startsAfreshKeepingTheChannelAlone(std::string const& token, int named)
{
    auto const memory = makeNativeHandoverMemory();
    auto leftover = std::array<int, 2> {}; // what the old image left: a program's PTY, say
    REQUIRE(::pipe(leftover.data()) == 0);
    auto channel = std::array<int, 2> {}; // the keeper's channel, kept across
    REQUIRE(::pipe(channel.data()) == 0);
    auto outcome = std::array<int, 2> {};
    REQUIRE(::pipe(outcome.data()) == 0);
    auto const receiver = makeNativeImageReceiver(*memory, { channel[1] }, Leftovers::Closed);

    // In a child: the takeover closes descriptors of the process it runs in.
    auto const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        if (::dup2(outcome[1], STDOUT_FILENO) == -1)
            ::_exit(2);
        auto const taken = receiver->takeOver(token);
        auto const isOpen = [](int fd) {
            return ::fcntl(fd, F_GETFD) != -1;
        };
        auto const told = !taken && taken.error() == TakeoverFailure::StartAfresh && isOpen(channel[1])
                                  && !isOpen(leftover[0]) && (named < 0 || !isOpen(named))
                              ? TakeoverOutcome::TakenOver
                              : TakeoverOutcome::Failed;
        ::_exit(::write(STDOUT_FILENO, &told, 1) == 1 ? 0 : 3);
    }
    for (auto const fd: { leftover[0], leftover[1], channel[0], channel[1], outcome[1] })
        ::close(fd);

    auto const status = waitFor(child, std::chrono::steady_clock::now() + ChildDeadline);
    auto told = TakeoverOutcome::Failed;
    auto const read = ::read(outcome[0], &told, 1);
    ::close(outcome[0]);
    if (!status)
        FAIL("the child taking the image over did not end in time");
    return WIFEXITED(*status) && WEXITSTATUS(*status) == 0 && read == 1 && told == TakeoverOutcome::TakenOver;
}
} // namespace

TEST_CASE("behind a keeper, an image that cannot be read leaves nothing open but what is kept",
          "[vthost][handover][keeper]")
{
    // Named by the token, but no memory object: a pipe, whose image cannot be read.
    auto unreadable = std::array<int, 2> {};
    REQUIRE(::pipe(unreadable.data()) == 0);
    auto const token = handoverToken(vtpty::CrossingHandle { unreadable[0] });
    CHECK(startsAfreshKeepingTheChannelAlone(token, unreadable[0]));
    ::close(unreadable[0]);
    ::close(unreadable[1]);
}

TEST_CASE("behind a keeper, a token that names no descriptor leaves nothing open but what is kept",
          "[vthost][handover][keeper]")
{
    CHECK(startsAfreshKeepingTheChannelAlone("x", -1));
}

#endif
