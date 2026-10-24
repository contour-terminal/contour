// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <vtcontrol/ObjectRef.hpp>
#include <vthost/control/PaneText.hpp>

using vtcontrol::Truncation;
using vthost::control::boundBlockText;
using vthost::control::keepTail;

TEST_CASE("keepTail never splits a UTF-8 sequence", "[vthost][control]")
{
    // "ä" is two bytes, "€" three: a cut that lands inside one moves to the next sequence's start.
    auto const text = std::string { "a\xC3\xA4\xE2\x82\xAC"
                                    "b" }; // a ä € b: 1 + 2 + 3 + 1 bytes

    CHECK(keepTail(text, 10).text == text);
    CHECK(keepTail(text, 10).truncated == Truncation::None);
    CHECK(keepTail(text, 7).truncated == Truncation::None);

    auto const four = keepTail(text, 4);
    CHECK(four.text
          == "\xE2\x82\xAC"
             "b");
    CHECK(four.truncated == Truncation::Head);

    // Five bytes would start inside "ä": the cut moves past it.
    CHECK(keepTail(text, 5).text
          == "\xE2\x82\xAC"
             "b");
    // Three would start inside "€".
    CHECK(keepTail(text, 3).text == "b");
    CHECK(keepTail(text, 0).text.empty());
}

TEST_CASE("boundBlockText cuts the output before the prompt", "[vthost][control]")
{
    auto const [prompt, output] = boundBlockText("$ ", "0123456789", 6);
    CHECK(prompt.text == "$ ");
    CHECK(prompt.truncated == Truncation::None);
    CHECK(output.text == "6789");
    CHECK(output.truncated == Truncation::Head);

    auto const [longPrompt, dropped] = boundBlockText("0123456789", "out", 4);
    CHECK(longPrompt.text == "6789");
    CHECK(dropped.text.empty());
    CHECK(dropped.truncated == Truncation::Head);

    auto const [fits, whole] = boundBlockText("$ ", "out", 5);
    CHECK(fits.truncated == Truncation::None);
    CHECK(whole.truncated == Truncation::None);
    CHECK(whole.text == "out");
}

TEST_CASE("blockInfoOf names the block and marks a superseded one", "[vthost][control]")
{
    auto const pane = *vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { 42 };
    record.state = vtbackend::CommandBlockState::Finished;
    record.end = vtbackend::CommandBlockEnd::Implicit;
    record.commandStartedAt =
        std::chrono::system_clock::time_point { std::chrono::milliseconds { 1'700'000'000'123 } };
    record.duration = std::chrono::milliseconds { 1500 };

    auto const info = vthost::control::blockInfoOf(pane, record, 2);
    CHECK(info.state == vtcontrol::BlockState::Superseded);
    CHECK(info.id
          == "block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/" + std::to_string((uint64_t { 2 } << 32) | 42));
    CHECK(info.pane == "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(info.startedAt == "2023-11-14T22:13:20.123Z");
    CHECK(info.durationMs == 1500);
    CHECK_FALSE(info.command.has_value());
    CHECK_FALSE(info.promptAt.has_value());
}
