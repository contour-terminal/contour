// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/BlockStatusText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <string_view>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{
/// 10:00:00 UTC, when the commands below started.
constexpr auto Started = std::chrono::system_clock::time_point {
    std::chrono::sys_days { std::chrono::year { 2026 } / std::chrono::October / 27 } + 10h
};

/// A record of `make` that started at Started and ended so.
[[nodiscard]] CommandBlockRecord finished(std::optional<int> exitCode,
                                          std::chrono::steady_clock::duration duration,
                                          ContextOutcome outcome = {})
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId { 1 };
    record.state = CommandBlockState::Finished;
    record.exitCode = exitCode;
    record.outcome = outcome;
    record.promptStartedAt = Started;
    record.commandStartedAt = Started;
    record.duration = duration;
    return record;
}
} // namespace

TEST_CASE("blockStatus.everyEndingHasOneRow", "[blockstatus]")
{
    // One row per CommandEnding, in its order, then the Running row.
    REQUIRE(BlockStatusWordingRows.size() == CommandEndingCount + 1);
    for (auto const i: std::views::iota(std::size_t { 0 }, CommandEndingCount))
        CHECK(BlockStatusWordingRows[i].ending == static_cast<CommandEnding>(i));
    CHECK_FALSE(BlockStatusWordingRows.back().ending.has_value());
}

TEST_CASE("blockStatus.words", "[blockstatus]")
{
    // One duration format everywhere (ruling F1): formatCommandDuration's.
    auto const failed = finished(8, 12'400ms);
    CHECK(blockStatusText(failed, Started + 13s) == "Failed · exit 8 · 12s");
    CHECK(spokenBlockStatus(failed, Started + 13s) == "Failed, exit 8, 12 seconds");

    auto const succeeded = finished(0, 4ms);
    CHECK(blockStatusText(succeeded, Started + 1s) == "4ms");
    CHECK(spokenBlockStatus(succeeded, Started + 1s) == "Succeeded, 4 milliseconds");

    CHECK(blockStatusText(finished(0, 134s), Started + 200s) == "2m 14s");
    CHECK(spokenBlockStatus(finished(0, 134s), Started + 200s) == "Succeeded, 2 minutes 14 seconds");
    CHECK(spokenBlockStatus(finished(0, 1s), Started + 2s) == "Succeeded, 1 second");
    CHECK(spokenBlockStatus(finished(0, 3'720s), Started + 2h) == "Succeeded, 1 hour 2 minutes");

    auto const killed =
        finished(std::nullopt,
                 3s,
                 ContextOutcome { .exit = ContextExit::Failure, .signal = ContextSignal::Kill, .status = 0 });
    CHECK(blockStatusText(killed, Started + 4s) == "Killed · SIGKILL · 3s");
    CHECK(spokenBlockStatus(killed, Started + 4s) == "Killed, SIGKILL, 3 seconds");
}

TEST_CASE("blockStatus.aRunningCommandCountsFromItsStart", "[blockstatus]")
{
    auto running = finished(std::nullopt, 0s);
    running.state = CommandBlockState::Running;
    running.duration = std::nullopt;
    CHECK(blockStatusText(running, Started + 8'700ms) == "Running · 8s");
    CHECK(spokenBlockStatus(running, Started + 8'700ms) == "Running, 8 seconds");
}

TEST_CASE("blockStatus.aPromptSaysNothing", "[blockstatus]")
{
    auto prompt = finished(std::nullopt, 0s);
    prompt.state = CommandBlockState::Prompting;
    CHECK(blockStatusText(prompt, Started).empty());
    CHECK(spokenBlockStatus(prompt, Started).empty());
}

TEST_CASE("blockChip.textReadsAtFourAndAHalf", "[blockstatus]")
{
    // F29: the chip's text over its band over the page reads at 4.5:1, on every bundled scheme and outcome.
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        for (auto const i: std::views::iota(std::size_t { 0 }, CommandBlockOutcomeCount))
        {
            CAPTURE(i);
            auto const colors = blockChipColors(palette, static_cast<CommandBlockOutcome>(i));
            CHECK(contrastRatio(colors.text, chipGround(palette, colors)) >= MinimumTextContrast);
        }
    }
}
