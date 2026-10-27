// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/BlockRail.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;

namespace
{
constexpr auto RailGutter = GutterSettings { .exitStatus = true, .blockColumnForm = BlockColumnForm::Rail };

/// A MockTerm that counts the redraws it is asked for.
struct RedrawCountingTerm final: MockTerm<>
{
    using MockTerm<>::MockTerm;

    int redraws = 0; ///< How often the terminal asked for a frame to be drawn.

    void renderBufferUpdated() override { ++redraws; }
};

/// Three finished commands, `one` (success), `two` (failure) and `three` (success), then a live prompt.
void runThreeBlocks(MockTerm<>& mc)
{
    mc.terminal.settings().gutter = RailGutter;
    runCommand(mc, "one", { "a" }, 0);
    runCommand(mc, "two", { "b" }, 1);
    runCommand(mc, "three", { "c" }, 0);
    mc.writeToScreen(PromptStart);
}

/// The ids of the stored blocks, oldest first.
[[nodiscard]] std::vector<CommandBlockId> blockIds(Terminal const& terminal)
{
    return std::ranges::to<std::vector>(terminal.commandBlocks().records()
                                        | std::views::transform(&CommandBlockRecord::id));
}

/// The rail mark drawn on screen row @p row of the next frame.
[[nodiscard]] RailMark markOnRow(Terminal& terminal, int row)
{
    for (auto const& cell: renderOf(terminal).gutter)
        if (cell.lineOffset == LineOffset(row) && cell.mark != RailMark::None)
            return cell.mark;
    return RailMark::None;
}

[[nodiscard]] bool hasBlockFocusMode(Terminal const& terminal)
{
    return (terminal.bindingModes() & static_cast<uint8_t>(MatchModes::BlockFocus)) != 0;
}
} // namespace

TEST_CASE("Terminal.blockfocus.previousWalksBackToTheOldestAndStops", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    auto const ids = blockIds(mc.terminal);
    REQUIRE(ids.size() == 4); // three finished and the live prompt
    CHECK_FALSE(mc.terminal.focusedCommandBlock().has_value());

    CHECK(mc.terminal.focusCommandBlock(BlockStep::Previous) == ids[2]);
    CHECK(mc.terminal.focusCommandBlock(BlockStep::Previous) == ids[1]);
    CHECK(mc.terminal.focusCommandBlock(BlockStep::Previous) == ids[0]);
    CHECK_FALSE(mc.terminal.focusCommandBlock(BlockStep::Previous).has_value());
    CHECK(mc.terminal.focusedCommandBlock() == ids[0]);

    SECTION("Next goes forward from the focus and stops at the newest command, never the live prompt")
    {
        CHECK(mc.terminal.focusCommandBlock(BlockStep::Next) == ids[1]);
        CHECK(mc.terminal.focusCommandBlock(BlockStep::Next) == ids[2]);
        CHECK_FALSE(mc.terminal.focusCommandBlock(BlockStep::Next).has_value());
        CHECK(mc.terminal.focusedCommandBlock() == ids[2]);
    }
}

TEST_CASE("Terminal.blockfocus.nextWithoutFocusStartsAtTheViewportsTop", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    auto const ids = blockIds(mc.terminal);
    CHECK(mc.terminal.focusCommandBlock(BlockStep::Next) == ids[0]);
}

TEST_CASE("Terminal.blockfocus.theMatchModesSayBlockFocusWhileFocused", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    CHECK_FALSE(hasBlockFocusMode(mc.terminal));

    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous);
    CHECK(hasBlockFocusMode(mc.terminal));

    mc.terminal.clearCommandBlockFocus();
    CHECK_FALSE(mc.terminal.focusedCommandBlock().has_value());
    CHECK_FALSE(hasBlockFocusMode(mc.terminal));
}

TEST_CASE("Terminal.blockfocus.clearingRedrawsOnlyWhenFocused", "[blockfocus]")
{
    auto mc = RedrawCountingTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    auto const before = mc.redraws;
    mc.terminal.clearCommandBlockFocus();
    CHECK(mc.redraws == before);

    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous);
    CHECK(mc.redraws == before + 1);
    mc.terminal.clearCommandBlockFocus();
    CHECK(mc.redraws == before + 2);
}

TEST_CASE("Terminal.blockfocus.theHeadRowsRailMarkIsACheckForASuccess", "[blockfocus][gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc); // `one` heads row 0, `two` row 2, `three` row 4
    CHECK(markOnRow(mc.terminal, 4) == RailMark::None);

    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous); // `three`
    CHECK(markOnRow(mc.terminal, 4) == RailMark::Check);
    CHECK(markOnRow(mc.terminal, 0) == RailMark::None);

    // A failure says what it is at rest, and focused too.
    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous); // `two`
    CHECK(markOnRow(mc.terminal, 2) == RailMark::Cross);
}

TEST_CASE("Terminal.blockfocus.theFocusedRailIsLitLikeAHoveredOne", "[blockfocus][gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous); // `three`
    auto const lit =
        railColor(mc.terminal.colorPalette(), CommandBlockOutcome::Success, BlockAttention::Focused);
    auto const rest =
        railColor(mc.terminal.colorPalette(), CommandBlockOutcome::Success, BlockAttention::Rest);
    CHECK(lit != rest);
    for (auto const& cell: renderOf(mc.terminal).gutter)
        if (cell.lineOffset == LineOffset(4) && cell.rail != RailSegment::None)
            CHECK(cell.railColor == lit);
}

TEST_CASE("Terminal.blockfocus.retiringTheFocusedBlockClearsTheFocus", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    runThreeBlocks(mc);
    auto const ids = blockIds(mc.terminal);
    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous);
    std::ignore = mc.terminal.focusCommandBlock(BlockStep::Previous); // block 2
    REQUIRE(mc.terminal.focusedCommandBlock() == ids[1]);

    mc.terminal.commandBlocks().retire(std::array { ids[1] });
    mc.writeToScreen("x"); // the next parsed batch notices
    CHECK_FALSE(mc.terminal.focusedCommandBlock().has_value());
    CHECK_FALSE(hasBlockFocusMode(mc.terminal));
}

TEST_CASE("Terminal.blockfocus.focusScrollsAnOffscreenHeadIntoView", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    mc.terminal.settings().gutter = RailGutter;
    runCommand(mc, "old", { "a", "b" }, 0);
    printLines(mc, "filler", 30);
    mc.writeToScreen(PromptStart);
    auto const ids = blockIds(mc.terminal);
    REQUIRE_FALSE(ids.empty());

    CHECK(mc.terminal.focusCommandBlock(BlockStep::Previous) == ids.front());
    auto const* const record = mc.terminal.commandBlocks().find(ids.front());
    REQUIRE(record != nullptr);
    auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *record);
    REQUIRE(head.has_value());
    CHECK(mc.terminal.viewport().isLineVisible(*head));
}

TEST_CASE("Terminal.blockfocus.aBlockAtItsPromptIsNeverFocused", "[blockfocus]")
{
    auto mc = MockTerm { PageSize { LineCount(20), ColumnCount(20) } };
    mc.terminal.settings().gutter = RailGutter;
    mc.writeToScreen(PromptStart);
    CHECK_FALSE(mc.terminal.focusCommandBlock(BlockStep::Previous).has_value());
    CHECK_FALSE(mc.terminal.focusCommandBlock(BlockStep::Next).has_value());
    CHECK_FALSE(hasBlockFocusMode(mc.terminal));
}

TEST_CASE("Terminal.blockfocus.aNestedShellWithoutIntegrationStaysOneRunningBlock", "[blockfocus][gutter]")
{
    // `ssh` starts (;C) and the remote shell, which marks nothing, prints prompts and output for 50 rows.
    auto mc = MockTerm { PageSize { LineCount(60), ColumnCount(40) } };
    mc.terminal.settings().gutter = RailGutter;
    startCommand(mc, "ssh host");
    for (auto const row: std::views::iota(0, 25))
        mc.writeToScreen(std::format("remote$ ls {}\r\nfile {}\r\n", row, row));

    auto const ids = blockIds(mc.terminal);
    REQUIRE(ids.size() == 1);
    auto const* const record = mc.terminal.commandBlocks().find(ids.front());
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Running);

    auto const buffer = renderOf(mc.terminal);
    auto rails = std::map<int, RenderGutterCell> {};
    auto marks = std::vector<RailMark> {};
    for (auto const& cell: buffer.gutter)
    {
        if (cell.rail != RailSegment::None)
            rails.emplace(unbox<int>(cell.lineOffset), cell);
        if (cell.mark != RailMark::None)
            marks.push_back(cell.mark);
    }

    // Every row from the `ssh` line down to the cursor belongs to the one block: Head on the first, Body
    // after it, in the running colour; no segment or mark for the nested shell's own prompt rows.
    REQUIRE(rails.size() >= 50);
    CHECK(rails.at(0).rail == RailSegment::Head);
    for (auto const& [row, cell]: rails)
    {
        INFO("row " << row);
        CHECK((cell.rail == RailSegment::Head || cell.rail == RailSegment::Body));
        CHECK(cell.outcome == CommandBlockOutcome::Running);
    }
    CHECK(marks == std::vector { RailMark::Running });

    // One chip, on the `ssh` row: on the Terminal row's annotations, on a native one's pills.
    auto chipRows = std::set<int> {};
    for (auto const& cell: buffer.annotations)
        chipRows.insert(unbox<int>(cell.position.line));
    for (auto const& chip: buffer.chips)
        chipRows.insert(unbox<int>(chip.lineOffset));
    CHECK(chipRows.size() <= 1);
    if (!chipRows.empty())
        CHECK(*chipRows.begin() == 0);

    // Nothing after it to focus, so the step finds none, and the focus stays where it was.
    CHECK(mc.terminal.focusCommandBlock(BlockStep::Previous) == ids.front());
    CHECK_FALSE(mc.terminal.focusCommandBlock(BlockStep::Next).has_value());
    CHECK(mc.terminal.focusedCommandBlock() == ids.front());
}
