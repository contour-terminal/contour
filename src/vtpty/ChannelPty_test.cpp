// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChannelPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using vtpty::ChannelPty;
using vtpty::PageSize;

namespace
{

PageSize testPageSize()
{
    return PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
}

std::string readAll(ChannelPty& pty, std::optional<std::chrono::milliseconds> timeout)
{
    auto pool = crispy::buffer_object_pool<char> { 4096 };
    auto const storage = pool.allocateBufferObject();
    auto const result = pty.read(*storage, timeout, 4096);
    if (!result)
        return "(EAGAIN)";
    return std::string { result->data };
}

} // namespace

TEST_CASE("read returns fed data and blocks across feeds", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    pty.feed("hello");
    CHECK(pty.isStdoutPending());
    CHECK(readAll(pty, 100ms) == "hello");
    CHECK(!pty.isStdoutPending());

    // A concurrent feed unblocks a reader waiting on an empty buffer.
    auto feeder = std::thread { [&pty] {
        std::this_thread::sleep_for(20ms);
        pty.feed("later");
    } };
    CHECK(readAll(pty, 5000ms) == "later");
    feeder.join();
}

TEST_CASE("an empty buffer times out with EAGAIN instead of EOF", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    CHECK(readAll(pty, 5ms) == "(EAGAIN)");
}

TEST_CASE("a bare wakeupReader unblocks the reader with EAGAIN", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    auto waker = std::thread { [&pty] {
        std::this_thread::sleep_for(20ms);
        pty.wakeupReader();
    } };
    CHECK(readAll(pty, 5000ms) == "(EAGAIN)");
    waker.join();
}

TEST_CASE("EOF is reported only once closed and drained", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    pty.feed("tail");
    pty.close();
    CHECK(pty.isClosed());
    // The fed data still drains first; only then does read report EOF.
    CHECK(readAll(pty, 100ms) == "tail");
    CHECK(readAll(pty, 100ms).empty());
    pty.waitForClosed();
}

TEST_CASE("writes buffer when no sink is set and route to the sink otherwise", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    CHECK(pty.write("buffered") == 8);
    CHECK(pty.stdinSnapshot() == "buffered");

    auto sunk = std::vector<std::string> {};
    pty.setWriteSink([&sunk](std::string_view data) { sunk.emplace_back(data); });
    CHECK(pty.write("routed") == 6);
    REQUIRE(sunk.size() == 1);
    CHECK(sunk.front() == "routed");
    // Sink-routed writes do not accumulate in the buffer.
    CHECK(pty.stdinSnapshot() == "buffered");
}

TEST_CASE("resize updates the page size and notifies the sink", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    auto seen = std::vector<PageSize> {};
    pty.setResizeSink(
        [&seen](PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) { seen.push_back(cells); });

    auto const newSize = PageSize { vtpty::LineCount(50), vtpty::ColumnCount(120) };
    pty.resizeScreen(newSize);

    CHECK(pty.pageSize() == newSize);
    REQUIRE(seen.size() == 1);
    CHECK(seen.front() == newSize);
    std::ignore = pty.slave();
}
