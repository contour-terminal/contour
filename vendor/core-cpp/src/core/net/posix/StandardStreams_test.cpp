// SPDX-License-Identifier: Apache-2.0
//
// The POSIX half of the standard streams: a pipe, a socket and a terminal are Pollable, read
// through the loop and never through the blocking executor, with the inherited descriptor's flags
// left as the parent set them; and a Pollable input the backend refuses is read on the executor.
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/StandardStreams.hpp>
#include <core/net/StandardStreamsTestSupport.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/StandardStreamPrimitives.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using core::async::Task;
using core::async::testing::ManualExecutor;
using core::net::EventLoop;
using core::net::IInputStream;
using core::net::InputEnd;
using core::net::NetError;
using core::net::openStandardStreams;
using core::net::StandardStreamHandles;
using core::net::StreamKind;
using core::net::StreamRead;
using core::net::streams_test::makeTestPipe;
using core::net::streams_test::OwnedHandle;
using core::platform::InvalidHandle;

namespace
{

/// A loop over the platform's default backend, kept alive with it.
struct Loop
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    EventLoop loop { *backend };
};

/// What one awaited read came to.
struct ReadOutcome
{
    std::optional<std::expected<StreamRead, NetError>> answer;
    bool isCancelled = false;
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
}

/// Writes @p text into @p fd after @p delay on @p loop, as another process would.
Task<void> feedLater(EventLoop* loop, int fd, std::string text, std::chrono::milliseconds delay)
{
    co_await loop->delay(delay);
    REQUIRE(::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
}

/// Runs @p task on @p loop, failing the case -- naming it -- where it has not finished in 5 s.
void blockOnWithin(EventLoop& loop, Task<void> task)
{
    constexpr auto Limit = std::chrono::seconds { 5 };
    if (!core::net::testing::blockOnWithin(loop, std::move(task), Limit))
        FAIL("a stream wait in \"" << Catch::getResultCapture().getCurrentTestName()
                                   << "\" took longer than 5 s");
}

/// @return @p outcome's bytes as text; the case fails where it holds no successful read.
[[nodiscard]] std::string textOf(ReadOutcome const& outcome)
{
    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    auto text = std::string {};
    for (auto const b: (*outcome.answer)->bytes)
        text.push_back(static_cast<char>(b));
    return text;
}

/// @return @p outcome's end; the case fails where it holds no successful read.
[[nodiscard]] InputEnd endOf(ReadOutcome const& outcome)
{
    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    return (*outcome.answer)->end;
}

/// One input stream over @p fd, its outputs over nothing.
[[nodiscard]] StandardStreamHandles inputOnly(int fd) noexcept
{
    return StandardStreamHandles { .input = fd, .output = InvalidHandle, .error = InvalidHandle };
}

/// Both ends of a connected stream socket pair.
struct SocketPair
{
    OwnedHandle near; ///< The end a stream reads.
    OwnedHandle far;  ///< The end the test writes.
};

[[nodiscard]] SocketPair makeSocketPair()
{
    auto fds = std::array<int, 2> {};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    return SocketPair { .near = OwnedHandle { fds[0] }, .far = OwnedHandle { fds[1] } };
}

/// A pseudo-terminal: the controlling side the test writes, and the terminal a stream reads.
struct PseudoTerminal
{
    OwnedHandle controller; ///< The master side.
    OwnedHandle terminal;   ///< The slave side: what a shell's stdin would be.
};

/// @return A new pseudo-terminal in raw mode, so a byte written is a byte read; SKIPs where the
///         host has none.
[[nodiscard]] PseudoTerminal makePseudoTerminal()
{
    auto controller = OwnedHandle { ::posix_openpt(O_RDWR | O_NOCTTY) };
    if (controller.get() < 0)
        SKIP("no pseudo-terminal on this host");
    REQUIRE(::grantpt(controller.get()) == 0);
    REQUIRE(::unlockpt(controller.get()) == 0);
    // ptsname_r, not ptsname: the latter returns a static buffer.
    auto name = std::array<char, 128> {};
    REQUIRE(::ptsname_r(controller.get(), name.data(), name.size()) == 0);
    auto terminal = OwnedHandle { ::open(name.data(), O_RDWR | O_NOCTTY) };
    REQUIRE(terminal.get() >= 0);
    auto mode = termios {};
    REQUIRE(::tcgetattr(terminal.get(), &mode) == 0);
    ::cfmakeraw(&mode);
    REQUIRE(::tcsetattr(terminal.get(), TCSANOW, &mode) == 0);
    return PseudoTerminal { .controller = std::move(controller), .terminal = std::move(terminal) };
}

/// Reads @p fd's input through a stream on a real loop: once with the bytes already there, once
/// parked until they arrive. Asserts nothing ever reached the blocking executor.
void readsThroughTheLoop(int readFd, int writeFd)
{
    auto loop = Loop {};
    auto blocking = ManualExecutor {};
    auto streams = openStandardStreams(loop.loop, blocking, inputOnly(readFd));

    REQUIRE(::write(writeFd, "ready", 5) == 5);
    auto ready = ReadOutcome {};
    blockOnWithin(loop.loop, readInto(streams.input.get(), 64, &ready));
    CHECK(textOf(ready) == "ready");

    auto parked = ReadOutcome {};
    blockOnWithin(
        loop.loop,
        core::net::testing::allOf(readInto(streams.input.get(), 64, &parked),
                                  feedLater(&loop.loop, writeFd, "later", std::chrono::milliseconds { 20 })));
    CHECK(textOf(parked) == "later");
    CHECK(endOf(parked) == InputEnd::More);
    CHECK(blocking.pending() == 0);
}

} // namespace

TEST_CASE("a socket, a pseudo-terminal and a FIFO are Pollable", "[net][standard-streams]")
{
    auto const pair = makeSocketPair();
    CHECK(core::net::classifyStreamHandle(pair.near.get()) == StreamKind::Pollable);

    auto const pty = makePseudoTerminal();
    CHECK(core::net::classifyStreamHandle(pty.terminal.get()) == StreamKind::Pollable);
    CHECK(::isatty(pty.terminal.get()) == 1);

    auto const pipe = makeTestPipe();
    CHECK(core::net::classifyStreamHandle(pipe.read.get()) == StreamKind::Pollable);
}

TEST_CASE("a pipe input reads through the loop, never through the blocking executor, to its end",
          "[net][standard-streams]")
{
    auto pipe = makeTestPipe();
    readsThroughTheLoop(pipe.read.get(), pipe.write.get());

    auto loop = Loop {};
    auto blocking = ManualExecutor {};
    auto streams = openStandardStreams(loop.loop, blocking, inputOnly(pipe.read.get()));
    pipe.write.reset();
    auto ended = ReadOutcome {};
    blockOnWithin(loop.loop, readInto(streams.input.get(), 64, &ended));
    CHECK(textOf(ended).empty());
    CHECK(endOf(ended) == InputEnd::Reached);
    CHECK(blocking.pending() == 0);
}

TEST_CASE("a socket input reads through the loop", "[net][standard-streams]")
{
    auto const pair = makeSocketPair();
    readsThroughTheLoop(pair.near.get(), pair.far.get());
}

TEST_CASE("a terminal input reads through the loop", "[net][standard-streams]")
{
    auto const pty = makePseudoTerminal();
    readsThroughTheLoop(pty.terminal.get(), pty.controller.get());
}

TEST_CASE("the inherited descriptor's O_NONBLOCK is neither set nor cleared", "[net][standard-streams]")
{
    // The open file description is the parent's too: a flag set here is set in its shell.
    auto const nonBlocking = GENERATE(0, O_NONBLOCK);
    auto const pipe = makeTestPipe();
    auto const before = ::fcntl(pipe.read.get(), F_GETFL);
    REQUIRE(before >= 0);
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, (before & ~O_NONBLOCK) | nonBlocking) == 0);
    auto const set = ::fcntl(pipe.read.get(), F_GETFL);

    readsThroughTheLoop(pipe.read.get(), pipe.write.get());
    CHECK(::fcntl(pipe.read.get(), F_GETFL) == set);
}

TEST_CASE("a stop during a pollable read throws, and the bytes are the next read's",
          "[net][standard-streams]")
{
    auto const pipe = makeTestPipe();
    auto loop = Loop {};
    auto blocking = ManualExecutor {};
    auto streams = openStandardStreams(loop.loop, blocking, inputOnly(pipe.read.get()));

    auto stopped = ReadOutcome {};
    blockOnWithin(loop.loop,
                  core::net::testing::anyOf(
                      readInto(streams.input.get(), 64, &stopped),
                      core::net::testing::sleepFor(&loop.loop, std::chrono::milliseconds { 20 })));
    CHECK(stopped.isCancelled);

    REQUIRE(::write(pipe.write.get(), "kept", 4) == 4);
    auto next = ReadOutcome {};
    blockOnWithin(loop.loop, readInto(streams.input.get(), 64, &next));
    CHECK(textOf(next) == "kept");
}

TEST_CASE("a pollable input whose registration the backend refuses is read on the blocking executor",
          "[net][standard-streams]")
{
    // macOS's kqueue refuses some terminals with EINVAL; epoll would refuse a regular file with
    // EPERM, were the table ever to call one Pollable.
    auto const pipe = makeTestPipe();
    auto backend = core::net::testing::ScriptedBackend {};
    backend.refuseNextAttach();
    auto loop = EventLoop { backend };
    auto blocking = ManualExecutor {};
    auto streams = openStandardStreams(loop, blocking, inputOnly(pipe.read.get()));
    REQUIRE(core::net::classifyStreamHandle(pipe.read.get()) == StreamKind::Pollable);

    REQUIRE(::write(pipe.write.get(), "first", 5) == 5);
    auto first = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &first));
    std::ignore = loop.runUntilIdle();
    REQUIRE(blocking.pending() == 1);
    blocking.drain();
    std::ignore = loop.runUntilIdle();
    CHECK(textOf(first) == "first");

    // And it stays Blocking: the backend would take the next registration, but it is not asked.
    REQUIRE(::write(pipe.write.get(), "second", 6) == 6);
    auto second = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &second));
    std::ignore = loop.runUntilIdle();
    REQUIRE(blocking.pending() == 1);
    blocking.drain();
    std::ignore = loop.runUntilIdle();
    CHECK(textOf(second) == "second");
}

TEST_CASE("a refused input over a non-blocking descriptor waits in the worker, not in the loop",
          "[net][standard-streams][threading]")
{
    // The parent made the descriptor non-blocking, and it is not this process's to change back: the
    // worker's read finds nothing yet and must wait for the bytes rather than answer EAGAIN.
    constexpr auto Budget = std::chrono::seconds { 10 };
    auto pipe = makeTestPipe();
    auto const flags = ::fcntl(pipe.read.get(), F_GETFL);
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, flags | O_NONBLOCK) == 0);
    auto backend = core::net::testing::ScriptedBackend {};
    backend.refuseNextAttach();
    auto loop = EventLoop { backend };
    auto pool = core::async::ThreadPoolExecutor { 1 };
    auto streams = openStandardStreams(loop, pool, inputOnly(pipe.read.get()));
    // On the way out, a failed REQUIRE's included: the write end closes first, so a worker still
    // blocked in its read sees the end and the pool can be joined, rather than hanging the binary.
    auto const release = core::net::detail::ScopeGuard { [&]() noexcept {
        pipe.write.reset();
        pool.stop();
    } };

    auto outcome = ReadOutcome {};
    auto isFinished = std::atomic<bool> { false };
    auto const flow = [](IInputStream* input, ReadOutcome* out, std::atomic<bool>* done) -> Task<void> {
        co_await readInto(input, 64, out);
        done->store(true, std::memory_order_release);
    };
    loop.spawn(flow(streams.input.get(), &outcome, &isFinished));
    // One turn first, so the refusal sends the read to the worker while the pipe is still empty.
    std::ignore = loop.runUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    REQUIRE_FALSE(isFinished.load(std::memory_order_acquire));
    REQUIRE(::write(pipe.write.get(), "eventually", 10) == 10);

    auto const deadline = std::chrono::steady_clock::now() + Budget;
    while (!isFinished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        if (loop.runUntilIdle() == 0)
            std::this_thread::yield();
    }
    REQUIRE(isFinished.load(std::memory_order_acquire));
    CHECK(textOf(outcome) == "eventually");
    CHECK(::fcntl(pipe.read.get(), F_GETFL) == (flags | O_NONBLOCK));
}

TEST_CASE("a readiness with nothing to read waits again instead of blocking the loop",
          "[net][standard-streams]")
{
    // A blocking descriptor the loop calls readable may still have nothing: a spurious wake, or
    // another reader. The read is confirmed first; unconfirmed, it would block the loop's thread.
    // A watchdog writes after 2 s, so a read that did block ends -- with the watchdog's bytes,
    // which the case then refuses -- rather than hanging the binary.
    // A pipe, and a terminal: macOS's poll cannot ask a tty (POLLNVAL), so the confirmation falls
    // back to select there, and this is the case that holds it to that.
    auto const kind = GENERATE(as<std::string> {}, "pipe", "pseudo-terminal");
    CAPTURE(kind);
    auto input = OwnedHandle {};
    auto feeder = OwnedHandle {};
    if (kind == "pipe")
    {
        auto pipe = makeTestPipe();
        input = std::move(pipe.read);
        feeder = std::move(pipe.write);
    }
    else
    {
        auto pty = makePseudoTerminal();
        input = std::move(pty.terminal);
        feeder = std::move(pty.controller);
    }
    auto backend = core::net::testing::ScriptedBackend {};
    auto loop = EventLoop { backend };
    auto blocking = ManualExecutor {};
    auto streams = openStandardStreams(loop, blocking, inputOnly(input.get()));

    auto mutex = std::mutex {};
    auto finished = std::condition_variable {};
    auto isDone = false;
    auto watchdog = std::thread { [&] {
        auto lock = std::unique_lock { mutex };
        if (!finished.wait_for(lock, std::chrono::seconds { 2 }, [&] { return isDone; }))
            std::ignore = ::write(feeder.get(), "watchdog", 8);
    } };
    auto const joinWatchdog = core::net::detail::ScopeGuard { [&]() noexcept {
        {
            auto const lock = std::scoped_lock { mutex };
            isDone = true;
        }
        finished.notify_all();
        watchdog.join();
    } };

    // One turn at a time, each with the one step the script gives it: the flow parks, then is told
    // its descriptor is readable while the pipe is empty, then the bytes really arrive.
    // Each step is consumed by exactly one backend wait, and a turn that has ready work may not
    // wait at all, so a step is driven until a wait has taken it.
    auto const step = [&](auto push) {
        auto const consumed = backend.waitCount() + 1;
        push();
        // Bounded: a loop with nothing parked returns without asking the backend at all.
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 100))
        {
            if (backend.waitCount() >= consumed)
                break;
            std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
        }
    };
    auto const idle = [&] {
        backend.pushTimeout();
    };
    auto outcome = ReadOutcome {};
    loop.spawn(readInto(streams.input.get(), 64, &outcome));
    step(idle);
    auto const parked = backend.lastHandlerId();
    REQUIRE(parked);
    REQUIRE(backend.attachedCount() > 0);

    auto const started = std::chrono::steady_clock::now();
    step([&] { backend.pushReadable(parked); });
    step(idle);
    step(idle);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds { 1 });
    REQUIRE_FALSE(outcome.answer.has_value());
    CHECK(blocking.pending() == 0);

    REQUIRE(::write(feeder.get(), "real", 4) == 4);
    step([&] { backend.pushReadable(backend.lastHandlerId()); });
    step(idle);
    step(idle);
    CHECK(textOf(outcome) == "real");
}

TEST_CASE("a write turns the description's SIGPIPE off for good where the system has the flag",
          "[net][standard-streams]")
{
    // macOS raises a pipe's SIGPIPE at the process, so blocking it on the writing thread is not
    // enough. The description's flag is set once and stays set: a save and a restore around each
    // write raced with every other writer of the shared description.
#ifdef F_GETNOSIGPIPE
    auto pipe = makeTestPipe();
    auto const byte = std::array { std::byte { 'x' } };
    REQUIRE(core::net::detail::writeStream(pipe.write.get(), byte).has_value());
    CHECK(::fcntl(pipe.write.get(), F_GETNOSIGPIPE) == 1);
#else
    SKIP("SIGPIPE goes to the writing thread here, and that thread blocks it");
#endif
}
