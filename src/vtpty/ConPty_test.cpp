// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <gsl/util>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
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
using vtpty::testing::execInfo;

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

/// Copies the probe into @p directory, which it creates.
/// @return The copy.
std::filesystem::path probeCopiedInto(std::filesystem::path const& directory)
{
    std::filesystem::create_directories(directory);
    auto copy = directory / probeProgram().filename();
    std::filesystem::copy_file(probeProgram(), copy);
    return copy;
}

/// Reads what a terminal would see of the probe running in @p process until its sentinel, answering the
/// pseudoconsole's queries along the way as a terminal would; fails the test when the sentinel never comes.
/// @param process The started probe.
/// @return Everything the pseudoconsole wrote.
std::string readProbeOutput(vtpty::Process& process)
{
    auto answered = std::vector<std::string_view> {};
    auto output = vtpty::testing::readUntil(process, [&](std::string_view text) {
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
    INFO("pseudoconsole output: " << core::escape(output));
    REQUIRE(output.contains(ProbeSentinel));
    return output;
}

/// Runs the probe on a pseudoconsole and reads what a terminal would see of it until the probe's
/// sentinel, as readProbeOutput() does.
/// @param exec How to run the probe, or a copy of it: what to pass it, for it to print back.
/// @return Everything the pseudoconsole wrote.
std::string runProbe(vtpty::Process::ExecInfo const& exec = execInfo(core::pathToUtf8(probeProgram())))
{
    auto process = vtpty::testing::unplacedProcess(exec, pageSize(80, 24));
    auto const started = process.start();

    // Ended on the way out, however the test went: ~Process waits for its child, which may be the login
    // shell a failed spawn fell back to rather than the probe, which ends by itself.
    auto const endChild =
        gsl::finally([&process] { process.terminate(vtpty::Process::TerminationHint::Normal); });

    REQUIRE(started.has_value());
    // The probe itself, not the login shell a failed spawn falls back to.
    REQUIRE(started->diagnostic.empty());

    return readProbeOutput(process);
}

/// @return What the probe printed in @p output under @p label, in order, decoded from its hex.
std::vector<std::string> printedFields(std::string_view output, std::string_view label)
{
    auto const prefix = std::format("{}: [", label);
    auto fields = std::vector<std::string> {};
    for (auto const piece: std::views::split(output, std::string_view { prefix }) | std::views::drop(1))
    {
        auto const line = std::string_view { piece.begin(), piece.end() };
        auto field = core::fromHexString(line.substr(0, line.find(']')));
        REQUIRE(field.has_value());
        fields.push_back(std::move(*field));
    }
    return fields;
}

/// @return The arguments the probe printed in @p output, in order.
std::vector<std::string> printedArguments(std::string_view output)
{
    return printedFields(output, "argument");
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

namespace
{

/// How long a test waits for a read on another thread to come back before it gives up on it.
constexpr auto ReadReturnBound = std::chrono::seconds { 10 };

/// How a read that brought nothing back ended.
struct EmptyRead
{
    int error;                          ///< The errno it left.
    std::chrono::milliseconds duration; ///< How long it took.
};

/// Reads @p pty, @p timeout at a time, until a read brings nothing back.
/// @return How that read ended.
EmptyRead readUntilEmpty(ConPty& pty, std::optional<std::chrono::milliseconds> timeout)
{
    auto const& clock = core::platform::defaultSteadyClock();
    auto pool = crispy::BufferObjectPool<char> { 4096 };
    while (true)
    {
        auto const storage = pool.allocateBufferObject();
        auto const started = clock.now();
        if (auto const result = pty.read(*storage, timeout, 4096); !result)
            return EmptyRead { .error = errno,
                               .duration = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now()
                                                                                                 - started) };
    }
}

/// Reads @p pty, @p timeout at a time, on a thread of its own, until a read brings nothing back.
/// @return How that read will have ended.
std::future<EmptyRead> readUntilEmptyAsync(ConPty& pty, std::optional<std::chrono::milliseconds> timeout)
{
    return std::async(std::launch::async, [&pty, timeout] { return readUntilEmpty(pty, timeout); });
}

/// @return Whether @p read came back within ReadReturnBound. When it did not, @p pty is closed, which
///         ends the read in any case, so that the test fails rather than hangs.
bool cameBack(std::future<EmptyRead> const& read, ConPty& pty)
{
    if (read.wait_for(ReadReturnBound) == std::future_status::ready)
        return true;
    pty.close();
    return false;
}

} // namespace

// A read waits for the pseudoconsole to say something for as long as it is told to, and no longer: a
// reader that has to notice anything else -- its session closing, a refresh to render -- relies on it.
TEST_CASE("ConPty.readGivesUpAtItsTimeout", "[vtpty][conpty]")
{
    constexpr auto Timeout = std::chrono::milliseconds { 200 };
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    auto read = readUntilEmptyAsync(pty, Timeout);
    REQUIRE(cameBack(read, pty));

    auto const [error, duration] = read.get();
    CHECK(error == EAGAIN);
    // A wait may come back up to a timer tick early.
    CHECK(duration >= Timeout / 2);
    CHECK(duration < ReadReturnBound);
}

// wakeupReader() ends a read that is waiting, as it does on every other PTY.
TEST_CASE("ConPty.wakeupReaderEndsAWaitingRead", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    auto read = readUntilEmptyAsync(pty, std::nullopt);
    std::this_thread::sleep_for(std::chrono::milliseconds { 500 }); // past whatever ConPTY says at first
    pty.wakeupReader();
    REQUIRE(cameBack(read, pty));

    CHECK(read.get().error == EINTR);
}

// close() may come from another thread while a read waits on the pipe, as it does when a tab is
// closed. The read ends, and so does every read after it, without touching the closed handle.
TEST_CASE("ConPty.closeEndsAWaitingRead", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    auto read = readUntilEmptyAsync(pty, std::nullopt);
    std::this_thread::sleep_for(std::chrono::milliseconds { 500 }); // past whatever ConPTY says at first
    pty.close();
    REQUIRE(cameBack(read, pty));
    CHECK(read.get().error == EINTR);

    CHECK(readUntilEmpty(pty, std::nullopt).error == EPIPE);
}

// close() comes from the GUI thread while the session's reader reads again after every EINTR, as its main
// loop does: Terminal::processInputOnce() answers EINTR with "go on". The read close() wakes lets go of the
// PTY, and the reader must not take it back for a read that waits for output. An idle shell writes none, so
// close() waited for it, freezing the GUI, and the pseudoconsole it closes next -- which is what ends the
// shell -- stayed open.
//
// Whether the reader takes the PTY back first is up to the scheduler. Here the reader and close() share one
// processor, the reader at the higher priority, so that it runs the moment close() wakes it, before close()
// reaches for the PTY: the order a busy machine may take, made the only one.
TEST_CASE("ConPty.closeIsNotHeldUpByAReaderThatReadsAgain", "[vtpty][conpty][threading]")
{
    constexpr auto CloseBound = std::chrono::seconds { 2 };
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    auto processors = DWORD_PTR {};
    auto systemProcessors = DWORD_PTR {};
    REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &processors, &systemProcessors));
    auto const oneProcessor = DWORD_PTR { 1 } << std::countr_zero(processors);
    auto const runOnOneProcessorAt = [oneProcessor](int priority) {
        return SetThreadAffinityMask(GetCurrentThread(), oneProcessor) != 0
               && SetThreadPriority(GetCurrentThread(), priority) != FALSE;
    };

    // The errno of each read that brought nothing back, up to the first that is neither EINTR nor EAGAIN,
    // or until the test gives up.
    auto errors = std::vector<int> {};
    auto readerScheduled = false;
    auto givingUp = std::atomic<bool> { false };
    auto reader = std::jthread { [&] {
        readerScheduled = runOnOneProcessorAt(THREAD_PRIORITY_HIGHEST);
        auto pool = crispy::BufferObjectPool<char> { 4096 };
        while (true)
        {
            auto const storage = pool.allocateBufferObject();
            if (pty.read(*storage, std::nullopt, 4096))
                continue;
            errors.push_back(errno);
            if ((errors.back() != EINTR && errors.back() != EAGAIN) || givingUp)
                return;
        }
    } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 500 }); // past whatever ConPTY says at first

    auto closeScheduled = false;
    auto closeReturned = std::promise<void> {};
    auto const closeReturns = closeReturned.get_future();
    auto closing = std::jthread { [&] {
        closeScheduled = runOnOneProcessorAt(THREAD_PRIORITY_LOWEST);
        pty.close();
        closeReturned.set_value();
    } };
    auto const closedInTime = closeReturns.wait_for(CloseBound) == std::future_status::ready;
    // A close() held up is let go by a reader that stops reading, so that the test fails rather than hangs.
    givingUp = true;
    while (closeReturns.wait_for(std::chrono::milliseconds { 100 }) != std::future_status::ready)
        pty.wakeupReader();
    reader.join();
    closing.join();

    REQUIRE(readerScheduled);
    REQUIRE(closeScheduled);
    CHECK(closedInTime);
    // The read close() woke says EINTR, and the one the reader starts right after it EPIPE, without a wait.
    CHECK(errors == std::vector { EINTR, EPIPE });
}

// The pseudoconsole API reports why it failed in the HRESULT it returns, not in the thread's last error,
// which would say nothing, or something unrelated. A pseudoconsole of no size is one it refuses
// (CreatePseudoConsole), and a negative size one it cannot resize to (ResizePseudoConsole).
TEST_CASE("ConPty.failureSaysWhy", "[vtpty][conpty]")
{
    // E_INVALIDARG, as the system spells it.
    constexpr auto InvalidArgument = std::string_view { "0x80070057" };

    SECTION("creating the pseudoconsole")
    {
        auto pty = ConPty { pageSize(0, 0) };
        auto const started = pty.start();
        REQUIRE_FALSE(started.has_value());
        INFO("detail: " << started.error().detail);
        CHECK(started.error().detail.contains(InvalidArgument));
        CHECK_FALSE(started.error().detail.starts_with(' '));
    }

    SECTION("resizing it")
    {
        auto pty = ConPty { pageSize(80, 24) };
        REQUIRE(pty.start().has_value());
        auto message = std::string {};
        try
        {
            pty.resizeScreen(pageSize(40'000, 24)); // past what a COORD holds
        }
        catch (std::runtime_error const& error)
        {
            message = error.what();
        }
        INFO("message: " << message);
        CHECK(message.contains(InvalidArgument));
        CHECK_FALSE(message.starts_with(' '));
    }
}

// A write that close() overtakes fails, and so does every write after it, without touching the closed
// handle, which Windows may by then have handed to something else.
TEST_CASE("ConPty.writeFailsOnceClosed", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());
    REQUIRE(pty.write("x") == 1);

    // Writing from another thread while close() runs, as the session's input does while its tab closes.
    auto writes = std::async(std::launch::async, [&pty] {
        auto written = 1;
        while (written == 1)
            written = pty.write("x");
        return errno;
    });
    pty.close();
    REQUIRE(writes.wait_for(ReadReturnBound) == std::future_status::ready);
    CHECK(writes.get() == EPIPE);

    CHECK(pty.write("x") == -1);
}

// A write and close() racing, round after round: no crash, and once a write has failed none succeeds, each
// saying EPIPE, nor does a byte reach a pipe opened right after the close, which Windows may hand the value
// of the handle close() let go of. The writes are serialized against close() so that none can use that
// handle. Not deterministic: whether a write meets the close at the one moment that matters in a round is
// up to the scheduler, so a clean run says the race was not seen, not that it cannot happen.
TEST_CASE("ConPty.writeRacingCloseFailsCleanly", "[vtpty][conpty][threading]")
{
    struct Writes
    {
        int failures = 0;
        int successesAfterFailure = 0;
        int otherErrors = 0;
    };

    for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
    {
        auto pty = ConPty { pageSize(80, 24) };
        REQUIRE(pty.start().has_value());

        auto writes = std::async(std::launch::async, [&pty] {
            auto result = Writes {};
            while (result.failures < 1000)
            {
                if (pty.write("x") == 1)
                    result.successesAfterFailure += result.failures > 0 ? 1 : 0;
                else
                {
                    ++result.failures;
                    result.otherErrors += errno == EPIPE ? 0 : 1;
                }
            }
            return result;
        });
        pty.close();

        // Pipes to take the handle values close() let go of.
        auto pipes = std::array<std::pair<HANDLE, HANDLE>, 4> {};
        for (auto& [reader, writer]: pipes)
            REQUIRE(CreatePipe(&reader, &writer, nullptr, 0));
        auto const closePipes = gsl::finally([&pipes] {
            for (auto const& [reader, writer]: pipes)
            {
                CloseHandle(reader);
                CloseHandle(writer);
            }
        });

        REQUIRE(writes.wait_for(ReadReturnBound) == std::future_status::ready);
        auto const result = writes.get();
        CHECK(result.successesAfterFailure == 0);
        CHECK(result.otherErrors == 0);
        for (auto const& [reader, writer]: pipes)
        {
            auto stray = DWORD {};
            REQUIRE(PeekNamedPipe(reader, nullptr, 0, nullptr, &stray, nullptr));
            CHECK(stray == 0);
        }
    }
}

// A ConPty is usable as constructed, its slave() included, which used to be null until start(). The slave
// end is the console host's, so a write to it fails, before start() and after.
TEST_CASE("ConPty.slaveIsThereFromConstruction", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 24) };
    auto& slave = pty.slave();
    errno = 0;
    CHECK(slave.write("x") == -1);
    CHECK(errno == EPIPE);

    REQUIRE(pty.start().has_value());
    CHECK(&pty.slave() == &slave);
    CHECK(pty.slave().write("x") == -1);
}

// A session's exit watcher waits in waitForClosed() on a thread of its own, and freeing the session waits
// for that thread once it has closed the PTY: close() ends the wait at once, not on a poll's next tick.
TEST_CASE("ConPty.closeEndsAWaitForClosed", "[vtpty][conpty][threading]")
{
    constexpr auto Bound = std::chrono::milliseconds { 200 };
    auto pty = ConPty { pageSize(80, 24) };
    REQUIRE(pty.start().has_value());

    auto waiting = std::async(std::launch::async, [&pty] { pty.waitForClosed(); });
    CHECK(waiting.wait_for(std::chrono::milliseconds { 100 })
          == std::future_status::timeout); // open: it waits
    auto const closedAt = std::chrono::steady_clock::now();
    pty.close();
    REQUIRE(waiting.wait_for(std::chrono::seconds { 5 }) == std::future_status::ready);

    auto const took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - closedAt);
    INFO("waitForClosed() returned " << took.count() << " ms after close()");
    CHECK(took < Bound);
}

// A PTY never started, or closed already, has nothing to wait for.
TEST_CASE("ConPty.waitForClosedReturnsOnceClosed", "[vtpty][conpty]")
{
    auto const returnsAtOnce = [](ConPty& pty) {
        auto waiting = std::async(std::launch::async, [&pty] { pty.waitForClosed(); });
        auto const returned =
            waiting.wait_for(std::chrono::milliseconds { 200 }) == std::future_status::ready;
        pty.close(); // lets a wait that did not return go, rather than hang the suite
        return returned;
    };

    auto neverStarted = ConPty { pageSize(80, 24) };
    CHECK(returnsAtOnce(neverStarted));

    auto closed = ConPty { pageSize(80, 24) };
    REQUIRE(closed.start().has_value());
    closed.close();
    CHECK(returnsAtOnce(closed));
}

// Freeing a session frees its Process on the GUI thread, once the session has closed it. The console host
// ends the child after that by itself, in the child's own time -- a program saving its work holds the close
// up for seconds -- and nothing is left for the Process to wait for.
TEST_CASE("ConPty.closedProcessIsNotHeldUpByItsChild", "[vtpty][conpty]")
{
    constexpr auto Bound = std::chrono::milliseconds { 200 };
    auto process = std::optional<vtpty::Process> {};
    process.emplace(execInfo(core::pathToUtf8(probeProgram()), { "--linger-on-close" }),
                    vtpty::createPty(pageSize(80, 24), std::nullopt),
                    /*escapeSandbox=*/false,
                    std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process->start().has_value());
    readProbeOutput(*process); // the probe is up, and lingers once closed

    process->close();
    auto const freedAt = std::chrono::steady_clock::now();
    process.reset();

    auto const took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - freedAt);
    INFO("~Process returned " << took.count() << " ms after it was closed");
    CHECK(took < Bound);
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

    std::ignore = runProbe();
}

// The program asked for runs, and receives its arguments as they were given. Windows hands a child one
// command line, which the child's C runtime splits again at every space outside double quotes, reading
// a backslash before a double quote as an escape. And CreateProcess reads an unquoted program path with
// a space in it -- "C:\Program Files\PowerShell\7\pwsh.exe" -- one space-ended prefix at a time:
// "C:\Program.exe" first, which runs instead wherever it exists, handed the rest of the path as its
// first argument.
TEST_CASE("ConPty.childReceivesItsProgramPathAndArgumentsIntact", "[vtpty][conpty]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
    auto const program = probeCopiedInto(directory.path() / "with space");
    // The prefix that would run instead: "...\with space\probe.exe" read up to its space.
    std::filesystem::copy_file(probeProgram(), directory.path() / "with.exe");

    auto const arguments = std::vector<std::string> {
        "plain", "two  words", R"(say "hi" twice)", R"(a\\"b)", R"(ends in\)", "", R"(C:\dir\file)",
    };
    // Not read as a command line either, which would split it at the same space: it names a program.
    auto const output = runProbe(execInfo(core::pathToUtf8(program), arguments));
    CHECK(printedArguments(output) == arguments);
}

// A program configured as a whole command line still runs -- a `shell:` setting "wsl.exe -d Ubuntu",
// which worked while the program went to CreateProcess unquoted. It names no program, so it is read as
// the C runtime reads a command line: its first word the program, the rest arguments ahead of the
// configured ones.
TEST_CASE("ConPty.childRunsAProgramConfiguredAsACommandLine", "[vtpty][conpty]")
{
    SECTION("its words, the program's directories separated by backslashes")
    {
        auto const output =
            runProbe(execInfo(std::format("{} first second", core::pathToUtf8(probeProgram())), { "third" }));
        CHECK(printedArguments(output) == std::vector<std::string> { "first", "second", "third" });
    }

    SECTION("a quoted program path with a space in it")
    {
        // A value holding a double quote reached CreateProcess as written before it was read so, and
        // still would: the reading must not change what it runs.
        auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
        auto const program = probeCopiedInto(directory.path() / "with space");

        auto const output =
            runProbe(execInfo(std::format(R"("{}" first)", core::pathToUtf8(program)), { "second" }));
        CHECK(printedArguments(output) == std::vector<std::string> { "first", "second" });
    }

    SECTION("an unquoted program path with a space in it, beside a file without an extension")
    {
        // CreateProcess runs a name without an extension only with ".exe" appended: a file without one at
        // a prefix of the path -- "C:\Program", the classic -- it passes over, and runs the program. The
        // reading must pass over it too, or the child is handed the rest of the path as an argument.
        auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
        auto const program = probeCopiedInto(directory.path() / "with space");
        std::filesystem::copy_file(probeProgram(), directory.path() / "with");

        auto const output = runProbe(execInfo(std::format("{} first", core::pathToUtf8(program))));
        CHECK(printedArguments(output) == std::vector<std::string> { "first" });
    }

    SECTION("cmd.exe running a quoted program, spelled in its /s /c idiom")
    {
        // cmd.exe reads its command line by rules of its own: it drops the outermost quotes of what
        // follows /s /c, and runs the rest. What follows the program reaches it as written; read by the C
        // runtime's rules and quoted again, it would be a different line.
        auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
        auto const program = probeCopiedInto(directory.path() / "with space");

        auto const output =
            runProbe(execInfo(std::format(R"(cmd.exe /d /s /c ""{}" first")", core::pathToUtf8(program))));
        CHECK(printedArguments(output) == std::vector<std::string> { "first" });
    }

    SECTION("an unquoted program path with a space in it, as Git Bash is configured")
    {
        // "C:\Program Files\Git\bin\bash.exe --login": the program is the first whitespace-ended prefix
        // that names one, tried from the left, as CreateProcess reads it. Read as words instead, it ran only
        // because CreateProcess tried the prefixes of the line they were joined back into -- with one blank
        // between them, so not under a directory whose name has two. The decoy is the prefix one word
        // longer, which a reading that tried the longest first would run instead.
        auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
        auto const program = probeCopiedInto(directory.path() / "with  space");
        auto decoy = program;
        decoy.replace_extension().concat(" first.exe");
        std::filesystem::copy_file(probeProgram(), decoy);

        for (auto const& spelled: { program, std::filesystem::path { program }.replace_extension() })
        {
            auto const output =
                runProbe(execInfo(std::format("{} first second", core::pathToUtf8(spelled)), { "third" }));
            INFO("configured program: " << core::pathToUtf8(spelled));
            CHECK(printedArguments(output) == std::vector<std::string> { "first", "second", "third" });
        }
    }
}

// Every string a spawn hands the child arrives as it was given, beyond ASCII too: the program's path, its
// arguments, its environment and its working directory are UTF-8 here, and a wide Win32 API takes them
// converted; the code-page ("A") API would read them in the ANSI code page, which spells most of Unicode
// not at all.
TEST_CASE("ConPty.childReceivesWhatItIsGivenBeyondAscii", "[vtpty][conpty]")
{
    constexpr auto Awkward = vtpty::testing::NonAsciiNameUtf8;
    auto const directory = core::testing::ScopedTempDir { "contour-conpty" };

    // One at a time, so that each is seen to arrive on its own.
    SECTION("a program under a directory so named")
    {
        auto const program =
            probeCopiedInto(directory.path() / core::pathFromTrustedUtf8(std::format("program {}", Awkward)));

        auto const output = runProbe(execInfo(core::pathToUtf8(program), { "ran" }));
        CHECK(printedArguments(output) == std::vector<std::string> { "ran" });
    }

    SECTION("its arguments")
    {
        auto const exec = execInfo(core::pathToUtf8(probeProgram()),
                                   { std::string { Awkward }, std::format("a {} b", Awkward) });
        auto const output = runProbe(exec);
        CHECK(printedArguments(output) == exec.arguments);
    }

    SECTION("its environment")
    {
        auto exec = execInfo(core::pathToUtf8(probeProgram()));
        exec.env = { { "CONTOUR_PROBE_VALUE", std::format("value {}", Awkward) } };
        auto const output = runProbe(exec);
        CHECK(printedFields(output, "environment")
              == std::vector { std::format("CONTOUR_PROBE_VALUE=value {}", Awkward) });
    }

    SECTION("its working directory")
    {
        auto exec = execInfo(core::pathToUtf8(probeProgram()));
        exec.workingDirectory =
            directory.path() / core::pathFromTrustedUtf8(std::format("directory {}", Awkward));
        std::filesystem::create_directory(exec.workingDirectory);
        auto const output = runProbe(exec);
        auto const directories = printedFields(output, "directory");
        REQUIRE(directories.size() == 1);
        CHECK(core::pathFromUtf8(directories.front()) == exec.workingDirectory);
    }
}

// The child's environment is built for it and handed to it, rather than set in this process's own for it
// to inherit, where another thread would read the child's values meanwhile: what it inherits, less what
// it is not to -- the name compared as Windows compares it -- and what is configured for it.
TEST_CASE("ConPty.childIsHandedTheEnvironmentBuiltForIt", "[vtpty][conpty]")
{
    auto const inherited = core::testing::ScopedEnv { "CONTOUR_PROBE_INHERITED", "kept" };
    auto const dropped = core::testing::ScopedEnv { "CONTOUR_PROBE_DROPPED", "lost" };
    auto exec = execInfo(core::pathToUtf8(probeProgram()));
    exec.env = { { "CONTOUR_PROBE_SET", "configured" }, { "CONTOUR_PROBE_EMPTY", "" } };
    exec.removedEnvironment = { "contour_probe_dropped" };

    auto const output = runProbe(exec);
    CHECK(printedFields(output, "environment")
          == std::vector<std::string> { "CONTOUR_PROBE_INHERITED=kept", "CONTOUR_PROBE_SET=configured" });
}

// A program named without a directory runs as found on the PATH configured for the child, as it does on
// POSIX, where the child looks it up once it has its environment: CreateProcess searches the PATH of this
// process, which a configured one is not on.
TEST_CASE("ConPty.childIsFoundOnThePathConfiguredForIt", "[vtpty][conpty]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-conpty" };
    std::filesystem::copy_file(probeProgram(), directory.path() / "contour-probe-on-path.exe");

    auto exec = execInfo("contour-probe-on-path", { "found" });
    exec.env = { { "Path", core::pathToUtf8(directory.path()) } }; // spelled as Windows spells it
    auto const output = runProbe(exec);
    CHECK(printedArguments(output) == std::vector<std::string> { "found" });
}

// A single word that names no program has no command line to read: the spawn falls back to the login
// shell, and says so.
TEST_CASE("ConPty.unknownProgramFallsBackToTheLoginShell", "[vtpty][conpty]")
{
    auto process = vtpty::testing::unplacedProcess(execInfo("contour-no-such-program"), pageSize(80, 24));
    auto const started = process.start();
    REQUIRE(started.has_value());
    CHECK(started->diagnostic.contains("Failed to spawn contour-no-such-program"));
    process.terminate(vtpty::Process::TerminationHint::Normal);
    std::ignore = process.wait();
}

// Whether an OSC 133 mark a console program writes reaches the terminal on the far side of the
// pseudoconsole -- a shell integration on Windows, PowerShell's, depends on it. ConPTY stands
// between the two: it parses what the child writes into a console buffer of its own, and a sequence
// reaches the terminal only if ConPTY passes it on. The ConPTY under test is the one Contour ships
// beside its executable (CONTOUR_EMBED_OPENCONSOLE); without it, the system's own.
TEST_CASE("ConPty.passesOsc133Through", "[vtpty][conpty][osc133]")
{
    INFO("bundled conpty.dll beside the test: "
         << std::filesystem::exists(probeProgram().parent_path() / "conpty.dll"));

    auto const output = runProbe();
    INFO("pseudoconsole output: " << core::escape(output));

    // The probe ran, and ConPTY rendered what it printed, and passed every mark on, parameters included. Only
    // the introducer and payload are checked: ConPTY may re-terminate a sequence with BEL instead of ST.
    CHECK(output.contains("\x1b]133;A"));
    CHECK(output.contains("\x1b]133;B"));
    CHECK(output.contains("\x1b]133;C;cmdline_url=echo%20hi"));
    CHECK(output.contains("\x1b]133;D;0"));
}
