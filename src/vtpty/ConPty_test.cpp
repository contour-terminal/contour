// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/HandoverSupport.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>
#ifdef _WIN32
    #include <vtpty/Win32Handle.hpp>
#endif

#include <crispy/BufferObject.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

// The umbrella header: gsl::finally is in <gsl/gsl_util> in the GSL 3.1.0 the tree pins, and in <gsl/util>
// in 4.x.
#include <gsl/gsl>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    auto output = vtpty::testing::readUntilAnswering(
        process, [](std::string_view text) { return text.contains(ProbeSentinel); });
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

namespace
{

/// @return The program a pseudoconsole is handed over with, and the process that adopts it
///         (test/conpty_handover_probe.cpp).
std::filesystem::path handoverProbe()
{
    auto probe = std::filesystem::path { VTPTY_CONPTY_HANDOVER_PROBE };
    probe.make_preferred();
    return probe;
}

/// Reads @p pty until @p needle, answering the pseudoconsole's queries along the way as a terminal would.
/// @return Whether it came.
bool awaitOutput(vtpty::Pty& pty, std::string_view needle)
{
    auto const output = vtpty::testing::readUntilAnswering(
        pty, [needle](std::string_view text) { return text.contains(needle); });
    INFO("pseudoconsole output: " << core::escape(output));
    return output.contains(needle);
}

/// Starts @p commandLine with exactly @p inherited inherited, and its standard error this process's.
/// @return The process; null where it could not be started.
HANDLE spawnInheriting(std::wstring commandLine, std::vector<HANDLE> inherited)
{
    auto errorOutput = HANDLE {};
    DuplicateHandle(GetCurrentProcess(),
                    GetStdHandle(STD_ERROR_HANDLE),
                    GetCurrentProcess(),
                    &errorOutput,
                    0,
                    TRUE,
                    DUPLICATE_SAME_ACCESS);
    if (errorOutput != nullptr)
        inherited.push_back(errorOutput);
    for (auto* const handle: inherited)
        REQUIRE(vtpty::setInheritance(handle, vtpty::HandleInheritance::Inherited).has_value());

    auto attributes = vtpty::AttributeList::create(1);
    REQUIRE(attributes.has_value());
    REQUIRE(attributes->inheritOnly(inherited).has_value());
    auto startup = STARTUPINFOEXW {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdError = errorOutput;
    startup.lpAttributeList = attributes->get();
    auto info = PROCESS_INFORMATION {};
    auto const started = CreateProcessW(nullptr,
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup.StartupInfo,
                                        &info);
    if (errorOutput != nullptr)
        CloseHandle(errorOutput);
    if (!started)
        return nullptr;
    CloseHandle(info.hThread);
    return info.hProcess;
}

/// Whether a process runs or is held, as a console host that stopped reading would be.
enum class Scheduling : uint8_t
{
    Running = 0,
    Suspended,
};

/// Suspends or resumes every thread of @p process.
/// @return The call's NTSTATUS: zero once done.
[[nodiscard]] LONG schedule(HANDLE process, Scheduling scheduling)
{
    using NtProcessFn = LONG(NTAPI*)(HANDLE);
    auto* const call = vtpty::ntdllFunction<NtProcessFn>(
        scheduling == Scheduling::Suspended ? "NtSuspendProcess" : "NtResumeProcess");
    return call != nullptr ? call(process) : static_cast<LONG>(STATUS_ENTRYPOINT_NOT_FOUND);
}

/// A handle of the test's own to what @p handle names: the program's exit closes the pseudoconsole's handles,
/// and frees their values.
[[nodiscard]] vtpty::UniqueHandle ownHandleTo(HANDLE handle)
{
    auto duplicate = HANDLE {};
    REQUIRE(DuplicateHandle(
                GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)
            != FALSE);
    return vtpty::UniqueHandle { duplicate };
}

/// Fills @p pipe, the pipe to a pseudoconsole that reads nothing, behind the back of the ConPty that writes
/// to it: in ever smaller writes, until a single byte no longer goes in at once. A write that does not is
/// cancelled, which takes all of it back, as nothing reads it.
void fillBehindItsBack(HANDLE pipe)
{
    auto const completed = vtpty::UniqueHandle { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    REQUIRE(completed.get() != nullptr);
    auto const filler = std::string(4096, 'z');
    auto size = filler.size();
    while (size > 0)
    {
        auto overlapped = OVERLAPPED {};
        overlapped.hEvent = completed.get();
        if (WriteFile(pipe, filler.data(), static_cast<DWORD>(size), nullptr, &overlapped))
            continue;
        REQUIRE(GetLastError() == ERROR_IO_PENDING);
        CancelIoEx(pipe, &overlapped);
        auto transferred = DWORD {};
        std::ignore = GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
        size /= 2;
    }
}

} // namespace

// A live upgrade on Windows hands each pseudoconsole to a successor process: the successor inherits its
// handles, rebuilds it (ConPty::adopt()) and the program on it (Process::adopt()), and the process that made
// it lets go of its own handles without closing it. The program must notice nothing, a resize from the
// successor must reach it, and closing the pseudoconsole there must end its console host, as for one the
// successor had made.
TEST_CASE("ConPty.adoptedInASecondProcessResizesAndCloses", "[vtpty][conpty][handover]")
{
    vtpty::testing::requireHandoverCapable();
    auto process =
        std::make_unique<vtpty::Process>(execInfo(core::pathToUtf8(handoverProbe()), { "program" }),
                                         std::make_unique<ConPty>(pageSize(80, 25)),
                                         /*escapeSandbox=*/false,
                                         std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process->start().has_value());
    auto& pty = static_cast<ConPty&>(process->pty());
    REQUIRE(pty.consoleHost() == vtpty::ConsoleHost::Bundled);
    REQUIRE(awaitOutput(*process, "ready"));
    std::ignore = process->write("size\r\n");
    REQUIRE(awaitOutput(*process, "size=80x25"));

    auto const handles = pty.handover();
    REQUIRE(handles.has_value());
    auto const child = process->handoverChild();
    REQUIRE(child.has_value());

    auto inherited = std::vector<HANDLE> {};
    for (auto const handle: handles->handles())
        inherited.push_back(handle.native());
    inherited.push_back(child->processHandle.native());
    auto commandLine = std::format(L"\"{}\" adopter", handoverProbe().wstring());
    for (auto const handle: handles->handles())
        commandLine += std::format(L" {}", handle.value);
    commandLine += std::format(L" {} {} 80 25", child->pid, child->processHandle.value);
    auto* const adopter = spawnInheriting(std::move(commandLine), inherited);
    REQUIRE(adopter != nullptr);

    // This process's own handles go, as the old daemon's go with it: the adopter's are the last.
    auto released = process->releaseChild();
    REQUIRE(released.has_value());
    auto programProcess = HANDLE {};
    DuplicateHandle(GetCurrentProcess(),
                    released->processHandle.native(),
                    GetCurrentProcess(),
                    &programProcess,
                    SYNCHRONIZE,
                    FALSE,
                    0);
    CloseHandle(released->processHandle.native());
    auto const ownHandles = pty.releaseHandover();
    REQUIRE(ownHandles.has_value());
    REQUIRE(ownHandles->conpty.has_value());
    for (auto const handle: ownHandles->conpty->handles())
        CloseHandle(handle.native());
    process.reset();

    REQUIRE(WaitForSingleObject(adopter, 30'000) == WAIT_OBJECT_0);
    auto exitCode = DWORD {};
    GetExitCodeProcess(adopter, &exitCode);
    CloseHandle(adopter);
    // The probe says on stderr what failed; its exit code names the step.
    CHECK(exitCode == 0);
    CHECK(WaitForSingleObject(programProcess, 10'000) == WAIT_OBJECT_0);
    CloseHandle(programProcess);
}

// A live upgrade stops each session's pump, which may be writing to a program that reads nothing: once the
// pipe is full, the write waits for good. interruptWrites() ends that wait with what was written, and every
// later write returns at once, until allowWrites().
TEST_CASE("ConPty.interruptWritesEndsAWriteThePseudoconsoleDoesNotRead", "[vtpty][conpty][threading]")
{
    vtpty::testing::requireHandoverCapable();
    auto process = vtpty::testing::unplacedProcess(execInfo(core::pathToUtf8(handoverProbe()), { "program" }),
                                                   pageSize(80, 25));
    REQUIRE(process.start().has_value());
    auto& pty = static_cast<ConPty&>(process.pty());
    REQUIRE(awaitOutput(process, "ready"));
    auto const handles = pty.handover();
    REQUIRE(handles.has_value());
    REQUIRE(handles->conpty.has_value());
    auto const consoleHost = ownHandleTo(handles->conpty->consoleHost.native());

    // Unchecked: the console host may have ended with the program by then, and it is resumed twice where the
    // test runs to its end.
    auto const resumeConsoleHost = [&consoleHost] {
        std::ignore = schedule(consoleHost.get(), Scheduling::Running);
    };
    auto const flood = std::string(4 * 1024 * 1024, 'x');
    auto result = std::future<std::pair<int, int>> {};

    // A console host that reads nothing more: the pipe fills, and the write waits.
    REQUIRE(schedule(consoleHost.get(), Scheduling::Suspended) == 0);
    // Declared after the future, so that it runs before the future is destroyed: that waits for the write,
    // which a suspended console host would never let end. A check that fails while the write waits must fail
    // the test, not hang it. The write ends once the pseudoconsole is closed under it, with its output no
    // longer read and its console host running: the host ends, and the pipe breaks.
    auto const release = gsl::finally([&] {
        resumeConsoleHost();
        pty.allowWrites();
        pty.close();
    });
    result = std::async(std::launch::async, [&] {
        auto const written = pty.write(flood);
        return std::pair { written, errno };
    });
    CHECK(result.wait_for(std::chrono::milliseconds { 300 }) == std::future_status::timeout);

    pty.interruptWrites();
    REQUIRE(result.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    auto const [written, error] = result.get();
    CHECK(written >= 0);
    CHECK(std::cmp_less(written, flood.size()));
    CHECK(error == EINTR);

    SECTION("a write while interrupted returns at once, having written nothing")
    {
        errno = 0;
        CHECK(pty.write("more") == 0);
        CHECK(errno == EINTR);
    }

    SECTION("allowWrites() lets writes through again")
    {
        pty.allowWrites();
        REQUIRE(schedule(consoleHost.get(), Scheduling::Running) == 0);
        CHECK(pty.write("\r\n") == 2);
    }

    // Running again before the program is ended and waited for: past the checks, nothing waits on a console
    // host that cannot run.
    resumeConsoleHost();
    process.terminate(vtpty::Process::TerminationHint::Normal);
    std::ignore = process.wait();
}

// A chunk of more than one byte waits for the reader where the pipe's room was overstated -- here, because
// the pipe was filled behind ConPty's back after it last asked. interruptWrites() ends that wait as it ends a
// single byte's, and the cancelled chunk, which the pseudoconsole never read, counts as not written.
TEST_CASE("ConPty.interruptWritesEndsAChunkThePipeHadNoRoomFor", "[vtpty][conpty][threading]")
{
    vtpty::testing::requireHandoverCapable();
    auto process = vtpty::testing::unplacedProcess(execInfo(core::pathToUtf8(handoverProbe()), { "program" }),
                                                   pageSize(80, 25));
    REQUIRE(process.start().has_value());
    auto& pty = static_cast<ConPty&>(process.pty());
    REQUIRE(awaitOutput(process, "ready"));
    auto const handles = pty.handover();
    REQUIRE(handles.has_value());
    REQUIRE(handles->conpty.has_value());
    auto const consoleHost = ownHandleTo(handles->conpty->consoleHost.native());
    auto const resumeConsoleHost = [&consoleHost] {
        std::ignore = schedule(consoleHost.get(), Scheduling::Running);
    };
    auto result = std::future<std::pair<int, int>> {};

    REQUIRE(schedule(consoleHost.get(), Scheduling::Suspended) == 0);
    // As in ConPty.interruptWritesEndsAWriteThePseudoconsoleDoesNotRead: run before the future waits.
    auto const release = gsl::finally([&] {
        resumeConsoleHost();
        pty.allowWrites();
        pty.close();
    });

    // ConPty asks the pipe its room, as nothing has written to it yet, and knows all of it but these two
    // bytes free. The host, suspended, changes nothing in between. Room enough for the chunk below, or it
    // would be sized to a single byte, and the case would test what the one above does.
    auto const chunk = std::string(1024, 'y');
    auto const pipe = handles->conpty->inputWriter.native();
    REQUIRE(vtpty::writableWithoutWaiting(pipe) >= chunk.size() + 2);
    REQUIRE(pty.write("ab") == 2);
    fillBehindItsBack(pipe);

    // Within what ConPty knows free, so it asks nothing, and hands the pipe all of it at once.
    result = std::async(std::launch::async, [&] {
        auto const written = pty.write(chunk);
        return std::pair { written, errno };
    });
    CHECK(result.wait_for(std::chrono::milliseconds { 300 }) == std::future_status::timeout);

    pty.interruptWrites();
    REQUIRE(result.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    auto const [written, error] = result.get();
    CHECK(written == 0);
    CHECK(error == EINTR);

    resumeConsoleHost();
    process.terminate(vtpty::Process::TerminationHint::Normal);
    std::ignore = process.wait();
}

// A live upgrade quiesces a session -- its writes interrupted -- and captures its pseudoconsole's handles by
// value. Closed then, as the program's exit watcher closes it, they would be freed for the next handle this
// process makes, and the successor would adopt that one under their names: the close is held until the
// upgrade rolls back.
TEST_CASE("ConPty.closeWhileWritesAreInterruptedWaitsUntilTheyAreAllowed", "[vtpty][conpty][handover]")
{
    vtpty::testing::requireHandoverCapable();
    auto pty = ConPty { pageSize(80, 25) };
    REQUIRE(pty.start().has_value());
    auto const before = pty.handover();
    REQUIRE(before.has_value());

    pty.interruptWrites();
    pty.close();
    CHECK_FALSE(pty.isClosed());
    auto const held = pty.handover();
    REQUIRE(held.has_value());
    CHECK(held->handles() == before->handles());

    pty.allowWrites();
    CHECK(pty.isClosed());
}

// The upgrade that went ahead: the handles are the successor's, and the close held for them is dropped with
// them.
TEST_CASE("ConPty.closeHeldForAHandoverIsDroppedWithItsHandles", "[vtpty][conpty][handover]")
{
    vtpty::testing::requireHandoverCapable();
    auto pty = ConPty { pageSize(80, 25) };
    REQUIRE(pty.start().has_value());
    pty.interruptWrites();
    pty.close();

    auto const handedOver = pty.releaseHandover();
    REQUIRE(handedOver.has_value());
    REQUIRE(handedOver->conpty.has_value());
    auto const& released = handedOver->conpty;
    CHECK(pty.isClosed());
    pty.allowWrites();
    for (auto const handle: released->handles())
    {
        auto flags = DWORD {};
        CHECK(GetHandleInformation(handle.native(), &flags) != FALSE);
    }

    // Adopted again here, as a successor would: it serves on, and closes everything as it goes.
    auto adopted = ConPty::adopt(*handedOver);
    REQUIRE(adopted.has_value());
    CHECK_FALSE((*adopted)->isClosed());
    adopted->reset();
    auto flags = DWORD {};
    CHECK(GetHandleInformation(released->signal.native(), &flags) == FALSE);
}

// The pseudoconsole takes no pixel size, but a live upgrade carries it, for the successor's terminal to lay
// images out by before any client tells it again.
TEST_CASE("ConPty.keepsThePixelSizeThePseudoconsoleCannotTake", "[vtpty][conpty]")
{
    auto pty = ConPty { pageSize(80, 25) };
    CHECK_FALSE(pty.pixelSize().has_value());
    auto const pixels = vtpty::ImageSize { .width = vtpty::Width(800), .height = vtpty::Height(500) };
    pty.resizeScreen(pageSize(100, 50), pixels);
    CHECK(pty.pixelSize() == pixels);
    pty.resizeScreen(pageSize(90, 50));
    CHECK(pty.pixelSize() == pixels);
}
