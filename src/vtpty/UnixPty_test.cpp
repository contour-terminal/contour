// SPDX-License-Identifier: Apache-2.0
#include <vtpty/OwnedFd.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/UnixPty.hpp>
#include <vtpty/UnixUtils.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/ioctl.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <termios.h>
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

/// Closes a PTY when it goes out of scope, so that a thread waiting for the close ends however the test does.
///
/// Not gsl::finally: the tree pins Microsoft GSL 3.1.0, which spells that header `gsl/gsl_util`, while
/// GSL 4.x spells it `gsl/util`, so no single include builds everywhere. close() is idempotent.
class ClosingPty
{
  public:
    explicit ClosingPty(vtpty::UnixPty& pty) noexcept: _pty { pty } {}
    ClosingPty(ClosingPty const&) = delete;
    ClosingPty(ClosingPty&&) = delete;
    ClosingPty& operator=(ClosingPty const&) = delete;
    ClosingPty& operator=(ClosingPty&&) = delete;
    ~ClosingPty() { _pty.close(); }

  private:
    vtpty::UnixPty& _pty;
};

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

TEST_CASE("UnixPty.resizeScreen.racesAConcurrentClose", "[vtpty][unixpty][threading]")
{
    // The session's parser thread closes the PTY the moment its shell exits, while the GUI thread may be
    // resizing it -- a window that lays itself out as its shell exits at once. A resize that read the
    // descriptor before close() and issued TIOCSWINSZ after it failed with EBADF and threw from a Qt event
    // handler, which ended the process. Each round closes while resizes run; none may throw.
    constexpr auto Rounds = 200;
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        auto pty = vtpty::UnixPty {
            vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) }, std::nullopt
        };
        REQUIRE(pty.start().has_value());

        auto started = std::atomic<bool> { false };
        auto resizing = std::future<void> {};
        // Declared after the future, so that it runs before the future is destroyed, which waits for the
        // resizer: a check that fails early must fail the test, not leave it waiting for a close.
        auto const closeOnExit = ClosingPty { pty };
        resizing = std::async(std::launch::async, [&pty, &started] {
            auto lines = 10;
            while (!pty.isClosed())
            {
                pty.resizeScreen(
                    vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(80) });
                lines = lines == 10 ? 11 : 10;
                started.store(true, std::memory_order_release);
            }
        });
        // Also done waiting where the resizer ended without one resize: it threw on the first.
        while (!started.load(std::memory_order_acquire) && resizing.wait_for(0s) != std::future_status::ready)
            std::this_thread::yield();
        pty.close();
        CHECK_NOTHROW(resizing.get());
    }
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

namespace
{

/// Opens a second descriptor on @p pty's slave side, as the program running in it holds one.
/// @return The descriptor, closed when it goes out of scope, even past a failed REQUIRE; none on failure.
[[nodiscard]] vtpty::OwnedFd openSlaveOf(vtpty::UnixPty const& pty)
{
    auto path = std::array<char, 128> {};
    if (::ptsname_r(unbox<int>(pty.handle()), path.data(), path.size()) != 0)
        return vtpty::OwnedFd { -1 };
    return vtpty::OwnedFd { ::open(path.data(), O_RDWR | O_NOCTTY) };
}

/// Changes the slave's local flags as a program in the terminal would.
/// @param slave The slave descriptor.
/// @param clear The local flags to clear.
void clearLocalFlags(int slave, tcflag_t clear)
{
    auto tio = termios {};
    REQUIRE(::tcgetattr(slave, &tio) == 0);
    tio.c_lflag &= ~clear;
    REQUIRE(::tcsetattr(slave, TCSANOW, &tio) == 0);
}

} // namespace

TEST_CASE("UnixPty.inputEcho.followsTheSlavesTermios", "[vtpty][unixpty][inputecho]")
{
    // The GUI reads the echo from the master; the program in the terminal changes it on the slave. Both
    // descriptors name one terminal, so the master sees each change: sudo turning echo off reads Hidden,
    // an editor entering raw mode reads Raw.
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) },
                         std::nullopt };
    CHECK(pty.inputEcho() == vtpty::InputEcho::Unknown);
    REQUIRE(pty.start().has_value());
    auto slave = openSlaveOf(pty);
    REQUIRE(slave.get() >= 0);

    CHECK(pty.inputEcho() == vtpty::InputEcho::Echoed);
    clearLocalFlags(slave.get(), ECHO);
    CHECK(pty.inputEcho() == vtpty::InputEcho::Hidden);
    clearLocalFlags(slave.get(), ICANON);
    CHECK(pty.inputEcho() == vtpty::InputEcho::Raw);

    slave.reset();
    pty.close();
    CHECK(pty.inputEcho() == vtpty::InputEcho::Unknown);
}

TEST_CASE("UnixPty.adopt.readsWhatTheChildWrites", "[vtpty][unixpty][handover]")
{
    // A pty adopted from its master fd reads what the child writes, takes input and resizes -- and the
    // original object can go without closing the descriptor or killing the child.
    auto const size = vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };
    auto adopted = std::unique_ptr<vtpty::UnixPty> {};
    auto child = std::optional<vtpty::AdoptedChild> {};
    {
        auto original = vtpty::testing::unplacedProcess(
            vtpty::testing::execInfo("/bin/sh", { "-c", "echo ready; exec cat" }), size);
        REQUIRE(original.start().has_value());
        auto* const unixPty = dynamic_cast<vtpty::UnixPty*>(&original.pty());
        REQUIRE(unixPty != nullptr);
        // The child flushes the terminal as it starts, which would eat what is written before it ran.
        REQUIRE(vtpty::testing::printed(original, "ready"));

        auto const released = unixPty->releaseHandover();
        REQUIRE(released.has_value());
        auto const& inherited = *released;
        CHECK(unixPty->isClosed());
        child = original.releaseChild();
        REQUIRE(child.has_value());
        // As a hand-off leaves it: inheritable, which the adopter undoes.
        REQUIRE(vtpty::util::setExecInheritance(inherited.masterFd, vtpty::util::ExecInheritance::KeepOnExec)
                    .has_value());
        auto result = vtpty::UnixPty::adopt(inherited);
        REQUIRE(result.has_value());
        adopted = std::move(*result);
    } // The original Process is gone: neither the descriptor nor the child may be.

    REQUIRE(::fcntl(unbox<int>(adopted->handle()), F_GETFD) >= 0);
    CHECK((::fcntl(unbox<int>(adopted->handle()), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(::kill(static_cast<pid_t>(child->pid), 0) == 0);
    CHECK(adopted->pageSize() == size);

    adopted->write("hello\n");
    CHECK(vtpty::testing::printed(*adopted, "hello"));

    auto const resized = vtpty::PageSize { .lines = vtpty::LineCount(10), .columns = vtpty::ColumnCount(40) };
    adopted->resizeScreen(resized);
    auto ws = winsize {};
    REQUIRE(::ioctl(unbox<int>(adopted->handle()), TIOCGWINSZ, &ws) == 0);
    CHECK(ws.ws_row == 10);
    CHECK(ws.ws_col == 40);

    auto process = vtpty::Process::adopt(*child, std::move(adopted));
    REQUIRE(process.has_value());
    (*process)->terminate(vtpty::Process::TerminationHint::Kill);
    CHECK((*process)->wait().has_value());
}

TEST_CASE("UnixPty.adopt.refusesADescriptorThatIsNotOpen", "[vtpty][unixpty][handover]")
{
    auto inherited =
        vtpty::InheritedPty { .masterFd = 987, .stdoutFastPipe = std::nullopt, .pageSize = {}, .pixels = {} };
    auto const result = vtpty::UnixPty::adopt(inherited);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == vtpty::AdoptError::BadDescriptor);
}

TEST_CASE("UnixPty.write.interruptedReturnsWhatItWrote", "[vtpty][unixpty][handover]")
{
    // A write to a program that reads nothing waits for it forever, unless interrupted: a live upgrade must
    // stop the writer without the program's help, and learn exactly how much the PTY took.
    auto const size = vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };
    auto process = vtpty::testing::unplacedProcess(
        vtpty::testing::execInfo("/bin/sh", { "-c", "stty raw -echo; printf ready; exec sleep 1000" }), size);
    REQUIRE(process.start().has_value());
    REQUIRE(vtpty::testing::printed(process, "ready"));

    auto const payload = letters(std::size_t { 1024 } * 1024);
    auto writing = std::async(std::launch::async, [&] {
        auto const written = process.write(payload);
        return std::pair { written, errno };
    });
    // The program reads nothing: the write fills the PTY, then waits.
    REQUIRE(writing.wait_for(200ms) == std::future_status::timeout);

    process.interruptWrites();
    REQUIRE(writing.wait_for(5s) == std::future_status::ready);
    auto const [written, error] = writing.get();
    CHECK(written > 0);
    CHECK(std::cmp_less(written, payload.size()));
    CHECK(error == EINTR);

    // Interrupted until allowed again: a later write takes what fits, and waits for nothing.
    auto const later = std::async(std::launch::async, [&] { return process.write(payload); });
    REQUIRE(later.wait_for(5s) == std::future_status::ready);

    process.allowWrites();
    auto waiting = std::async(std::launch::async, [&] { return process.write(payload); });
    CHECK(waiting.wait_for(200ms) == std::future_status::timeout);
    process.terminate(vtpty::Process::TerminationHint::Kill);
    process.close();
    REQUIRE(waiting.wait_for(5s) == std::future_status::ready);
}

TEST_CASE("UnixPty.start.ofAnAdoptedPtyKeepsItsDescriptors", "[vtpty][unixpty][handover]")
{
    // An adopted PTY is running already: starting it must not open another one over the adopted master.
    auto const size = vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };
    auto original = vtpty::testing::unplacedProcess(
        vtpty::testing::execInfo("/bin/sh", { "-c", "echo ready; exec cat" }), size);
    REQUIRE(original.start().has_value());
    REQUIRE(vtpty::testing::printed(original, "ready"));
    auto* const unixPty = dynamic_cast<vtpty::UnixPty*>(&original.pty());
    REQUIRE(unixPty != nullptr);
    auto const released = unixPty->releaseHandover();
    REQUIRE(released.has_value());
    auto const& inherited = *released;
    auto child = original.releaseChild();
    REQUIRE(child.has_value());

    auto adopted = vtpty::UnixPty::adopt(inherited);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->start().has_value());
    CHECK(unbox<int>((*adopted)->handle()) == inherited.masterFd);

    std::ignore = (*adopted)->write("hello\n");
    CHECK(vtpty::testing::printed(**adopted, "hello"));

    auto process = vtpty::Process::adopt(*child, std::move(*adopted));
    REQUIRE(process.has_value());
    (*process)->terminate(vtpty::Process::TerminationHint::Kill);
    CHECK((*process)->wait().has_value());
}

namespace
{
/// Holds the soft descriptor limit at the lowest free descriptor while it lives, so that nothing more opens.
class DescriptorsExhausted
{
  public:
    DescriptorsExhausted()
    {
        ::getrlimit(RLIMIT_NOFILE, &_original);
        auto const lowestFree = ::dup(STDIN_FILENO);
        ::close(lowestFree);
        auto exhausted = _original;
        exhausted.rlim_cur = static_cast<rlim_t>(lowestFree);
        ::setrlimit(RLIMIT_NOFILE, &exhausted);
    }
    DescriptorsExhausted(DescriptorsExhausted const&) = delete;
    DescriptorsExhausted& operator=(DescriptorsExhausted const&) = delete;
    DescriptorsExhausted(DescriptorsExhausted&&) = delete;
    DescriptorsExhausted& operator=(DescriptorsExhausted&&) = delete;
    /// Restored before any assertion after it can unwind: every later test needs its descriptors.
    ~DescriptorsExhausted() { ::setrlimit(RLIMIT_NOFILE, &_original); }

  private:
    rlimit _original {};
};
} // namespace

TEST_CASE("a PTY that cannot be opened fails its start, not the process", "[vtpty][unixpty]")
{
    auto const pageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
    auto started = vtpty::StartResult {};
    auto pty = std::optional<vtpty::UnixPty> {};
    {
        auto const exhausted = DescriptorsExhausted {};
        // Constructed out of descriptors too: it holds none until it starts.
        pty.emplace(pageSize, std::nullopt);
        started = pty->start();
    }
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().error == vtpty::StartError::PtyAllocationFailed);
    // And it starts once descriptors are to be had again.
    CHECK(pty->start().has_value());
}

TEST_CASE("a PTY adopted out of descriptors is refused as such, not as a bad descriptor", "[vtpty][unixpty]")
{
    auto source =
        vtpty::UnixPty { vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }, std::nullopt };
    REQUIRE(source.start().has_value());
    auto const inherited = source.handover();
    REQUIRE(inherited.has_value());
    auto adopted = std::expected<std::unique_ptr<vtpty::UnixPty>, vtpty::AdoptError> {};
    {
        auto const exhausted = DescriptorsExhausted {};
        adopted = vtpty::UnixPty::adopt(*inherited);
    }
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error() == vtpty::AdoptError::OutOfDescriptors);
}

TEST_CASE("waiting for a PTY to close outlasts running out of descriptors", "[vtpty][unixpty][threading]")
{
    auto pty =
        vtpty::UnixPty { vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) }, std::nullopt };
    REQUIRE(pty.start().has_value());
    auto waited = std::future<void> {};
    auto whileOpen = std::future_status::ready;
    auto afterClose = std::future_status::timeout;
    {
        auto const exhausted = DescriptorsExhausted {};
        waited = std::async(std::launch::async, [&] { pty.waitForClosed(); });
        // Returning would mean "closed" to its caller: out of descriptors, it must keep waiting.
        whileOpen = waited.wait_for(std::chrono::milliseconds(300));
        // And a close while it has no selector still ends the wait.
        pty.close();
        afterClose = waited.wait_for(std::chrono::seconds(5));
    }
    CHECK(whileOpen == std::future_status::timeout);
    CHECK(afterClose == std::future_status::ready);
}
