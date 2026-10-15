// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/LivePromptMarks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ranges>

using namespace vtbackend;

TEST_CASE("LivePromptMarks.a right prompt is marked on its own line", "[osc133]")
{
    auto marks = LivePromptMarks {};
    marks.markRightPrompt(StableLine { .id = 7, .generation = 1 }, ColumnOffset(30));

    CHECK(marks.rightPromptOn(StableLine { .id = 7, .generation = 1 }) == ColumnOffset(30));
    CHECK(marks.rightPromptOn(StableLine { .id = 8, .generation = 1 }) == std::nullopt);
    // A reflow or a reset made a new generation of ids: the line is gone, whatever row now has its id.
    CHECK(marks.rightPromptOn(StableLine { .id = 7, .generation = 2 }) == std::nullopt);

    SECTION("redrawn, it is where it was drawn last")
    {
        marks.markRightPrompt(StableLine { .id = 7, .generation = 1 }, ColumnOffset(28));
        CHECK(marks.rightPromptOn(StableLine { .id = 7, .generation = 1 }) == ColumnOffset(28));
    }

    SECTION("a new prompt forgets it")
    {
        marks.clear();
        CHECK(marks.rightPromptOn(StableLine { .id = 7, .generation = 1 }) == std::nullopt);
    }
}

TEST_CASE("LivePromptMarks.an input ends with its line where the shell said so last", "[osc133]")
{
    auto marks = LivePromptMarks {};
    auto const line = StableLine { .id = 3, .generation = 0 };
    CHECK(marks.inputEndOn(line) == InputEnd::AtOutputStart);

    marks.markRightPrompt(line, ColumnOffset(30));
    marks.markInputEnd(line, InputEnd::AtLineEnd); // OSC 133;I
    CHECK(marks.inputEndOn(line) == InputEnd::AtLineEnd);
    CHECK(marks.rightPromptOn(line) == ColumnOffset(30));

    marks.markInputEnd(line, InputEnd::AtOutputStart); // OSC 133;B, the prompt redrawn
    CHECK(marks.inputEndOn(line) == InputEnd::AtOutputStart);
    CHECK(marks.rightPromptOn(line) == ColumnOffset(30));
}

TEST_CASE("LivePromptMarks.a renamed line keeps its marks, its columns moved with their cells", "[osc133]")
{
    // A resize that leaves the prompt's rows as the shell drew them still names every line anew, and may move
    // the cell a mark names to another column of the line.
    auto marks = LivePromptMarks {};
    auto const before = StableLine { .id = 7, .generation = 1 };
    auto const after = StableLine { .id = 3, .generation = 2 };
    auto const moveColumn = [](ColumnOffset column) {
        return column + ColumnOffset(5);
    };
    marks.markRightPrompt(before, ColumnOffset(30));
    marks.markInputEnd(before, InputEnd::AtLineEnd);

    marks.rename(before, after, moveColumn);

    CHECK(marks.rightPromptOn(after) == ColumnOffset(35));
    CHECK(marks.inputEndOn(after) == InputEnd::AtLineEnd);
    CHECK(marks.rightPromptOn(before) == std::nullopt);
    CHECK(marks.inputEndOn(before) == InputEnd::AtOutputStart);

    SECTION("a line nothing was marked on takes nothing along")
    {
        auto const unmarked = StableLine { .id = 4, .generation = 2 };
        marks.rename(StableLine { .id = 8, .generation = 1 }, unmarked, moveColumn);
        CHECK(marks.rightPromptOn(unmarked) == std::nullopt);
        CHECK(marks.rightPromptOn(after) == ColumnOffset(35));
    }
}

TEST_CASE("LivePromptMarks.a line's marks follow it through a scroll inside margins", "[osc133]")
{
    // The scroll moved what ids 11..15 named up by one, and dropped what 10 named, while every id kept naming
    // its place.
    auto marks = LivePromptMarks {};
    auto const lineOf = [](int64_t id) {
        return StableLine { .id = id, .generation = 1 };
    };
    marks.markRightPrompt(lineOf(13), ColumnOffset(30)); // moves to 12
    marks.markInputEnd(lineOf(13), InputEnd::AtLineEnd);
    marks.markRightPrompt(lineOf(10), ColumnOffset(20)); // dropped
    marks.markRightPrompt(lineOf(5), ColumnOffset(25));  // outside the move

    marks.follow(
        RowMove { .changed = { .first = 10, .last = 15 }, .moved = { .first = 11, .last = 15 }, .by = -1 });

    CHECK(marks.rightPromptOn(lineOf(12)) == ColumnOffset(30));
    CHECK(marks.inputEndOn(lineOf(12)) == InputEnd::AtLineEnd);
    CHECK(marks.rightPromptOn(lineOf(13)) == std::nullopt);
    CHECK(marks.inputEndOn(lineOf(13)) == InputEnd::AtOutputStart);
    CHECK(marks.rightPromptOn(lineOf(10)) == std::nullopt);
    CHECK(marks.rightPromptOn(lineOf(5)) == ColumnOffset(25));
}

TEST_CASE("LivePromptMarks.keeps no more lines than a prompt scan reaches", "[osc133]")
{
    // A shell may mark right prompts on as many lines as it likes before its next prompt starts.
    auto marks = LivePromptMarks {};
    auto const lineAt = [](size_t i) {
        return StableLine { .id = static_cast<int64_t>(i), .generation = 0 };
    };
    for (auto const i: std::views::iota(size_t { 0 }, LivePromptMarks::MaxLines + 1))
        marks.markRightPrompt(lineAt(i), ColumnOffset(10));

    CHECK(marks.rightPromptOn(lineAt(0)) == std::nullopt);
    CHECK(marks.rightPromptOn(lineAt(1)) == ColumnOffset(10));
    CHECK(marks.rightPromptOn(lineAt(LivePromptMarks::MaxLines)) == ColumnOffset(10));
}
