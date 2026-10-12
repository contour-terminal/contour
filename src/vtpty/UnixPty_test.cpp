// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixPty.hpp>

#include <crispy/BufferObject.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

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
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 10'000))
    {
        if (flood.received.size() >= payload.size())
            break;
        auto const storage = pool.allocateBufferObject();
        auto const room = storage->bytesAvailable();
        auto const result = pty.read(*storage, 1000ms, budget);
        if (!result)
            continue; // a timeout: the writer has not caught up yet
        if (result->data.empty())
            break; // end of file: nothing more is coming
        if (result->data.size() > std::min(budget, room))
            ++flood.oversizedBatches;
        flood.received += result->data;
    }

    pty.close();
    writer.join();
    return flood;
}

} // namespace

TEST_CASE("UnixPty.read.deliversAFloodWholeAndInBoundedBatches", "[vtpty][unixpty]")
{
    // Whatever size of piece the kernel hands out, a flood written into the slave must arrive through
    // read() whole and in order, and no batch may outgrow the read budget or the free space of the
    // buffer it lands in.
    auto const payload = letters(std::size_t { 256 } * 1024);

    SECTION("the read budget bounds a batch")
    {
        auto const flood = floodThroughRead(payload, std::size_t { 16 } * 1024, std::size_t { 1024 } * 1024);
        CHECK(flood.oversizedBatches == 0);
        CHECK(flood.received.size() == payload.size());
        CHECK(std::ranges::equal(flood.received, payload));
    }

    SECTION("the buffer's free space bounds a batch")
    {
        auto const flood = floodThroughRead(payload, std::size_t { 64 } * 1024, std::size_t { 4096 });
        CHECK(flood.oversizedBatches == 0);
        CHECK(flood.received.size() == payload.size());
        CHECK(std::ranges::equal(flood.received, payload));
    }
}
