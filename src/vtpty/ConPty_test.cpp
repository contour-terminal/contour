// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Escape.hpp>

#include <gsl/util>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <Windows.h>

using vtpty::ColumnCount;
using vtpty::ConPty;
using vtpty::LineCount;
using vtpty::PageSize;

namespace
{

constexpr PageSize pageSize(int columns, int lines) noexcept
{
    return PageSize { .lines = LineCount(lines), .columns = ColumnCount(columns) };
}

/// What a terminal answers to the two queries a pseudoconsole may send it at startup: primary
/// device attributes and the cursor position. An unanswered one can hold ConPTY's output back.
constexpr auto TerminalAnswers = std::array {
    std::pair { std::string_view { "\x1b[c" }, std::string_view { "\x1b[?61;4c" } },
    std::pair { std::string_view { "\x1b[6n" }, std::string_view { "\x1b[1;1R" } },
};

/// The line the probe prints last, after every mark.
constexpr auto ProbeSentinel = std::string_view { "contour-probe-done" };

/// @return The probe: a console program that writes OSC 133 marks, then ProbeSentinel.
std::filesystem::path probeProgram()
{
    auto probe = std::filesystem::path { VTPTY_CONPTY_OSC133_PROBE };
    probe.make_preferred();
    return probe;
}

/// Runs the probe on a pseudoconsole and reads what a terminal would see of it until the probe's
/// sentinel, answering the pseudoconsole's queries along the way as a terminal would.
/// @return Everything the pseudoconsole wrote.
std::string runProbe()
{
    auto process = vtpty::Process { vtpty::Process::ExecInfo { .program = probeProgram().string(),
                                                               .arguments = {},
                                                               .workingDirectory = {},
                                                               .env = {},
                                                               .removedEnvironment = {} },
                                    vtpty::createPty(pageSize(80, 24), std::nullopt),
                                    /*escapeSandbox=*/false,
                                    std::make_shared<vtpty::NoPlacement>() };
    auto const started = process.start();

    // A read on ConPTY blocks until the pseudoconsole has something to say. Ending the child is what
    // makes it return, should the probe never print its sentinel; ending it on the way out as well
    // means no failed assertion can leave ~Process waiting on a child that is still running.
    auto const watchdog = std::jthread { [&process](std::stop_token const& stop) {
        auto mutex = std::mutex {};
        auto lock = std::unique_lock { mutex };
        auto wakeup = std::condition_variable_any {};
        std::ignore = wakeup.wait_for(lock, stop, std::chrono::seconds { 30 }, [] { return false; });
        process.terminate(vtpty::Process::TerminationHint::Normal);
    } };

    REQUIRE(started.has_value());
    // The probe itself, not the login shell a failed spawn falls back to.
    REQUIRE(started->diagnostic.empty());

    auto answered = std::vector<std::string_view> {};
    return vtpty::testing::readUntil(process, [&](std::string_view text) {
        for (auto const& [query, answer]: TerminalAnswers)
        {
            if (text.contains(query) && !std::ranges::contains(answered, query))
            {
                answered.push_back(query);
                std::ignore = process.write(answer);
            }
        }
        return text.contains(ProbeSentinel);
    });
}

} // namespace

// A ConPty that was never started has no pseudoconsole to resize. Rather than drop the size, it
// records it so that start() can create the pseudoconsole at the size the display asked for --
// attachDisplay() is what first tells a PTY its size, and it runs before start().
TEST_CASE("ConPty.resizeScreen.beforeStart", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.isClosed());
    REQUIRE(pty.pageSize() == pageSize(80, 24));

    pty.resizeScreen(pageSize(132, 50));

    CHECK(pty.pageSize() == pageSize(132, 50));
}

// Regression test: resizing a *closed* ConPty must not reach ResizePseudoConsole.
//
// close() invalidates _master but leaves _slave alive, so the old `if (!_slave)` guard let a
// post-close resize through and passed INVALID_HANDLE_VALUE to ResizePseudoConsole, which
// dereferences it -- an access violation reading 0xffffffffffffffff. It was reached on the ordinary
// session-teardown path: closing a pane makes QML re-run the Loader binding for the surviving
// panes, which re-enters setSession() -> attachDisplay() -> Terminal::resizeScreen() while the PTY
// is already closed.
//
// Without the fix this test crashes the runner rather than failing an assertion, which is precisely
// the behaviour being guarded against.
TEST_CASE("ConPty.resizeScreen.afterClose", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };

    REQUIRE(pty.start().has_value());
    REQUIRE_FALSE(pty.isClosed());

    // A resize on the live pseudoconsole is forwarded and remembered.
    pty.resizeScreen(pageSize(100, 30));
    CHECK(pty.pageSize() == pageSize(100, 30));

    pty.close();
    REQUIRE(pty.isClosed());

    // The slave outlives close(); it is the master handle that decides whether a resize can be
    // forwarded. Guarding on the slave is what let the invalidated handle through.
    pty.resizeScreen(pageSize(132, 50));

    // The size is still recorded, matching the not-yet-started branch and UnixPty's behaviour.
    CHECK(pty.pageSize() == pageSize(132, 50));
}

// close() is reachable more than once (explicitly, then again from ~ConPty), and a resize may
// arrive between the two. Neither the second close nor the resize may touch the stale handle.
TEST_CASE("ConPty.resizeScreen.afterRepeatedClose", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };

    REQUIRE(pty.start().has_value());
    pty.close();
    pty.close();

    REQUIRE(pty.isClosed());

    pty.resizeScreen(pageSize(20, 10));

    CHECK(pty.pageSize() == pageSize(20, 10));
}

// Terminal::processInputOnce() reads ahead with a zero timeout while the render thread holds the
// terminal. A zero-timeout read that parked in ReadFile there would hold back output already read
// until the child wrote again -- forever, for a child sitting at a prompt.
TEST_CASE("ConPty.read.aZeroTimeoutNeverBlocks", "[vtpty][conpty]")
{
    using namespace std::chrono_literals;
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    // Whatever the pseudoconsole announces on start is read first; once nothing is waiting, a
    // zero-timeout read must come back empty-handed at once, saying EAGAIN.
    auto reader = std::async(std::launch::async, [&pty]() -> int {
        auto pool = crispy::BufferObjectPool<char> { 4096 };
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 100))
        {
            auto const storage = pool.allocateBufferObject();
            if (!pty.read(*storage, std::chrono::milliseconds::zero(), 4096))
                return errno;
        }
        return 0;
    });

    auto const finished = reader.wait_for(5s) == std::future_status::ready;
    pty.close(); // breaks the pipe, so a ReadFile that wrongly parked returns and the test can end
    CHECK(finished);
    CHECK(reader.get() == EAGAIN);
}

// Terminal::processInputOnce() takes EAGAIN and EINTR for "nothing yet" and reads again. A zero-timeout
// read leaves EAGAIN behind, so a read that fails once the child is gone must not leave it standing: the
// parser thread would read again, fail again at once, and spin.
TEST_CASE("ConPty.read.aFailedReadSaysTheDeviceIsGone", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());
    auto pool = crispy::BufferObjectPool<char> { 4096 };

    // Whatever the pseudoconsole announces on start is read first, until a zero-timeout read finds
    // nothing waiting.
    auto const nothingWaiting = [&]() {
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 100))
        {
            auto const storage = pool.allocateBufferObject();
            if (!pty.read(*storage, std::chrono::milliseconds::zero(), 4096))
                return true;
        }
        return false;
    }();
    REQUIRE(nothingWaiting);
    REQUIRE(errno == EAGAIN);

    pty.close();
    auto const storage = pool.allocateBufferObject();
    auto const result = pty.read(*storage, std::nullopt, 4096);
    auto const error = errno;

    CHECK_FALSE(result.has_value());
    CHECK(error != EAGAIN);
    CHECK(error != EINTR);
}

// A child writes to its pseudoconsole even while this process's own standard output is redirected,
// as it is under ctest, or for a daemon logging to a file. Unless told which standard handles to use,
// CreateProcess hands a console child its parent's redirected ones, and everything the child prints
// then bypasses the terminal for the parent's output.
TEST_CASE("ConPty.childWritesToPseudoconsoleWhileOutputIsRedirected", "[vtpty][conpty]")
{
    // Inheritable, as the pipe a test runner reads this process's output from is.
    auto inheritable = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                             .lpSecurityDescriptor = nullptr,
                                             .bInheritHandle = TRUE };
    auto readEnd = HANDLE {};
    auto writeEnd = HANDLE {};
    REQUIRE(CreatePipe(&readEnd, &writeEnd, &inheritable, 0));
    auto* const originalOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, writeEnd);
    auto const restoreOutput = gsl::finally([&] {
        SetStdHandle(STD_OUTPUT_HANDLE, originalOutput);
        CloseHandle(writeEnd);
        CloseHandle(readEnd);
    });

    auto const output = runProbe();
    INFO("pseudoconsole output: " << core::escape(output));

    CHECK(output.contains(ProbeSentinel));
}
