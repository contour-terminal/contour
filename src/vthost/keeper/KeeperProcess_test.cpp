// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <format>
#include <random>
#include <system_error>
#include <thread>

#include <unistd.h>

#include <vthost/keeper/KeeperProcess.hpp>
#include <vthost/keeper/PosixPorts.hpp>

using namespace vthost::keeper;

namespace
{

/// A directory of its own, removed with this.
struct ScratchDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path()
                                 / std::format("contour-keeper-process-{}", std::random_device {}());

    ScratchDirectory() { std::filesystem::create_directories(path); }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(path, ignored);
    }
};

/// What no daemon is ever started from: a keeper that gets as far as launching one fails the test.
KeeperProcessConfig neverLaunched(std::filesystem::path socket)
{
    return KeeperProcessConfig { .daemonImage = "/nonexistent/contour",
                                 .daemonArguments = {},
                                 .environment = {},
                                 .socketPath = std::move(socket),
                                 .launchOptions = {},
                                 .policy = {} };
}

} // namespace

#ifdef __linux__
TEST_CASE("the running threads are counted", "[keeper][posix]")
{
    auto const alone = runningThreadCount();
    REQUIRE(alone.has_value());
    auto release = std::atomic<bool> { false };
    auto second = std::thread { [&] {
        while (!release.load())
            std::this_thread::yield();
    } };
    // At least one more, not exactly one: a sanitizer runtime may start a helper thread of its own along
    // with the process's first (ThreadSanitizer's background thread does).
    auto const together = runningThreadCount();
    REQUIRE(together.has_value());
    CHECK(*together > *alone);
    release.store(true);
    second.join();
}

TEST_CASE("a keeper beside a second thread does not start", "[keeper][posix]")
{
    auto const scratch = ScratchDirectory {};
    auto release = std::atomic<bool> { false };
    auto second = std::thread { [&] {
        while (!release.load())
            std::this_thread::yield();
    } };
    auto const ran = runKeeperProcess(neverLaunched(scratch.path / "sock"));
    release.store(true);
    second.join();
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error() == KeeperStartError::MultiThreaded);
    // It failed before the label: nothing was locked.
    CHECK_FALSE(std::filesystem::exists(labelLockPath(scratch.path / "sock")));
}
#endif

TEST_CASE("a keeper on a held label does not start", "[keeper][posix]")
{
    // The keeper checks its threads before its label. ThreadSanitizer's helper thread, started with an
    // earlier test case's thread and restarted in every fork, keeps this process past one for good.
    if (auto const threads = runningThreadCount(); threads && *threads != 1)
        SKIP("this process runs more than one thread: a sanitizer runtime's helper thread");
    auto const scratch = ScratchDirectory {};
    auto const socket = scratch.path / "sock";
    auto const held = lockLabel(socket);
    REQUIRE(held.has_value());
    auto const ran = runKeeperProcess(neverLaunched(socket));
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error() == KeeperStartError::LabelHeld);
}

TEST_CASE("the keeper ignores SIGPIPE, so a log line into a closed pipe cannot end it", "[keeper][posix]")
{
    auto const scratch = ScratchDirectory {};
    auto const socket = scratch.path / "sock";
    auto const held = lockLabel(socket);
    REQUIRE(held.has_value());
    auto const previous = ::signal(SIGPIPE, SIG_DFL);
    // It fails to start either way -- on the label, or beside a sanitizer's thread -- and SIGPIPE is ignored
    // first thing, before either check.
    REQUIRE_FALSE(runKeeperProcess(neverLaunched(socket)).has_value());
    auto const disposition = ::signal(SIGPIPE, previous);
    CHECK(disposition == SIG_IGN);
}
