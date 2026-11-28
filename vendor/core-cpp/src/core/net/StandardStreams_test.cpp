// SPDX-License-Identifier: Apache-2.0
//
// The standard streams over handles each case makes for itself -- a file, the null device, a pipe
// -- and never over the test process's own stdio: a regular file is read on the blocking executor,
// a stop loses no input, one read and one write are in flight at a time, and writes land in order.
// What only POSIX has -- a pipe, a socket or a terminal waited for on the loop -- is in
// posix/StandardStreams_test.cpp.
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>
#include <core/net/StandardStreams.hpp>
#include <core/net/StandardStreamsTestSupport.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using core::async::Task;
using core::async::testing::ManualExecutor;
using core::net::EventLoop;
using core::net::IInputStream;
using core::net::InputEnd;
using core::net::IOutputStream;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::openStandardStreams;
using core::net::StandardStreamHandles;
using core::net::StreamKind;
using core::net::StreamRead;
using core::net::streams_test::makeTestPipe;
using core::net::streams_test::openForReading;
using core::net::streams_test::openForWriting;
using core::net::streams_test::openNullDevice;
using core::net::testing::TestLoop;
using core::platform::InvalidHandle;

namespace
{

/// What one awaited read came to: nothing yet, an answer (possibly a failure), or a stop.
struct ReadOutcome
{
    std::optional<std::expected<StreamRead, NetError>> answer;
    bool isCancelled = false;
    std::atomic<bool> isFinished { false };
};

/// What one awaited write came to.
struct WriteOutcome
{
    std::optional<std::expected<std::size_t, NetError>> answer;
    bool isCancelled = false;
    bool isFinished = false;
};

/// Reads at most @p maxBytes from @p input and records how it ended, a stop included.
Task<void> readInto(IInputStream* input, std::size_t maxBytes, ReadOutcome* out)
{
    try
    {
        out->answer = co_await input->read(maxBytes);
    }
    catch (core::async::OperationCancelled const&)
    {
        out->isCancelled = true;
    }
    out->isFinished.store(true, std::memory_order_release);
}

/// Writes @p text to @p output and records how it ended.
Task<void> writeInto(IOutputStream* output, std::string text, WriteOutcome* out)
{
    try
    {
        auto bytes = std::vector<std::byte> {};
        for (auto const c: text)
            bytes.push_back(static_cast<std::byte>(c));
        out->answer = co_await output->write(std::move(bytes));
    }
    catch (core::async::OperationCancelled const&)
    {
        out->isCancelled = true;
    }
    out->isFinished = true;
}

/// Sets @p ran on one of @p executor's threads.
core::async::DetachedTask runOnExecutor(core::async::IExecutor* executor, std::atomic<bool>* ran)
{
    co_await core::async::ResumeOn { *executor };
    ran->store(true, std::memory_order_release);
}

/// @return @p bytes as text.
[[nodiscard]] std::string textOf(std::vector<std::byte> const& bytes)
{
    auto text = std::string {};
    for (auto const b: bytes)
        text.push_back(static_cast<char>(b));
    return text;
}

/// @return @p outcome's bytes as text; the case fails where it holds no successful read.
[[nodiscard]] std::string textOf(ReadOutcome const& outcome)
{
    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    return textOf((*outcome.answer)->bytes);
}

/// @return @p outcome's end; the case fails where it holds no successful read.
[[nodiscard]] InputEnd endOf(ReadOutcome const& outcome)
{
    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    return (*outcome.answer)->end;
}

/// Writes @p text to a new file at @p path.
void writeFile(std::filesystem::path const& path, std::string_view text)
{
    auto file = std::ofstream { path, std::ios::binary };
    file << text;
}

/// @return What the file at @p path holds.
[[nodiscard]] std::string readFile(std::filesystem::path const& path)
{
    // Read by size, not through istreambuf_iterator, whose inlined buffer pointers GCC 14 calls a
    // potential null dereference at -O2.
    auto text = std::string(static_cast<std::size_t>(std::filesystem::file_size(path)), '\0');
    auto file = std::ifstream { path, std::ios::binary };
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

/// Writes @p text into @p handle with the platform's plain write, as a parent process would.
void feed(core::platform::NativeHandle handle, std::string_view text)
{
    REQUIRE(core::platform::platformWrite(handle, text.data(), text.size())
            == static_cast<std::intptr_t>(text.size()));
}

/// One input stream over @p input, its outputs over nothing.
[[nodiscard]] StandardStreamHandles inputOnly(core::platform::NativeHandle input) noexcept
{
    return StandardStreamHandles { .input = input, .output = InvalidHandle, .error = InvalidHandle };
}

/// A hand-driven loop and blocking executor, and a read that runs across both.
struct Driven
{
    Driven() = default;

    core::platform::ManualClock clock;
    TestLoop loop { clock };
    ManualExecutor blocking;

    /// Starts a read, runs what the blocking executor was given, and lets the loop deliver it.
    /// @return The finished read; the case fails where it did not finish.
    void readNow(IInputStream& input, std::size_t maxBytes, ReadOutcome& outcome)
    {
        loop.spawn(readInto(&input, maxBytes, &outcome));
        loop.drain();
        blocking.drain();
        loop.drain();
        REQUIRE(outcome.isFinished.load());
    }

    /// Writes @p text, running the blocking executor and the loop until it is answered.
    void writeNow(IOutputStream& output, std::string text, WriteOutcome& outcome)
    {
        loop.spawn(writeInto(&output, std::move(text), &outcome));
        loop.drain();
        blocking.drain();
        loop.drain();
        REQUIRE(outcome.isFinished);
    }
};

} // namespace

TEST_CASE("a regular file and the null device are Blocking, and a pipe is what its OS allows",
          "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "file", "x");
    auto const file = openForReading(directory / "file");
    auto const null = openNullDevice();
    auto const pipe = makeTestPipe();

    CHECK(core::net::classifyStreamHandle(file.get()) == StreamKind::Blocking);
    CHECK(core::net::classifyStreamHandle(null.get()) == StreamKind::Blocking);
    CHECK(core::net::classifyStreamHandle(pipe.read.get()) == core::net::streams_test::pipeStreamKind());
    CHECK(core::net::classifyStreamHandle(pipe.write.get()) == core::net::streams_test::pipeStreamKind());
    CHECK(core::net::classifyStreamHandle(InvalidHandle) == StreamKind::Blocking);
}

TEST_CASE("the process's standard handles are the platform's", "[net][standard-streams]")
{
    // Only compared, never read or written: the test process's own stdio is not the case's.
    auto const handles = core::net::processStandardHandles();
    CHECK(handles.input == core::platform::standardInput());
    CHECK(handles.output == core::platform::standardOutput());
    CHECK(handles.error == core::platform::standardError());
}

TEST_CASE("a regular-file input parks on the blocking executor and reads to its end",
          "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "hello world");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()));

    auto first = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 5, &first));
    driven.loop.drain();
    // Parked, and on the executor: the loop's thread has not read the file.
    REQUIRE(driven.blocking.pending() == 1);
    CHECK_FALSE(first.isFinished.load());
    driven.blocking.drain();
    CHECK_FALSE(first.isFinished.load()); // answered, but delivered by the loop
    driven.loop.drain();
    REQUIRE(first.isFinished.load());
    CHECK(textOf(first) == "hello");
    CHECK(endOf(first) == InputEnd::More);

    auto second = ReadOutcome {};
    driven.readNow(*streams.input, 64, second);
    CHECK(textOf(second) == " world");
    CHECK(endOf(second) == InputEnd::More);

    auto last = ReadOutcome {};
    driven.readNow(*streams.input, 64, last);
    CHECK(textOf(last).empty());
    CHECK(endOf(last) == InputEnd::Reached);
}

TEST_CASE("the null device as input ends at once, through the blocking executor", "[net][standard-streams]")
{
    // `stdin < /dev/null`: epoll refuses the null device, so it must never reach the loop.
    auto const null = openNullDevice();
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(null.get()));

    auto outcome = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 16, &outcome));
    driven.loop.drain();
    REQUIRE(driven.blocking.pending() == 1);
    driven.blocking.drain();
    driven.loop.drain();
    REQUIRE(outcome.isFinished.load());
    CHECK(textOf(outcome).empty());
    CHECK(endOf(outcome) == InputEnd::Reached);
}

TEST_CASE("a read is clamped to maxReadChunk, and a read of nothing reads nothing", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "abcdefgh");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()), 3);

    auto none = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 0, &none));
    driven.loop.drain();
    REQUIRE(none.isFinished.load());
    CHECK(driven.blocking.pending() == 0);
    CHECK(textOf(none).empty());
    CHECK(endOf(none) == InputEnd::More);

    auto clamped = ReadOutcome {};
    driven.readNow(*streams.input, static_cast<std::size_t>(-1), clamped);
    CHECK(textOf(clamped) == "abc");

    auto next = ReadOutcome {};
    driven.readNow(*streams.input, 2, next);
    CHECK(textOf(next) == "de");
}

TEST_CASE("a zero maxReadChunk is taken as one", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "ab");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()), 0);

    auto outcome = ReadOutcome {};
    driven.readNow(*streams.input, 64, outcome);
    CHECK(textOf(outcome) == "a");
}

TEST_CASE("a stop during a parked blocking read throws, and the next read returns the late bytes",
          "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "late bytes");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()));

    auto source = core::async::StopSource {};
    auto stopped = ReadOutcome {};
    auto flow = readInto(streams.input.get(), 64, &stopped);
    flow.handle().promise().setStopToken(source.get_token());
    flow.handle().resume();
    REQUIRE(driven.blocking.pending() == 1);
    source.request_stop();
    driven.loop.drain();
    REQUIRE(stopped.isFinished.load());
    CHECK(stopped.isCancelled);
    CHECK_FALSE(stopped.answer.has_value());

    SECTION("the stop reaches the worker while it is still queued")
    {
        // It must not read for a reader that is gone: it answers without touching the file, and the
        // next read starts a worker of its own and takes no late answer.
        driven.blocking.drain();
        CHECK(driven.loop.drain() == 0);

        auto resumed = ReadOutcome {};
        driven.loop.spawn(readInto(streams.input.get(), 64, &resumed));
        driven.loop.drain();
        CHECK_FALSE(resumed.isFinished.load());
        REQUIRE(driven.blocking.pending() == 1); // a fresh worker read
        driven.blocking.drain();
        driven.loop.drain();
        REQUIRE(resumed.isFinished.load());
        CHECK(textOf(resumed) == "late bytes");
        CHECK(endOf(resumed) == InputEnd::More);
    }

    SECTION("the next read asks for less than the worker it joins")
    {
        // The worker reads 64 bytes' worth for the reader that joined it, which asked for 4; the
        // rest is kept for the read after, never dropped.
        auto shorter = ReadOutcome {};
        driven.loop.spawn(readInto(streams.input.get(), 4, &shorter));
        driven.loop.drain();
        REQUIRE(driven.blocking.pending() == 1); // the same worker, still queued
        driven.blocking.drain();
        driven.loop.drain();
        REQUIRE(shorter.isFinished.load());
        CHECK(textOf(shorter) == "late");

        auto rest = ReadOutcome {};
        driven.loop.spawn(readInto(streams.input.get(), 64, &rest));
        driven.loop.drain();
        REQUIRE(rest.isFinished.load());
        CHECK(driven.blocking.pending() == 0);
        CHECK(textOf(rest) == " bytes");
    }

    SECTION("the next read starts while the worker is still out")
    {
        // It waits for the same worker read rather than starting a second one beside it.
        auto resumed = ReadOutcome {};
        driven.loop.spawn(readInto(streams.input.get(), 64, &resumed));
        driven.loop.drain();
        CHECK_FALSE(resumed.isFinished.load());
        CHECK(driven.blocking.pending() == 1);
        driven.blocking.drain();
        driven.loop.drain();
        REQUIRE(resumed.isFinished.load());
        CHECK(textOf(resumed) == "late bytes");
    }

    auto last = ReadOutcome {};
    driven.readNow(*streams.input, 64, last);
    CHECK(endOf(last) == InputEnd::Reached);
}

TEST_CASE("a blocking executor dropping a queued read answers it Cancelled", "[net][standard-streams]")
{
    // A job its executor destroys unrun is freed, not run: the read's guard is what keeps the
    // waiter from being stranded.
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "never read");
    auto const file = openForReading(directory / "input");
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = std::make_unique<ManualExecutor>();
    auto streams = openStandardStreams(loop, *blocking, inputOnly(file.get()));

    auto outcome = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &outcome));
    loop.drain();
    REQUIRE(blocking->pending() == 1);
    blocking.reset();
    loop.drain();
    REQUIRE(outcome.isFinished.load());
    REQUIRE(outcome.answer.has_value());
    REQUIRE_FALSE(outcome.answer->has_value());
    CHECK(outcome.answer->error().code == NetErrorCode::Cancelled);
}

TEST_CASE("a read of a flow already stopped throws and starts no blocking read", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "untouched");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()));

    driven.loop.rootStopSource().request_stop();
    auto outcome = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 64, &outcome));
    driven.loop.drain();
    REQUIRE(outcome.isFinished.load());
    CHECK(outcome.isCancelled);
    CHECK(driven.blocking.pending() == 0);
}

TEST_CASE("a second read while one is in flight is BadHandle, and an answered read frees the slot",
          "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    writeFile(directory / "input", "one");
    auto const file = openForReading(directory / "input");
    auto driven = Driven {};
    auto streams = openStandardStreams(driven.loop, driven.blocking, inputOnly(file.get()));

    auto first = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 64, &first));
    driven.loop.drain();
    REQUIRE(driven.blocking.pending() == 1);

    auto second = ReadOutcome {};
    driven.loop.spawn(readInto(streams.input.get(), 64, &second));
    driven.loop.drain();
    REQUIRE(second.isFinished.load());
    REQUIRE(second.answer.has_value());
    REQUIRE_FALSE(second.answer->has_value());
    CHECK(second.answer->error().code == NetErrorCode::BadHandle);
    CHECK(second.answer->error().systemCode == 0);
    CHECK(driven.blocking.pending() == 1); // the refusal started no second worker read

    driven.blocking.drain();
    driven.loop.drain();
    REQUIRE(first.isFinished.load());
    CHECK(textOf(first) == "one");

    auto third = ReadOutcome {};
    driven.readNow(*streams.input, 64, third);
    CHECK(endOf(third) == InputEnd::Reached);

    SECTION("an unstarted read holds the slot until it is destroyed")
    {
        auto unstarted = std::optional<Task<std::expected<StreamRead, NetError>>> { streams.input->read(1) };
        auto refused = ReadOutcome {};
        driven.loop.spawn(readInto(streams.input.get(), 1, &refused));
        driven.loop.drain();
        REQUIRE(refused.answer.has_value());
        REQUIRE_FALSE(refused.answer->has_value());
        CHECK(refused.answer->error().code == NetErrorCode::BadHandle);

        unstarted.reset();
        auto allowed = ReadOutcome {};
        driven.readNow(*streams.input, 1, allowed);
        CHECK(endOf(allowed) == InputEnd::Reached);
    }
}

TEST_CASE("three writes arrive in order, each through the blocking executor", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    auto output = openForWriting(directory / "output");
    auto error = openForWriting(directory / "error");
    auto driven = Driven {};
    auto streams = openStandardStreams(
        driven.loop,
        driven.blocking,
        StandardStreamHandles { .input = InvalidHandle, .output = output.get(), .error = error.get() });

    for (auto const* text: { "one ", "two ", "three" })
    {
        auto outcome = WriteOutcome {};
        driven.loop.spawn(writeInto(streams.output.get(), text, &outcome));
        driven.loop.drain();
        REQUIRE(driven.blocking.pending() == 1); // never written from the loop's thread
        driven.blocking.drain();
        driven.loop.drain();
        REQUIRE(outcome.isFinished);
        REQUIRE(outcome.answer.has_value());
        REQUIRE(outcome.answer->has_value());
        CHECK(**outcome.answer == std::string_view { text }.size());
    }
    auto toError = WriteOutcome {};
    driven.writeNow(*streams.error, "oops", toError);
    REQUIRE(toError.answer.has_value());
    REQUIRE(toError.answer->has_value());

    output.reset();
    error.reset();
    CHECK(readFile(directory / "output") == "one two three");
    CHECK(readFile(directory / "error") == "oops");
}

TEST_CASE("a second write while one is in flight is BadHandle and writes nothing", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    auto output = openForWriting(directory / "output");
    auto driven = Driven {};
    auto streams = openStandardStreams(
        driven.loop,
        driven.blocking,
        StandardStreamHandles { .input = InvalidHandle, .output = output.get(), .error = InvalidHandle });

    auto first = WriteOutcome {};
    driven.loop.spawn(writeInto(streams.output.get(), "first", &first));
    driven.loop.drain();
    REQUIRE(driven.blocking.pending() == 1);

    auto second = WriteOutcome {};
    driven.loop.spawn(writeInto(streams.output.get(), "second", &second));
    driven.loop.drain();
    REQUIRE(second.isFinished);
    REQUIRE(second.answer.has_value());
    REQUIRE_FALSE(second.answer->has_value());
    CHECK(second.answer->error().code == NetErrorCode::BadHandle);
    CHECK(second.answer->error().systemCode == 0);
    CHECK(driven.blocking.pending() == 1);

    driven.blocking.drain();
    driven.loop.drain();
    REQUIRE(first.isFinished);

    auto third = WriteOutcome {};
    driven.writeNow(*streams.output, "+third", third);
    REQUIRE(third.answer.has_value());
    CHECK(third.answer->has_value());

    output.reset();
    CHECK(readFile(directory / "output") == "first+third");
}

TEST_CASE("a write of a stopped flow writes nothing", "[net][standard-streams]")
{
    auto const directory = core::testing::ScopedTempDir { "core-cpp-standard-streams" };
    auto output = openForWriting(directory / "output");
    auto driven = Driven {};
    auto streams = openStandardStreams(
        driven.loop,
        driven.blocking,
        StandardStreamHandles { .input = InvalidHandle, .output = output.get(), .error = InvalidHandle });

    driven.loop.rootStopSource().request_stop();
    auto outcome = WriteOutcome {};
    driven.writeNow(*streams.output, "never", outcome);
    CHECK(outcome.isCancelled);
    CHECK(driven.blocking.pending() == 0);
    output.reset();
    CHECK(readFile(directory / "output").empty());
}

TEST_CASE("a write to a pipe nobody reads is an error, not the end of the process", "[net][standard-streams]")
{
    // On POSIX the write raises SIGPIPE, whose default action ends the process: the case passing at
    // all is half of what it checks.
    auto pipe = makeTestPipe();
    pipe.read.reset();
    auto driven = Driven {};
    auto streams = openStandardStreams(
        driven.loop,
        driven.blocking,
        StandardStreamHandles { .input = InvalidHandle, .output = pipe.write.get(), .error = InvalidHandle });

    auto outcome = WriteOutcome {};
    driven.writeNow(*streams.output, "into the void", outcome);
    REQUIRE(outcome.answer.has_value());
    REQUIRE_FALSE(outcome.answer->has_value());
    CHECK(outcome.answer->error().code == NetErrorCode::SystemError);
    CHECK(outcome.answer->error().systemCode != 0);

    // And again: the first one's signal was consumed, not left pending for the next write.
    auto again = WriteOutcome {};
    driven.writeNow(*streams.output, "and again", again);
    REQUIRE(again.answer.has_value());
    CHECK_FALSE(again.answer->has_value());
}

TEST_CASE("a stopped blocking read of a pipe keeps the bytes that arrive after it",
          "[net][standard-streams][threading]")
{
    // A worker really blocked in its read. On POSIX a pipe is Pollable, so the backend refuses its
    // registration to make it Blocking; on Windows a pipe is Blocking already, and the stop cancels
    // the worker's read instead. Either way the bytes written after the stop are the next read's.
    constexpr auto Budget = std::chrono::seconds { 10 };

    auto pipe = makeTestPipe();
    auto backend = core::net::testing::ScriptedBackend {};
    backend.refuseNextAttach();
    auto loop = EventLoop { backend };
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto streams = openStandardStreams(loop, pool, inputOnly(pipe.read.get()));
    // Outlives the guard: a probe the pool still holds when a REQUIRE fails runs as it stops.
    auto probeRan = std::atomic<bool> { false };
    // On the way out, a failed REQUIRE's included: the write end closes first, so a worker still
    // blocked in its read sees the end and the pool can be joined, rather than hanging the binary.
    auto const release = core::net::detail::ScopeGuard { [&]() noexcept {
        pipe.write.reset();
        pool.stop();
    } };

    auto const runUntil = [&](ReadOutcome const& outcome) {
        auto const deadline = std::chrono::steady_clock::now() + Budget;
        while (!outcome.isFinished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline)
        {
            if (loop.runUntilIdle() == 0)
                std::this_thread::yield();
        }
        return outcome.isFinished.load(std::memory_order_acquire);
    };

    auto source = core::async::StopSource {};
    auto stopped = ReadOutcome {};
    auto flow = readInto(streams.input.get(), 64, &stopped);
    flow.handle().promise().setStopToken(source.get_token());
    flow.handle().resume();
    // Give the worker time to enter its read; the outcome does not depend on whether it has.
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    source.request_stop();
    REQUIRE(runUntil(stopped));
    CHECK(stopped.isCancelled);

    if (core::net::streams_test::runningReadStop() == core::net::streams_test::RunningReadStop::Cancels)
    {
        // The pool's one thread is free again before any input: the stop cancelled the read rather
        // than leaving it to wait for bytes. Uncancelled, the probe could not run until the feed below.
        runOnExecutor(&pool, &probeRan);
        auto const deadline = std::chrono::steady_clock::now() + Budget;
        while (!probeRan.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(probeRan.load(std::memory_order_acquire));
    }

    feed(pipe.write.get(), "after the stop");
    auto resumed = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &resumed));
    REQUIRE(runUntil(resumed));
    CHECK(textOf(resumed) == "after the stop");

    pipe.write.reset();
    auto last = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &last));
    REQUIRE(runUntil(last));
    CHECK(endOf(last) == InputEnd::Reached);
}
