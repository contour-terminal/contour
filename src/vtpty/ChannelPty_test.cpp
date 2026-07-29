// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChannelPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

// A pane whose reader is never momentarily idle -- the ordinary case for a daemon-backed or
// tmux-mirrored pane, fed from the reactor thread while the parser pump is mid-read -- must not
// grow the output buffer without bound. Reclaiming only on a FULL drain bounded nothing: every byte
// the pane had ever produced stayed resident.
TEST_CASE("feed reclaims the consumed prefix instead of growing without bound", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    auto pool = crispy::buffer_object_pool<char> { 4096 };
    auto const storage = pool.allocateBufferObject();

    constexpr std::size_t ChunkSize = 4096;
    constexpr std::size_t Rounds = 400; // ~1.6 MiB total, far past the compaction threshold
    auto const chunk = std::string(ChunkSize, 'x');

    auto consumed = std::size_t { 0 };
    auto peak = std::size_t { 0 };
    for (auto round = std::size_t { 0 }; round < Rounds; ++round)
    {
        pty.feed(chunk);
        // Read all but ONE byte of what is buffered, so the reader keeps up (the backlog stays a
        // handful of bytes) yet the offset never reaches size() — the full-drain reset can
        // therefore never fire, which is exactly the streaming pane's situation. Anything the
        // buffer holds beyond the tiny backlog is consumed prefix that was not reclaimed.
        auto const result = pty.read(*storage, 100ms, ChunkSize - 1);
        REQUIRE(result.has_value());
        consumed += result->data.size();
        peak = std::max(peak, pty.bufferedOutputBytes());
    }

    // Bounded by the compaction threshold plus one round's feed, not by the ~1.6 MiB fed. The
    // exact constant is deliberately loose — the property is that the bound is a CONSTANT at all.
    CHECK(peak < std::size_t { 128 } * 1024);
    CHECK(consumed == Rounds * (ChunkSize - 1));

    // Compaction must not lose or reorder bytes: everything still unread is there to be read.
    auto remaining = std::size_t { 0 };
    while (pty.isStdoutPending())
    {
        auto const result = pty.read(*storage, 100ms, ChunkSize);
        REQUIRE(result.has_value());
        remaining += result->data.size();
    }
    CHECK(consumed + remaining == Rounds * ChunkSize);
}

// The byte STREAM must survive compaction intact, not merely its length.
TEST_CASE("feed preserves byte order across a compaction", "[vtpty][channelpty]")
{
    auto pty = ChannelPty { testPageSize() };
    auto pool = crispy::buffer_object_pool<char> { std::size_t { 1024 } * 1024 };
    auto const storage = pool.allocateBufferObject();

    auto expected = std::string {};
    auto seen = std::string {};
    for (auto round = 0; round < 40; ++round)
    {
        auto const chunk = std::string(4096, static_cast<char>('a' + (round % 26)));
        expected += chunk;
        pty.feed(chunk);
        auto const result = pty.read(*storage, 100ms, 3000); // partial: leaves a prefix behind
        REQUIRE(result.has_value());
        seen += std::string { result->data };
    }
    while (pty.isStdoutPending())
    {
        auto const result = pty.read(*storage, 100ms, 8192);
        REQUIRE(result.has_value());
        seen += std::string { result->data };
    }
    CHECK(seen == expected);
}
