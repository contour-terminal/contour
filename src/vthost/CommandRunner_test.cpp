// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/CommandRunner.hpp>

#ifndef _WIN32

TEST_CASE("the native runner captures standard output and the exit code", "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();

    auto const printed = runner->run(
        std::vector<std::string> { "sh", "-c", "printf 'a=1\\nb=2\\n'; echo ignored >&2; exit 3" });
    REQUIRE(printed.has_value());
    CHECK(printed->exitCode == 3);
    CHECK(printed->standardOutput == "a=1\nb=2\n");
}

// [vfork]: posix_spawn learns of a failed exec through memory it shares with its vfork'd child. valgrind runs
// that child as a fork, so the spawn reports success, and the child exits 127 instead.
TEST_CASE("the native runner reports a program that is not installed", "[vthost][service][command][vfork]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const result = runner->run(std::vector<std::string> { "contour-no-such-program-here" });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("the native runner reports a command killed by a signal as 128 plus the signal",
          "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const result = runner->run(std::vector<std::string> { "sh", "-c", "kill -TERM $$" });
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 128 + 15);
}

TEST_CASE("a native runner given an environment runs under it, and looks its program up on its PATH",
          "[vthost][service][command]")
{
    // The GUI runs systemctl on the attach's worker while its main thread still changes the environment:
    // the command is found on the snapshot's PATH, and runs under the snapshot alone.
    auto const directory = core::testing::ScopedTempDir { "contour-runner" };
    auto const probe = directory.path() / "contour-runner-probe";
    std::ofstream { probe } << "#!/bin/sh\nprintf '%s|%s' \"$ONLY_GIVEN\" \"${HOME-unset}\"\n";
    std::filesystem::permissions(probe, std::filesystem::perms::owner_all);
    auto const runner = vthost::makeNativeCommandRunner(
        std::vector<std::string> { std::format("PATH={}", directory.path().string()), "ONLY_GIVEN=yes" });

    auto const printed = runner->run(std::vector<std::string> { "contour-runner-probe" });
    REQUIRE(printed.has_value());
    CHECK(printed->exitCode == 0);
    CHECK(printed->standardOutput == "yes|unset");

    // Not on the given PATH, though it is on this process's.
    auto const missing = runner->run(std::vector<std::string> { "sh", "-c", "exit 0" });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == std::errc::no_such_file_or_directory);
}

    #ifdef __linux__
namespace
{
/// glibc's internal signals 32 and 33, as bits of /proc's signal masks. Under valgrind a program
/// posix_spawn() starts finds them ignored with no Contour code in the way: a plain C posix_spawn() of the
/// same grep shows "SigIgn: 0000000180000000" there, and nothing natively.
constexpr auto GlibcInternalSignals = std::uint64_t { 0x1'8000'0000 };

/// @return Whether this process runs under valgrind, which preloads its core into the program it runs.
[[nodiscard]] bool underValgrind()
{
    auto const preload = core::defaultEnvironment().get("LD_PRELOAD");
    return preload && preload->contains("vgpreload");
}

/// @return The mask on the line of /proc/PID/status that @p field names, as /proc prints it in hex.
[[nodiscard]] std::uint64_t maskOf(std::string_view status, std::string_view field)
{
    auto const at = status.find(field);
    REQUIRE(at != std::string_view::npos);
    auto const digits = status.substr(at + field.size());
    auto mask = std::uint64_t {};
    REQUIRE(std::from_chars(digits.data(), digits.data() + digits.size(), mask, 16).ec == std::errc {});
    return mask;
}
} // namespace

TEST_CASE("a command starts with no signal blocked or ignored", "[vthost][service][command]")
{
    // The daemon's threads block SIGTERM and SIGINT, and the GUI ignores SIGPIPE; neither carries over.
    auto blocked = sigset_t {};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    auto previousMask = sigset_t {};
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, &previousMask) == 0);
    auto* const previousUsr1 = ::signal(SIGUSR1, SIG_IGN);

    auto const runner = vthost::makeNativeCommandRunner();
    auto const status =
        runner->run(std::vector<std::string> { "grep", "-E", "^Sig(Blk|Ign):", "/proc/self/status" });

    ::signal(SIGUSR1, previousUsr1);
    ::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    REQUIRE(status.has_value());
    CHECK(maskOf(status->standardOutput, "SigBlk:\t") == 0);
    auto const valgrinds = underValgrind() ? GlibcInternalSignals : std::uint64_t { 0 };
    CHECK((maskOf(status->standardOutput, "SigIgn:\t") & ~valgrinds) == 0);
}
    #endif

TEST_CASE("a command that outlives its deadline is killed and reported as timed out",
          "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const started = std::chrono::steady_clock::now();
    auto const result =
        runner->runWithin(std::vector<std::string> { "sleep", "10" }, std::chrono::milliseconds { 200 });
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::timed_out);
    CHECK(took < std::chrono::seconds { 5 });

    // One that finishes in time is answered as run() would.
    auto const quick =
        runner->runWithin(std::vector<std::string> { "sh", "-c", "printf ok" }, std::chrono::seconds { 10 });
    REQUIRE(quick.has_value());
    CHECK(quick->standardOutput == "ok");
}

TEST_CASE("a command that closes its output and then hangs is killed at the deadline",
          "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const result = runner->runWithin(std::vector<std::string> { "sh", "-c", "exec >&-; sleep 10" },
                                          std::chrono::milliseconds { 200 });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::timed_out);
}

TEST_CASE("the native runner refuses an empty command", "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    CHECK_FALSE(runner->run({}).has_value());
}

#endif
