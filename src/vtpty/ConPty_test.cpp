// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <future>
#include <optional>
#include <ranges>

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
