// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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
using vthost::RelayConfig;
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

/// Reads exactly @p count bytes as text, or fewer when the stream ends first.
[[nodiscard]] std::string readText(IByteStream& stream, std::size_t count)
{
    auto text = std::string {};
    auto buffer = std::array<std::byte, 1024> {};
    while (text.size() < count)
    {
        auto const read =
            stream.readSome(std::span { buffer }.first(std::min(buffer.size(), count - text.size())));
        REQUIRE(read.has_value());
        if (*read == 0)
            break;
        for (auto const byte: std::span { buffer }.first(*read))
            text.push_back(static_cast<char>(byte));
    }
    return text;
}

[[nodiscard]] std::string readTextToEnd(IByteStream& stream)
{
    auto const bytes = readToEnd(stream);
    return std::ranges::to<std::string>(
        bytes | std::views::transform([](std::byte b) { return static_cast<char>(b); }));
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

TEST_CASE("runStdioRelay gives up on a daemon that answers the client with nothing",
          "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto relay = std::async(std::launch::async, [&]() {
        return vthost::runStdioRelay(
            client.a, daemon.a, RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 50 } });
    });

    writeText(*client.b, "hello");
    CHECK(readText(*daemon.b, 5) == "hello");
    CHECK(relay.get() == RelayEnd::DaemonSilent);
    CHECK(readTextToEnd(*client.b).empty()); // the client is told the relay is over

    client.b->closeWrite(); // lets the client→daemon thread finish
    daemon.b->closeWrite(); // lets the daemon→client thread finish
}

TEST_CASE("runStdioRelay counts the daemon's deadline from the client's half-close too",
          "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto relay = std::async(std::launch::async, [&]() {
        return vthost::runStdioRelay(
            client.a, daemon.a, RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 50 } });
    });

    client.b->closeWrite(); // owed: the daemon's end of the connection
    CHECK(readTextToEnd(*daemon.b).empty());
    CHECK(relay.get() == RelayEnd::DaemonSilent);

    daemon.b->closeWrite(); // lets the daemon→client thread finish
}

TEST_CASE("runStdioRelay sets no deadline while the client has not spoken", "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto relay = std::async(std::launch::async, [&]() {
        return vthost::runStdioRelay(
            client.a, daemon.a, RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 10 } });
    });

    // Ten deadlines' worth of a silent client and a silent daemon: nobody owes anybody anything yet.
    CHECK(relay.wait_for(std::chrono::milliseconds { 100 }) == std::future_status::timeout);

    daemon.b->closeWrite();
    CHECK(relay.get() == RelayEnd::DaemonEnded);
    client.b->closeWrite(); // lets the client→daemon thread finish
}

TEST_CASE("runStdioRelay sets no deadline once the daemon has answered", "[vthost][stdio][threading]")
{
    auto const client = makeMemoryStreamPair();
    auto const daemon = makeMemoryStreamPair();
    auto relay = std::async(std::launch::async, [&]() {
        return vthost::runStdioRelay(
            client.a, daemon.a, RelayConfig { .daemonReplyDeadline = std::chrono::milliseconds { 10 } });
    });

    // The daemon speaks first, so that no deadline can be armed and expire before its answer is read.
    writeText(*daemon.b, "hi");
    CHECK(readText(*client.b, 2) == "hi");
    writeText(*client.b, "hello");
    CHECK(readText(*daemon.b, 5) == "hello");
    // An idle session afterwards is no silent daemon: ten deadlines pass without ending the relay.
    CHECK(relay.wait_for(std::chrono::milliseconds { 100 }) == std::future_status::timeout);

    daemon.b->closeWrite();
    CHECK(relay.get() == RelayEnd::DaemonEnded);
    client.b->closeWrite(); // lets the client→daemon thread finish
}
