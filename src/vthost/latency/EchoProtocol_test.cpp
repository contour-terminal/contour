// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <string>
#include <tuple>

#include <vthost/latency/EchoProtocol.hpp>

using namespace vthost::latency;

TEST_CASE("echoReply echoes, wraps at column 62 and quits on ^D", "[vthost][latency]")
{
    auto state = EchoState {};
    CHECK(echoReply(state, 'a').bytes == "a");
    CHECK(state.column == expectedCursorColumn(0));
    for (auto const i: std::views::iota(std::size_t { 1 }, std::size_t { 60 }))
        std::ignore = echoReply(state, keyFor(i));
    CHECK(state.column == expectedCursorColumn(59)); // 62
    CHECK(echoReply(state, keyFor(60)).bytes == "\r\n> " + std::string { keyFor(60) });
    CHECK(state.column == expectedCursorColumn(60)); // 3
    auto const quit = echoReply(state, QuitByte);
    CHECK(quit.action == EchoAction::Quit);
    CHECK(quit.bytes.empty());
}

TEST_CASE("keys cycle through the alphabet and columns wrap with the echo", "[vthost][latency]")
{
    CHECK(keyFor(0) == 'a');
    CHECK(keyFor(25) == 'z');
    CHECK(keyFor(26) == 'a');
    CHECK(expectedCursorColumn(0) == PromptColumns + 1);
    CHECK(expectedCursorColumn(EchoWrapColumns) == expectedCursorColumn(0));
}

TEST_CASE("floodedBytes reads the count after the marker once its line is complete", "[vthost][latency]")
{
    CHECK_FALSE(floodedBytes("").has_value());
    CHECK_FALSE(floodedBytes("lots of output\r\n").has_value());
    CHECK_FALSE(floodedBytes("flood-bytes 12").has_value()); // the number may still grow
    CHECK_FALSE(floodedBytes("flood-bytes 12 do").has_value());
    CHECK_FALSE(floodedBytes("flood-bytes  done").has_value());
    CHECK_FALSE(floodedBytes("flood-bytes 12x done").has_value());
    CHECK(floodedBytes("output\r\nflood-bytes 1234567 done").value_or(0) == 1234567);
    CHECK(floodedBytes("flood-bytes 1 done\nflood-bytes 2 done\n").value_or(0) == 2); // the last one counts
}
