// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixPty.hpp>
#include <vtpty/UnixUtils.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

/// @return @p size bytes of lowercase letters: printable, and free of anything the line discipline
///         rewrites on its way to the master (a newline would arrive as CR LF).
std::string letters(std::size_t size)
{
    auto text = std::string(size, '\0');
    for (auto const i: std::views::iota(std::size_t { 0 }, size))
        text[i] = static_cast<char>('a' + (i % 26));
    return text;
}

/// What a flood read back through UnixPty::read() came to.
struct Flood
{
    std::string received;
    std::size_t oversizedBatches = 0; ///< Reads that returned more than the budget or the buffer's room.
    std::size_t fullBatches = 0;      ///< Reads that returned all the budget and the buffer's room allowed.
    std::size_t largestBatch = 0;     ///< The most one read returned.
};

/// Writes @p payload into a real PTY's slave side and reads it back through UnixPty::read().
/// @param budget     The read budget every read is given.
/// @param bufferSize The chunk size of the pool every read's buffer comes from.
Flood floodThroughRead(std::string const& payload, std::size_t budget, std::size_t bufferSize)
{
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) },
                         std::nullopt };
    REQUIRE(pty.start().has_value());

    // The slave side blocks while the master's buffers are full, so it writes from a thread of its
    // own. It stops at the first failed write: closing the master below fails one still pending.
    auto writer = std::thread { [&pty, &payload] {
        auto rest = std::string_view { payload };
        while (!rest.empty())
        {
            auto const written = pty.slave().write(rest);
            if (written <= 0)
                break;
            rest.remove_prefix(static_cast<std::size_t>(written));
        }
    } };

    auto pool = crispy::BufferObjectPool<char> { bufferSize };
    auto flood = Flood {};
    // Bounded by the clock rather than by a count of reads: a read() that lost bytes would otherwise
    // wait out every remaining attempt's timeout.
    auto const deadline = std::chrono::steady_clock::now() + 30s;
    while (flood.received.size() < payload.size() && std::chrono::steady_clock::now() < deadline)
    {
        auto const storage = pool.allocateBufferObject();
        auto const bound = std::min(budget, storage->bytesAvailable());
        auto const result = pty.read(*storage, 1000ms, budget);
        if (!result)
            continue; // a timeout: the writer has not caught up yet
        if (result->data.empty())
            break; // end of file: nothing more is coming
        if (result->data.size() > bound)
            ++flood.oversizedBatches;
        if (result->data.size() == bound)
            ++flood.fullBatches;
        flood.largestBatch = std::max(flood.largestBatch, result->data.size());
        flood.received += result->data;
    }

    // A writer whose bytes all arrived has returned or is about to, and is joined before the PTY closes
    // under it: its calls read the PTY's handles. Only a read loop that gave up closes first, to fail
    // the write the writer may still be blocked in.
    if (flood.received.size() == payload.size())
        writer.join();
    pty.close();
    if (writer.joinable())
        writer.join();
    return flood;
}

} // namespace

TEST_CASE("UnixPty.read.deliversAFloodWholeAndInBoundedBatches", "[vtpty][unixpty]")
{
    // Whatever size of piece the kernel hands out, a flood written into the slave must arrive through
    // read() whole and in order, and no batch may outgrow the read budget or the free space of the
    // buffer it lands in. Each section makes one of the two the tighter bound, well below the most the
    // kernel hands out per read (about 4 KiB), and checks that some read reached it. A 256-byte chunk
    // makes a buffer of a few hundred bytes: the pool rounds the buffer object and its storage up
    // together.
    auto const payload = letters(std::size_t { 256 } * 1024);

    SECTION("the read budget bounds a batch")
    {
        auto const flood = floodThroughRead(payload, std::size_t { 1000 }, std::size_t { 1024 } * 1024);
        INFO("largest batch: " << flood.largestBatch);
        CHECK(flood.oversizedBatches == 0);
        CHECK(flood.fullBatches > 0);
        CHECK(flood.received.size() == payload.size());
        CHECK(std::ranges::equal(flood.received, payload));
    }

    SECTION("the buffer's free space bounds a batch")
    {
        auto const flood = floodThroughRead(payload, std::size_t { 64 } * 1024, std::size_t { 256 });
        INFO("largest batch: " << flood.largestBatch);
        CHECK(flood.oversizedBatches == 0);
        CHECK(flood.fullBatches > 0);
        CHECK(flood.received.size() == payload.size());
        CHECK(std::ranges::equal(flood.received, payload));
    }
}

TEST_CASE("UnixPty.isClosed.seesAConcurrentClose", "[vtpty][unixpty][threading]")
{
    // The session's parser thread closes the PTY when the child goes away, and the GUI thread asks
    // isClosed() when it learns of it. The two hand over through a relaxed flag, which orders nothing,
    // so under the clang-tsan tree the answer must come from state close() publishes.
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) },
                         std::nullopt };
    REQUIRE(pty.start().has_value());

    auto closed = std::atomic<bool> { false };
    auto closer = std::thread { [&] {
        pty.close();
        closed.store(true, std::memory_order_relaxed);
    } };
    while (!closed.load(std::memory_order_relaxed))
        std::this_thread::yield();
    CHECK(pty.isClosed());
    closer.join();
}

TEST_CASE("UnixPty.close.isNotHeldUpByAParkedWrite", "[vtpty][unixpty][threading]")
{
    // A write parks once the application leaves the PTY's buffers full, and it held the lock close() and
    // isClosed() take: closing the PTY of a program that reads nothing -- the very way to be rid of it --
    // waited for that program forever, and the thread closing it with it. close() wakes the parked write
    // instead, which ends short, and isClosed() takes no lock.
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) },
                         std::nullopt };
    REQUIRE(pty.start().has_value());

    // No child reads the slave side: a megabyte is far more than the PTY buffers. Lines, which the slave's
    // line discipline keeps for a reader; text with no newline it throws away past its line buffer instead,
    // and that write would never wait.
    auto payload = std::string {};
    for ([[maybe_unused]] auto const line: std::views::iota(0, 16 * 1024))
        payload += letters(63) + "\n";
    auto writing = std::async(std::launch::async, [&pty, &payload] { return pty.write(payload); });
    REQUIRE(writing.wait_for(500ms) == std::future_status::timeout);

    auto const deadline = std::chrono::steady_clock::now() + 5s;
    auto closing = std::async(std::launch::async, [&pty] { pty.close(); });
    auto asking = std::async(std::launch::async, [&pty] { return pty.isClosed(); });
    auto const closed = closing.wait_until(deadline);
    auto const asked = asking.wait_until(deadline);
    // Where they wait, a reader on the slave side lets the parked write through, and them after it.
    if (closed != std::future_status::ready || asked != std::future_status::ready)
        vtpty::testing::drainSlave(pty.handle(),
                                   [&closing] { return closing.wait_for(0s) == std::future_status::ready; });
    closing.get();
    CHECK(closed == std::future_status::ready);
    CHECK(asked == std::future_status::ready);
    CHECK(pty.isClosed());

    REQUIRE(writing.wait_for(5s) == std::future_status::ready);
    auto const written = writing.get();
    CHECK(written >= 0);
    CHECK(std::cmp_less(written, payload.size()));
}

TEST_CASE("util.setNonBlocking.keepsTheOtherStatusFlags", "[vtpty][unixutils]")
{
    // fcntl(F_GETFL) returns the status flags rather than writing them through its third argument. Read that
    // way they read as none, and setting O_NONBLOCK cleared every other one.
    auto fds = std::array<int, 2> {};
    REQUIRE(::pipe(fds.data()) == 0);
    REQUIRE(::fcntl(fds[1], F_SETFL, O_APPEND) == 0);

    CHECK(vtpty::util::setNonBlocking(fds[1]).has_value());
    auto const flags = ::fcntl(fds[1], F_GETFL);
    CHECK((flags & O_NONBLOCK) != 0);
    CHECK((flags & O_APPEND) != 0);

    for (auto const fd: fds)
        ::close(fd);
}

TEST_CASE("util.setCloseOnExec.setsTheDescriptorFlag", "[vtpty][unixutils]")
{
    // Close-on-exec is a descriptor flag, set with F_SETFD. Handed to F_SETFL among the status flags, it is
    // ignored, and a child the process spawns inherits the descriptor.
    auto fds = std::array<int, 2> {};
    REQUIRE(::pipe(fds.data()) == 0);

    CHECK(vtpty::util::setCloseOnExec(fds[0]).has_value());
    CHECK((::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);

    for (auto const fd: fds)
        ::close(fd);
}

TEST_CASE("UnixPty.start.keepsTheMasterFromChildren", "[vtpty][unixpty]")
{
    // A started PTY's master is close-on-exec, and non-blocking: a process the terminal's own process spawns,
    // a URL opener or a QProcess, inherits no tab's master. It inherited every one.
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) },
                         std::nullopt };
    REQUIRE(pty.start().has_value());
    auto const master = unbox<int>(pty.handle());
    CHECK((::fcntl(master, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(master, F_GETFL) & O_NONBLOCK) != 0);
}

TEST_CASE("util.applyCreationFlags.setsWhatPipe2WouldHave", "[vtpty][unixutils]")
{
    // Where there is no pipe2(), a pipe is made with pipe() and handed the flags afterwards: close-on-exec as
    // the descriptor flag, non-blocking as a status flag. Anything else pipe2() would refuse.
    auto fds = std::array<int, 2> {};
    REQUIRE(::pipe(fds.data()) == 0);

    CHECK(vtpty::util::applyCreationFlags(fds[0], O_CLOEXEC | O_NONBLOCK).has_value());
    CHECK((::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0);

    CHECK(vtpty::util::applyCreationFlags(fds[1], O_CLOEXEC).has_value());
    CHECK((::fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(fds[1], F_GETFL) & O_NONBLOCK) == 0);

    CHECK(vtpty::util::applyCreationFlags(fds[1], O_APPEND).error() == std::errc::invalid_argument);

    for (auto const fd: fds)
        ::close(fd);
}
