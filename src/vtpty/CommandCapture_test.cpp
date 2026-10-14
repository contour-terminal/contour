// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandCapture.hpp>

#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <csignal>

    #include <pthread.h>
    #include <unistd.h>
#endif

using namespace std::chrono_literals;
using vtpty::CommandCaptureError;
using vtpty::SystemCommandCapture;

namespace
{
auto systemCapture(core::async::StopToken shutdown = {})
{
    return SystemCommandCapture { SystemCommandCapture::defaultShell(core::defaultEnvironment()),
                                  std::move(shutdown) };
}

// A command that runs far longer than any timeout used below, in this platform's shell.
#ifdef _WIN32
constexpr auto LongRunning = "ping -n 30 127.0.0.1 >nul";
constexpr auto EndlessOutput = "for /L %i in (1,0,2) do @echo OUTPUT";
#else
constexpr auto LongRunning = "sleep 30";
constexpr auto EndlessOutput = "while :; do echo OUTPUT; done";
#endif

/// A scratch directory unique to this instance (process id and a counter), so tests and concurrent
/// test processes never share one. It is removed on destruction on a best-effort basis: neither
/// creating nor removing it ever throws, since a failed cleanup must not abort the test binary.
class ScratchDir
{
  public:
    ScratchDir(): _path { uniquePath() }
    {
        auto ec = std::error_code {};
        std::filesystem::create_directories(_path, ec);
    }
    ScratchDir(ScratchDir const&) = delete;
    ScratchDir& operator=(ScratchDir const&) = delete;
    ~ScratchDir()
    {
        auto ec = std::error_code {};
        std::filesystem::remove_all(_path, ec);
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    static unsigned long currentProcessId()
    {
#ifdef _WIN32
        return ::GetCurrentProcessId();
#else
        return static_cast<unsigned long>(::getpid());
#endif
    }

    static std::filesystem::path uniquePath()
    {
        static auto counter = std::atomic<unsigned> { 0 };
        return std::filesystem::temp_directory_path()
               / std::format("contour-commandcapture-test-{}-{}", currentProcessId(), counter++);
    }

    std::filesystem::path _path;
};

/// A command that prints @p file.
auto catCommand(std::filesystem::path const& file)
{
#ifdef _WIN32
    return "type \"" + file.string() + "\"";
#else
    return "cat '" + file.string() + "'";
#endif
}

/// A command that, after about three seconds, creates @p marker -- unless it has been killed by then.
/// The delay dwarfs the run timeouts used with it, so a slow machine cannot make a kill miss the marker.
auto markerAfterDelay(std::filesystem::path const& marker)
{
#ifdef _WIN32
    return "ping -n 4 127.0.0.1 >nul & echo x > \"" + marker.string() + "\"";
#else
    return "sleep 3; echo x > '" + marker.string() + "'";
#endif
}

/// Comfortably longer than markerAfterDelay's delay: a surviving marker command has written by then.
constexpr auto MarkerWait = 5s;
} // namespace

TEST_CASE("firstLineOf", "[commandcapture]")
{
    CHECK(vtpty::firstLineOf("").empty());
    CHECK(vtpty::firstLineOf("OK") == "OK");
    CHECK(vtpty::firstLineOf("OK\n") == "OK");
    CHECK(vtpty::firstLineOf("CRLF\r\nSECOND") == "CRLF");
    CHECK(vtpty::firstLineOf("FINAL\r") == "FINAL");
    CHECK(vtpty::firstLineOf("BARE\rRETURN") == "BARE\rRETURN");
}

TEST_CASE("SystemCommandCapture.firstLineOfStdout", "[commandcapture]")
{
    auto capture = systemCapture();
    CHECK(capture.run("echo OK", 5s, {}) == "OK");
#ifdef _WIN32
    CHECK(capture.run("echo FIRST& echo SECOND", 5s, {}) == "FIRST");
    CHECK(capture.run("echo \"quoted text\"", 5s, {}) == "\"quoted text\"");
#else
    CHECK(capture.run("echo FIRST; echo SECOND", 5s, {}) == "FIRST");
    CHECK(capture.run("echo \"quoted text\"", 5s, {}) == "quoted text");
#endif
}

TEST_CASE("SystemCommandCapture.ignoresExitStatusAndStderr", "[commandcapture]")
{
    auto capture = systemCapture();
#ifdef _WIN32
    CHECK(capture.run("echo OUT& exit /b 7", 5s, {}) == "OUT");
    CHECK(capture.run("echo ERROR 1>&2", 5s, {}) == "");
    CHECK(capture.run("(echo ERROR 1>&2) 2>&1", 5s, {}) == "ERROR ");
#else
    CHECK(capture.run("echo OUT; exit 7", 5s, {}) == "OUT");
    CHECK(capture.run("echo ERROR 1>&2", 5s, {}) == "");
    CHECK(capture.run("(echo ERROR 1>&2) 2>&1", 5s, {}) == "ERROR");
#endif
}

TEST_CASE("SystemCommandCapture.drainsLargeOutput", "[commandcapture]")
{
    // Far more than a pipe buffer holds, produced by one process rather than a shell loop whose speed
    // depends on the machine.
    auto const scratch = ScratchDir {};
    auto const file = scratch.path() / "lines.txt";
    {
        auto out = std::ofstream { file, std::ios::binary };
        for (auto const line: std::views::iota(0, 20000))
            out << "OUTPUT" << (line % 2 == 0 ? "\n" : "\r\n");
    }
    auto capture = systemCapture();
    CHECK(capture.run(catCommand(file), 30s, {}) == "OUTPUT");
}

TEST_CASE("SystemCommandCapture.drainsMegabytesQuickly", "[commandcapture]")
{
    auto const scratch = ScratchDir {};
    auto const file = scratch.path() / "big.txt";
    {
        auto out = std::ofstream { file, std::ios::binary };
        out << "FIRST\n" << std::string(std::size_t { 8 } * 1024 * 1024, 'x') << '\n';
    }
    auto const command = catCommand(file);
    auto capture = systemCapture();
    auto const start = std::chrono::steady_clock::now();
    CHECK(capture.run(command, 20s, {}) == "FIRST");
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("SystemCommandCapture.timesOut", "[commandcapture]")
{
    auto capture = systemCapture();
    auto const start = std::chrono::steady_clock::now();
    auto const result = capture.run(LongRunning, 500ms, {});
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::TimedOut);
    CHECK(elapsed < 5s);
}

TEST_CASE("SystemCommandCapture.timesOutDespiteContinuousOutput", "[commandcapture]")
{
    // A producer that never pauses must not starve the deadline check.
    auto capture = systemCapture();
    auto const start = std::chrono::steady_clock::now();
    auto const result = capture.run(EndlessOutput, 500ms, {});
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::TimedOut);
    CHECK(elapsed < 5s);
}

TEST_CASE("SystemCommandCapture.timeoutKillsTheCommand", "[commandcapture]")
{
    auto const scratch = ScratchDir {};
    auto const marker = scratch.path() / "marker.txt";
    auto capture = systemCapture();
    auto const result = capture.run(markerAfterDelay(marker), 500ms, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::TimedOut);
    std::this_thread::sleep_for(MarkerWait);
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("SystemCommandCapture.stopRequestCancels", "[commandcapture]")
{
    auto capture = systemCapture();
    auto source = core::async::StopSource {};
    auto stopper = std::thread { [&] {
        std::this_thread::sleep_for(200ms);
        source.request_stop();
    } };
    auto const start = std::chrono::steady_clock::now();
    auto const result = capture.run(LongRunning, 20s, source.get_token());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::Cancelled);
    CHECK(elapsed < 5s);
}

TEST_CASE("SystemCommandCapture.shutdownCancelsARunInFlight", "[commandcapture]")
{
    auto shutdown = core::async::StopSource {};
    auto capture = systemCapture(shutdown.get_token());
    auto stopper = std::thread { [&] {
        std::this_thread::sleep_for(200ms);
        shutdown.request_stop();
    } };
    auto const start = std::chrono::steady_clock::now();
    auto const result = capture.run(LongRunning, 20s, {});
    auto const elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::Cancelled);
    CHECK(elapsed < 5s);
}

TEST_CASE("SystemCommandCapture.runAfterShutdownIsCancelledAtOnce", "[commandcapture]")
{
    auto const scratch = ScratchDir {};
    auto const marker = scratch.path() / "marker.txt";
    auto shutdown = core::async::StopSource {};
    shutdown.request_stop();
    auto capture = systemCapture(shutdown.get_token());
    auto const start = std::chrono::steady_clock::now();
    auto const result = capture.run(markerAfterDelay(marker), 20s, {});
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::Cancelled);
    CHECK(elapsed < 1s);
    std::this_thread::sleep_for(MarkerWait);
    CHECK_FALSE(std::filesystem::exists(marker)); // nothing was spawned
}

TEST_CASE("SystemCommandCapture.spawnFailure", "[commandcapture]")
{
    auto capture = SystemCommandCapture { "this-shell-does-not-exist-7f3a", {} };
    auto const result = capture.run("echo OK", 5s, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::SpawnFailed);
    CHECK(result.error().cause);
}

#ifndef _WIN32
TEST_CASE("SystemCommandCapture.unixTimeoutKillsBackgroundChildren", "[commandcapture]")
{
    // The shell exits at once; the backgrounded subshell inherits stdout, holds the pipe open and would
    // write the marker after three seconds -- unless the timeout killed its whole process group.
    auto const scratch = ScratchDir {};
    auto const marker = scratch.path() / "marker.txt";
    auto capture = systemCapture();
    auto const result = capture.run("(" + markerAfterDelay(marker) + ") & echo STARTED", 500ms, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::TimedOut);
    std::this_thread::sleep_for(MarkerWait);
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("SystemCommandCapture.unixSuccessKillsBackgroundChildren", "[commandcapture]")
{
    // The backgrounded subshell has no stdout, so the run succeeds when the shell exits; the survivor
    // must still be killed, or it writes the marker after three seconds.
    auto const scratch = ScratchDir {};
    auto const marker = scratch.path() / "marker.txt";
    auto capture = systemCapture();
    CHECK(capture.run("(" + markerAfterDelay(marker) + ") >/dev/null & echo STARTED", 5s, {}) == "STARTED");
    std::this_thread::sleep_for(MarkerWait);
    CHECK_FALSE(std::filesystem::exists(marker));
}

namespace
{
/// Blocks one signal in the calling thread and restores the previous mask on destruction.
class ScopedSignalBlock
{
  public:
    explicit ScopedSignalBlock(int signo) noexcept
    {
        auto blocked = sigset_t {};
        sigemptyset(&blocked);
        sigaddset(&blocked, signo);
        pthread_sigmask(SIG_BLOCK, &blocked, &_previous);
    }
    ScopedSignalBlock(ScopedSignalBlock const&) = delete;
    ScopedSignalBlock& operator=(ScopedSignalBlock const&) = delete;
    ScopedSignalBlock(ScopedSignalBlock&&) = delete;
    ScopedSignalBlock& operator=(ScopedSignalBlock&&) = delete;
    ~ScopedSignalBlock() { pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }

  private:
    sigset_t _previous {};
};
} // namespace

TEST_CASE("SystemCommandCapture.unixSpawnedShellDoesNotInheritBlockedSignals", "[commandcapture]")
{
    // The status-line pool threads block every signal, and a posix_spawn child inherits the calling
    // thread's mask. If the shell kept SIGTERM blocked it would survive its own `kill -TERM $$`.
    auto const block = ScopedSignalBlock { SIGTERM };
    auto capture = systemCapture();
    auto const result = capture.run("kill -TERM $$; echo alive", 5s, {});
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

namespace
{
/// Ignores one signal in the calling process and restores the previous disposition on destruction.
class ScopedSignalIgnore
{
  public:
    explicit ScopedSignalIgnore(int signo) noexcept: _signo { signo }
    {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(_signo, &ignore, &_previous);
    }
    ScopedSignalIgnore(ScopedSignalIgnore const&) = delete;
    ScopedSignalIgnore& operator=(ScopedSignalIgnore const&) = delete;
    ScopedSignalIgnore(ScopedSignalIgnore&&) = delete;
    ScopedSignalIgnore& operator=(ScopedSignalIgnore&&) = delete;
    ~ScopedSignalIgnore() { sigaction(_signo, &_previous, nullptr); }

  private:
    int _signo;
    struct sigaction _previous {};
};
} // namespace

TEST_CASE("SystemCommandCapture.unixSpawnedShellDoesNotInheritIgnoredSignals", "[commandcapture]")
{
    // SIG_IGN survives exec, so a shell spawned from a process ignoring SIGTERM would shrug off its own
    // `kill -TERM $$` unless the spawn resets the disposition to the default.
    auto const ignore = ScopedSignalIgnore { SIGTERM };
    auto capture = systemCapture();
    auto const result = capture.run("kill -TERM $$; echo alive", 5s, {});
    REQUIRE(result.has_value());
    CHECK(result->empty());
}
#endif

#ifdef _WIN32
TEST_CASE("SystemCommandCapture.windowsUtf8CommandLine", "[commandcapture]")
{
    // cmd.exe echoes in the console code page, so compare by effect: the command must name a file
    // whose name is non-ASCII, which it only finds if the command line reached it as UTF-16 intact.
    auto const scratch = ScratchDir {};
    auto const file = scratch.path() / std::filesystem::path { u8"ü.txt" };
    std::ofstream { file } << "x";
    auto capture = systemCapture();
    auto const utf8Path = file.u8string();
    auto const command = "if exist \"" + std::string { utf8Path.begin(), utf8Path.end() } + "\" echo FOUND";
    CHECK(capture.run(command, 5s, {}) == "FOUND");
}

TEST_CASE("SystemCommandCapture.windowsGrandchildrenDie", "[commandcapture]")
{
    // cmd.exe exits at once, but the detached grandchild keeps the pipe open and would write the
    // marker after about three seconds (a time-based delay, independent of CPU speed) -- unless the job
    // object killed it when the run timed out.
    auto const scratch = ScratchDir {};
    auto const marker = scratch.path() / "marker.txt";
    auto const script = scratch.path() / "grandchild.cmd";
    {
        auto out = std::ofstream { script };
        out << "@ping -n 4 127.0.0.1 >nul\n" << R"(@echo x > ")" << marker.string() << "\"\n";
    }
    auto const command = R"(start /b "" ")" + script.string() + "\"";
    auto capture = systemCapture();
    auto const result = capture.run(command, 500ms, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == CommandCaptureError::Kind::TimedOut);
    std::this_thread::sleep_for(MarkerWait);
    CHECK_FALSE(std::filesystem::exists(marker));
}
#endif
