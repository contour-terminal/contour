// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <future>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/ByteStream.hpp>
#include <vthost/testing/MemoryByteStream.hpp>

using vthost::IByteStream;
using vthost::PumpEnd;
using vthost::RelayEnd;
using vthost::testing::makeMemoryStreamPair;

namespace
{

void writeText(IByteStream& stream, std::string_view text)
{
    REQUIRE(stream.writeAll(std::as_bytes(std::span { text })).has_value());
}

/// Reads @p stream to its end.
[[nodiscard]] std::vector<std::byte> readToEnd(IByteStream& stream)
{
    auto received = std::vector<std::byte> {};
    auto buffer = std::array<std::byte, 1024> {};
    while (true)
    {
        auto const count = stream.readSome(buffer);
        REQUIRE(count.has_value());
        if (*count == 0)
            return received;
        received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*count));
    }
}

[[nodiscard]] std::string readTextToEnd(IByteStream& stream)
{
    auto const bytes = readToEnd(stream);
    return bytes | std::views::transform([](std::byte b) { return static_cast<char>(b); })
           | std::ranges::to<std::string>();
}

} // namespace

TEST_CASE("pumpBytes copies until the source ends, then closes the sink", "[vthost][stdio]")
{
    auto const source = makeMemoryStreamPair();
    auto const sink = makeMemoryStreamPair();
    writeText(*source.b, "hello");
    source.b->closeWrite();

    CHECK(vthost::pumpBytes(*source.a, *sink.a) == PumpEnd::SourceEnded);
    CHECK(readTextToEnd(*sink.b) == "hello");
}

TEST_CASE("pumpBytes carries every byte value unchanged", "[vthost][stdio]")
{
    // Twice through 0x00..0xFF, 0x0A and 0x0D included: a text-mode stream would turn the one into the other.
    auto sent = std::vector<std::byte> {};
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2))
        for (auto const value: std::views::iota(0, 256))
            sent.push_back(static_cast<std::byte>(value));

    auto const source = makeMemoryStreamPair();
    auto const sink = makeMemoryStreamPair();
    REQUIRE(source.b->writeAll(sent).has_value());
    source.b->closeWrite();

    CHECK(vthost::pumpBytes(*source.a, *sink.a) == PumpEnd::SourceEnded);
    CHECK(readToEnd(*sink.b) == sent);
}

TEST_CASE("pumpBytes reports a sink that fails", "[vthost][stdio]")
{
    auto const source = makeMemoryStreamPair();
    auto const sink = makeMemoryStreamPair();
    sink.a->closeWrite(); // what the pump writes now fails
    writeText(*source.b, "lost");

    CHECK(vthost::pumpBytes(*source.a, *sink.a) == PumpEnd::SinkFailed);
}

TEST_CASE("runStdioRelay ends when the daemon closes", "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    writeText(*daemon.b, "bye");
    daemon.b->closeWrite();

    // The client never ends its side, so the client→daemon thread is still blocked reading it: the relay
    // returning at all proves it does not wait for that thread.
    CHECK(vthost::runStdioRelay(client.a, daemon.a) == RelayEnd::DaemonEnded);
    CHECK(readTextToEnd(*client.b) == "bye");

    client.b->closeWrite(); // lets the detached thread finish
}

TEST_CASE("runStdioRelay half-closes the daemon when the client ends", "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto relay = std::async(std::launch::async, [&]() { return vthost::runStdioRelay(client.a, daemon.a); });

    writeText(*client.b, "ping");
    client.b->closeWrite();
    CHECK(readTextToEnd(*daemon.b) == "ping");

    daemon.b->closeWrite();
    CHECK(relay.get() == RelayEnd::DaemonEnded);
    CHECK(readTextToEnd(*client.b).empty());
}
