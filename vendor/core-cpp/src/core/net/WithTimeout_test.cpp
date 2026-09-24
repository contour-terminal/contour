// SPDX-License-Identifier: Apache-2.0
//
// `withTimeout` over the socket contract: what a timed-out operation leaves behind.
//
// A deadline is only as good as the state it leaves when it fires. `withTimeout` wins its race by
// stopping the losing flow's token, and a socket operation parked in that flow must then give its
// slot back -- a socket has ONE read operation, so a read that is still registered after the
// timeout turns the caller's next read into a double-arm (`contract::claimReadSlot`), and a reader
// that kept half-received bytes in the wrong place hands them back corrupted. Each case below times
// out a real read on a real socket pair and then asks the socket for more.
#include <core/async/AsTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/AsyncBufferedReader.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using core::async::Task;
using core::net::AsyncBufferedReader;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::testing::BackendMatrix;
using namespace std::chrono_literals;

namespace
{

/// Long enough that a loaded CI runner does not deliver bytes the case never sent, short enough
/// that the six sections below cost well under a second.
constexpr auto RaceDeadline = 30ms;

/// @param text The bytes to view.
/// @return @p text as the byte span a socket write takes.
std::span<std::byte const> bytesOf(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// Writes @p text to @p socket in full.
/// @param socket The writer.
/// @param text The bytes; a view of a literal, so it outlives the write.
/// @return True if every byte was written.
Task<bool> send(ISocket* socket, std::string_view text)
{
    auto const written = co_await socket->write(bytesOf(text));
    co_return written.has_value() && *written == text.size();
}

/// Reads once from @p socket into @p buffer, bounded by @p deadline.
/// @param loop The loop whose clock bounds the read.
/// @param socket The reader.
/// @param buffer The destination; must outlive the returned task.
/// @param deadline How long the read may wait.
/// @return The read's result, or nullopt if the deadline fired first.
Task<std::optional<core::net::IoResult>> readWithin(EventLoop* loop,
                                                    ISocket* socket,
                                                    std::span<std::byte> buffer,
                                                    std::chrono::milliseconds deadline)
{
    co_return co_await core::net::withTimeout(loop, core::async::asTask(socket->read(buffer)), deadline);
}

/// Reads one line through @p reader, bounded by @p deadline.
/// @param loop The loop whose clock bounds the read.
/// @param reader The reader; must outlive the returned task.
/// @param deadline How long the line may take.
/// @return The line's result, or nullopt if the deadline fired first.
Task<std::optional<std::expected<std::string, core::net::NetError>>> readLineWithin(
    EventLoop* loop, AsyncBufferedReader* reader, std::chrono::milliseconds deadline)
{
    co_return co_await core::net::withTimeout(loop, reader->readLine(), deadline);
}

} // namespace

TEST_CASE("A read that loses a withTimeout race gives the socket's read slot back", "[net][timeout]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            auto buffer = std::array<std::byte, 16> {};
            auto const timedOut = loop.blockOn(readWithin(&loop, pair->first.get(), buffer, RaceDeadline));
            REQUIRE_FALSE(timedOut.has_value());
            CHECK(loop.parkedWaiterCount() == 0); // the cancelled read retired its park

            // The same socket, read again. A read still registered from the lost race would make
            // this a second arm over a parked one, which drops one of them.
            REQUIRE(loop.blockOn(send(pair->second.get(), "late")));
            auto const answered = loop.blockOn(readWithin(&loop, pair->first.get(), buffer, 5s));
            REQUIRE(answered.has_value());
            REQUIRE(answered->has_value());
            CHECK(std::string_view { reinterpret_cast<char const*>(buffer.data()), **answered } == "late");
        }
    }
}

TEST_CASE("withTimeout hands back a read that finishes first", "[net][timeout]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());

            REQUIRE(loop.blockOn(send(pair->second.get(), "early")));
            auto buffer = std::array<std::byte, 16> {};
            auto const answered = loop.blockOn(readWithin(&loop, pair->first.get(), buffer, 5s));
            REQUIRE(answered.has_value());
            REQUIRE(answered->has_value());
            CHECK(std::string_view { reinterpret_cast<char const*>(buffer.data()), **answered } == "early");
            CHECK(loop.pendingTimerCount() == 0); // the losing deadline left no timer behind
        }
    }
}

TEST_CASE("A line that times out half-received is completed intact by the next read", "[net][timeout]")
{
    // The reader has bytes of the line when the deadline fires, and a refill parked for the rest.
    // Cancelling that refill must leave the reader holding exactly what it had: not the refill's
    // unfilled destination, and not less.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;
        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto pair = core::net::testing::makeSocketPair(loop);
            REQUIRE(pair.has_value());
            auto reader = AsyncBufferedReader { pair->first.get() };

            REQUIRE(loop.blockOn(send(pair->second.get(), "hal")));
            auto const timedOut = loop.blockOn(readLineWithin(&loop, &reader, RaceDeadline));
            REQUIRE_FALSE(timedOut.has_value());
            CHECK(reader.buffered() == 3);

            REQUIRE(loop.blockOn(send(pair->second.get(), "f a line\n")));
            auto const line = loop.blockOn(readLineWithin(&loop, &reader, 5s));
            REQUIRE(line.has_value());
            REQUIRE(line->has_value());
            CHECK(**line == "half a line");
            CHECK(reader.buffered() == 0);
        }
    }
}
