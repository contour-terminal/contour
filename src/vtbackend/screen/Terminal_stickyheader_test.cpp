// SPDX-License-Identifier: Apache-2.0
//
// The sticky command header wired to a live terminal: what Terminal::fillRenderBuffer() puts into
// RenderBuffer::stickyHeader for real OSC 133 output, and what Terminal::stickyHeaderBlock() reports for
// a press on row 0. The decision itself is tested pure in shell/StickyHeader_test.cpp.

#include <vtbackend/core/StickyHeaderKind.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalTestFixtures.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <latch>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;

namespace
{

/// What @p cells spell, left to right.
[[nodiscard]] std::string cellText(std::vector<RenderCell> const& cells)
{
    auto text = std::u32string {};
    for (auto const& cell: cells)
        text += cell.codepoints;
    return unicode::convert_to<char>(std::u32string_view { text });
}

/// Whether two cells draw the same: the same text, in the same place, size and colours.
[[nodiscard]] bool sameCell(RenderCell const& a, RenderCell const& b)
{
    return a.codepoints == b.codepoints && a.image == b.image && a.position == b.position
           && a.width == b.width && a.attributes.foregroundColor == b.attributes.foregroundColor
           && a.attributes.backgroundColor == b.attributes.backgroundColor
           && a.attributes.decorationColor == b.attributes.decorationColor
           && a.attributes.flags == b.attributes.flags && a.groupStart == b.groupStart
           && a.groupEnd == b.groupEnd;
}

/// Whether two batched lines draw the same: the same text, on the same row, in the same colours.
[[nodiscard]] bool sameLine(RenderLine const& a, RenderLine const& b)
{
    auto const sameAttributes = [](RenderAttributes const& x, RenderAttributes const& y) {
        return x.foregroundColor == y.foregroundColor && x.backgroundColor == y.backgroundColor
               && x.decorationColor == y.decorationColor && x.flags == y.flags;
    };
    return a.text == b.text && a.lineOffset == b.lineOffset && a.usedColumns == b.usedColumns
           && a.displayWidth == b.displayWidth && a.flags == b.flags
           && sameAttributes(a.textAttributes, b.textAttributes)
           && sameAttributes(a.fillAttributes, b.fillAttributes);
}

/// The block running now.
[[nodiscard]] CommandBlockId runningBlock(MockTerm<>& mc)
{
    auto const* const record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    return record->id;
}

} // namespace

TEST_CASE("StickyHeader.terminal.theModeDecidesWhenTheHeaderShows", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);

    SECTION("never: neither live nor scrolled back")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("scrolled (the default): only once scrolled back into the output")
    {
        REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);
        CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());

        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
        auto const header = drawFrame(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Command);
        CHECK(header->block == id);
        CHECK(cellText(header->cells) == "$ make");
        // What the frame drew is what a press on row 0 lands on.
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("always: while following the live output too")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->block == id);
        CHECK(cellText(header->cells) == "$ make");
    }
}

TEST_CASE("StickyHeader.terminal.noShellIntegrationNoHeader", "[stickyheader]")
{
    // A shell without integration leaves no blocks, and the terminal behaves as it always has.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    printLines(mc, "plain", 20);

    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(5)));
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
    REQUIRE(mc.terminal.viewport().scrollToTop());
    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
}

TEST_CASE("StickyHeader.terminal.neverOnTheAlternateScreen", "[stickyheader][altscreen]")
{
    // A full-screen program owns its first row; leaving it brings the header back.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "vim");
    printLines(mc, "out", 20);
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    REQUIRE(mc.terminal.stickyHeaderBlock().has_value());

    mc.writeToScreen("\033[?1049h");
    printLines(mc, "~", 3);
    // Before any frame of the alternate screen is drawn, a press on its first row is not one on the header
    // the primary screen's last frame showed.
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());

    mc.writeToScreen("\033[?1049l");
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(cellText(header->cells) == "$ vim");
}

TEST_CASE("StickyHeader.terminal.aCollapsedBlockShowsItsHeadNotAHeader", "[stickyheader][folding]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "make");

    auto const* const ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const lsId = ls->id;
    auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *ls);
    REQUIRE(head.has_value());

    // Expanded, and scrolled into its output: the header names it.
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(5)));
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->block == lsId);

    // Collapsed, its output is drawn nowhere: the head itself stands on the top row, and a header would
    // only repeat it.
    REQUIRE(mc.terminal.toggleFoldContaining(*head));
    REQUIRE(mc.terminal.viewport().topLine() == *head);
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
}

TEST_CASE("StickyHeader.terminal.noHeaderOverTheCommandLineItself", "[stickyheader]")
{
    // A command that wraps over three rows and prints nothing. Scrolled so that its own second row is
    // on top, the head is above the viewport -- yet the top row is still the command line, and a header
    // would repeat what is on screen.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, std::string(45, 'x'));
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "ls");
    printLines(mc, "out", 10);

    auto const* const wrapped = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(wrapped != nullptr);
    auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *wrapped);
    REQUIRE(head.has_value());

    auto const target = *head + LineOffset(1);
    REQUIRE(mc.terminal.viewport().scrollUp(
        LineCount::cast_from(unbox(mc.terminal.viewport().topLine() - target))));
    REQUIRE(mc.terminal.viewport().topLine() == target);
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());

    // Its last row too: the command line ends where its logical line does, not on the row it began.
    REQUIRE(mc.terminal.viewport().scrollDown(LineCount(1)));
    REQUIRE(mc.terminal.viewport().topLine() == *head + LineOffset(2));
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
}

TEST_CASE("StickyHeader.terminal.theHeaderCopiesTheRowTheInputIsOn", "[stickyheader]")
{
    // A prompt over several lines: the block's head is its first line, and what the user typed is on the
    // line OSC 133;B marked.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;

    SECTION("the line the prompt ended on, not the head")
    {
        mc.writeToScreen("\033]133;A\033\\~/src/contour\r\n> \033]133;B\033\\make\r\n\033]133;C\033\\");
    }

    SECTION("past a prompt row an erase left without its block")
    {
        // A shell redrawing its prompt erases everything below the first line and jumps to the line it
        // types on. The row it skipped carries no block until the cursor reaches it -- which says nothing
        // about which block it is in, so the walk down the prompt goes on past it.
        mc.writeToScreen("\033]133;A\033\\line one\r\nline two\r\n> ");
        mc.writeToScreen("\033[2A\033[9G\033[J\033[3;1H> \033]133;B\033\\make\r\n\033]133;C\033\\");
        auto const& grid = mc.terminal.primaryScreen().grid();
        REQUIRE(grid.lineAt(LineOffset(1)).blockId() == CommandBlockId {});
        REQUIRE(grid.lineAt(LineOffset(2)).blockId() == runningBlock(mc));
    }

    printLines(mc, "out", 20);
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(cellText(header->cells) == "> make");
}

TEST_CASE("StickyHeader.terminal.theChipNamesTheOutcome", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const& palette = mc.terminal.colorPalette();

    SECTION("running")
    {
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        REQUIRE(cellText(header->chip).starts_with("running "));
        CHECK(header->chip.front().attributes.foregroundColor
              == legibleTintOn(
                  palette, palette.blockStatusColor(CommandBlockOutcome::Running), header->background));
    }

    SECTION("failed, with its exit code")
    {
        mc.writeToScreen(commandEnd(2));
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        REQUIRE(cellText(header->chip).starts_with("✗ 2"));
        CHECK(header->chip.front().attributes.foregroundColor
              == legibleTintOn(
                  palette, palette.blockStatusColor(CommandBlockOutcome::Failure), header->background));
    }
}

TEST_CASE("StickyHeader.terminal.theHeaderKeepsTheRowsColoursOnItsOwnBand", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.writeToScreen("\033]133;A\033\\\033[32m$\033[m \033]133;B\033\\make\r\n\033]133;C\033\\");
    printLines(mc, "out", 20);

    auto const& palette = mc.terminal.colorPalette();
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    // Only what shows: the blanks after `$ make` would paint nothing the opaque band does not.
    REQUIRE(header->cells.size() == 6);

    // The prompt's own SGR survives the copy, lifted only where it would not read on the band ...
    CHECK(header->cells[0].attributes.foregroundColor
          == legibleTintOn(palette, palette.normalColor(2), header->background));
    // ... and a cell the shell left on the page background stays on it, so the band shows through it rather
    // than one rectangle per cell painting the band's colour over the band.
    CHECK(header->cells[2].attributes.foregroundColor == palette.defaultForeground);
    CHECK(header->cells[2].attributes.backgroundColor == palette.defaultBackground);

    CHECK(header->background == palette.stickyHeaderBackgroundColor());
    CHECK(header->separator == palette.stickyHeaderSeparatorColor());
    REQUIRE_FALSE(header->chip.empty());
    CHECK(header->chip.front().attributes.backgroundColor == palette.defaultBackground);
}

TEST_CASE("StickyHeader.terminal.theChipIsLaidOutAtThePagesEnd", "[stickyheader]")
{
    // The terminal places the chip itself, so the renderer only draws it: right-aligned on the header's row,
    // its last glyph ending StickyHeaderChipInset (2) columns short of the page's end, clear of the overlay
    // scrollbar: on a 30-column page, its last cell ends where column 28 begins.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "true");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE_FALSE(header->chip.empty());
    CHECK(header->chip.front().codepoints == U"✓");
    auto const& last = header->chip.back();
    CHECK(last.position.line == LineOffset(0));
    CHECK(last.position.column + ColumnOffset(last.width) == ColumnOffset(28));
    CHECK(header->chip.front().groupStart);
    CHECK(last.groupEnd);
}

TEST_CASE("StickyHeader.terminal.aLongCommandStopsABlankColumnShortOfTheChip", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make all install check distcheck");
    printLines(mc, "out", 20);

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE_FALSE(header->cells.empty());
    REQUIRE_FALSE(header->chip.empty());
    auto const textEnd = header->cells.back().position.column + ColumnOffset(header->cells.back().width);
    CHECK(textEnd + ColumnOffset(1) == header->chip.front().position.column);
}

TEST_CASE("StickyHeader.terminal.reverseVideoCopiesTheRowAsItShows", "[stickyheader]")
{
    // Under DECSCNM a default cell's background resolves to the foreground colour, which is not the page's:
    // the copy shows it as the row underneath does, band or no band.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    mc.writeToScreen("\033[?5h");

    auto const& palette = mc.terminal.colorPalette();
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE(cellText(header->cells) == "$ make");
    CHECK(header->cells[1].attributes.foregroundColor
          == legibleTintOn(palette, palette.defaultBackground, header->background));
    CHECK(header->cells[1].attributes.backgroundColor == palette.defaultForeground);
}

TEST_CASE("StickyHeader.terminal.theHeaderStaysOutOfTheCells", "[stickyheader][a11y]")
{
    // Selection, hit-testing and the accessibility bridge describe the GRID (the bridge reads
    // Screen::lineTextColumnAlignedAt). The header repeats a row that is still there underneath, so it
    // travels beside the cells: the covered row keeps its own entry, and no cell or line spells the copied
    // command.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));

    // The same frame with and without the header: whatever the header changed would show as a difference.
    auto const withHeader = renderOf(mc.terminal);
    REQUIRE(withHeader.stickyHeader.has_value());
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
    auto const without = renderOf(mc.terminal);
    REQUIRE_FALSE(without.stickyHeader.has_value());

    CHECK(std::ranges::equal(withHeader.cells, without.cells, sameCell));
    CHECK(std::ranges::equal(withHeader.lines, without.lines, sameLine));

    // ... and the row it covers is still drawn underneath it, as itself.
    auto const covered = renderedLayout(withHeader, LineOffset(0));
    REQUIRE_FALSE(covered.empty());
    CHECK(covered.front() == DrawnGlyph { .column = 0, .codepoints = U"o", .width = 1 });
}

TEST_CASE("StickyHeader.terminal.aColumnResizeMidOutputKeepsTheHeaderOnItsCommand", "[stickyheader][resize]")
{
    // Narrowing reflows every output line into two rows, which destroys stable row identity (a new
    // Grid::stableIdGeneration). The reflow re-heads every record before the next read, so the header still
    // finds its command -- with no scan of the output to do it.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) }, LineCount(200) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "compiling translation unit", 30);
    auto const id = runningBlock(mc);
    auto const generation = mc.terminal.primaryScreen().grid().stableIdGeneration();

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(24) });
    REQUIRE(mc.terminal.primaryScreen().grid().stableIdGeneration() != generation);

    auto header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Command);
    CHECK(header->block == id);
    CHECK(cellText(header->cells) == "$ make");

    // The command goes on printing at the new width, and the header goes on naming it ...
    printLines(mc, "linking unit", 10);
    header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->block == id);
    CHECK(cellText(header->cells) == "$ make");

    // ... including scrolled back into the reflowed output.
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Scrolled;
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(7)));
    header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->block == id);
}

TEST_CASE("StickyHeader.terminal.findsAHeadFurtherAwayThanAnyScanBudget", "[stickyheader]")
{
    // A build log runs to a million lines. The head is found through the record's cached position, never
    // by walking up from the top row -- so a head this far above it is found, where a walk bounded like
    // the fold scan (MaxFoldScanLines) or the prompt scan (MaxPromptScanLines) would stop long before it.
    auto constexpr OutputLines = static_cast<int>(MaxFoldScanLines) * 3;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(OutputLines + 100) };
    startCommand(mc, "make");
    printLines(mc, "out", OutputLines);

    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(2)));
    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(cellText(header->cells) == "$ make");
}

TEST_CASE("StickyHeader.terminal.anEvictedHeadLeavesThePlaceholderAtTheTop", "[stickyheader][evicted]")
{
    // A ten-line scrollback: forty lines of output push the head, and the first output lines, out of it.
    // The record outlives them, so the very top of what is left can still say which command it was.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);
    REQUIRE(mc.terminal.viewport().scrollToTop());

    SECTION("at the very top: the placeholder, with the running chip")
    {
        auto const header = drawFrame(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
        CHECK(cellText(header->cells) == "⋯ make — earlier output evicted");
        // The gutter's quieter text colour, lifted where it would not read on the band.
        auto const& palette = mc.terminal.colorPalette();
        CHECK(header->cells[0].attributes.foregroundColor
              == legibleTintOn(palette, palette.gutterTextColor(), header->background));
        CHECK(header->cells[0].attributes.backgroundColor == mc.terminal.colorPalette().defaultBackground);
        CHECK(cellText(header->chip).starts_with("running "));
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("show_evicted off: nothing")
    {
        mc.terminal.settings().stickyHeader.evictedPlaceholder = EvictedPlaceholder::Hidden;
        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("mode never does not hide it at the very top")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
    }
}

TEST_CASE("StickyHeader.terminal.anEvictedHeadKeepsThePlaceholderBelowTheTop", "[stickyheader][evicted]")
{
    // A long block whose prompt left the scrollback does not lose its header for that. Scrolled back into
    // its surviving output -- but not to the very top -- the placeholder takes the header's place wherever
    // the mode would show a header there.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);

    REQUIRE(mc.terminal.viewport().scrollToTop());
    REQUIRE(mc.terminal.viewport().scrollDown(LineCount(3)));
    REQUIRE(mc.terminal.viewport().scrolled());
    auto const topLine = mc.terminal.viewport().topLine();
    // Not the oldest row the scrollback holds: older ones are still above it ...
    REQUIRE(topLine > mc.terminal.primaryScreen().grid().addressableTop());
    // ... and it still belongs to the block whose head is gone.
    auto const* const record = mc.terminal.commandBlockAt(topLine);
    REQUIRE(record != nullptr);
    REQUIRE(record->id == id);
    REQUIRE_FALSE(headOffsetOf(mc.terminal.primaryScreen().grid(), *record).has_value());

    SECTION("scrolled (the default): the placeholder, naming the command")
    {
        REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);
        auto const header = drawFrame(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
        CHECK(cellText(header->cells) == "⋯ make — earlier output evicted");
        CHECK(cellText(header->chip).starts_with("running "));
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("never: nothing below the very top")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("show_evicted off: nothing")
    {
        mc.terminal.settings().stickyHeader.evictedPlaceholder = EvictedPlaceholder::Hidden;
        CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
    }

    SECTION("back at the live output: scrolled shows nothing there, always shows the placeholder")
    {
        REQUIRE(mc.terminal.viewport().scrollToBottom());
        CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());

        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
    }
}

TEST_CASE("StickyHeader.terminal.theHeaderStepsAsideForTheViCursor", "[stickyheader][vi]")
{
    // Over a block's output, the vi-mode cursor on the top row: a header there would hide it, so the header
    // steps aside while it is there.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);

    // Vi mode starts where the terminal's cursor is: on the top row.
    mc.writeToScreen("\033[H");
    mc.terminal.inputHandler().setMode(ViMode::Normal);
    auto const topLine = mc.terminal.viewport().topLine();
    REQUIRE(mc.terminal.normalModeCursorPosition().line == topLine);
    REQUIRE(mc.terminal.commandBlockAt(topLine)->id == id);
    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());

    SECTION("the cursor moves off the row: the header is back")
    {
        mc.sendCharEvent(U'j');
        REQUIRE(mc.terminal.viewport().topLine() == topLine);
        auto const header = drawFrame(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Command);
        // A press on the row is the header's again.
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }
}

TEST_CASE("StickyHeader.terminal.thePlaceholderAtTheVeryTopShowsOverTheViCursor",
          "[stickyheader][evicted][vi]")
{
    // The placeholder at the very top of the scrollback shows always: it is the one sign that the rows above
    // were dropped. `gg` takes the vi-mode cursor to exactly that row, and the placeholder stays.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);

    mc.terminal.inputHandler().setMode(ViMode::Normal);
    auto const oldest = mc.terminal.primaryScreen().grid().addressableTop();
    mc.sendCharSequence("gg");
    REQUIRE(mc.terminal.normalModeCursorPosition().line == oldest);
    REQUIRE(mc.terminal.viewport().topLine() == oldest);

    auto const header = drawFrame(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Evicted);
    CHECK(mc.terminal.stickyHeaderBlock() == id);
}

TEST_CASE("StickyHeader.terminal.theHeaderStepsAsideForTheTextCursor", "[stickyheader]")
{
    // `while :; do clear; printf %s "$(date)"; sleep 1; done` writes its one line, cursor and all, on the top
    // row of a cleared page. Under `always` a header there would hide both: it steps aside while the
    // terminal's own cursor is on the row, as it does for the vi-mode cursor.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "watch-date");
    auto const id = runningBlock(mc);
    mc.writeToScreen("\033[H\033[2J12:00:00");
    REQUIRE(mc.terminal.primaryScreen().cursor().position.line == LineOffset(0));
    REQUIRE(mc.terminal.commandBlockAt(LineOffset(0))->id == id);

    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());

    SECTION("the cursor moves on: the header is back over the row")
    {
        mc.writeToScreen("\r\n");
        auto const header = drawFrame(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Command);
        CHECK(header->block == id);
        CHECK(cellText(header->cells) == "$ watch-date");
    }
}

TEST_CASE("StickyHeader.terminal.aFoldCanPutTheVeryTopOnTheLivePage", "[stickyheader][evicted][folding]")
{
    // Following live output, with a later block collapsed: the rows it hides pull older ones onto the page,
    // down to the oldest row kept, which belongs to a block whose head is gone. That is the very top of the
    // scrollback, live or not, so the placeholder shows there under the default mode too.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const makeId = runningBlock(mc);
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "ls");
    printLines(mc, "file", 10);
    mc.writeToScreen(commandEnd(0));
    typeAtPrompt(mc, "");

    auto const* const ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const lsHead = headOffsetOf(mc.terminal.primaryScreen().grid(), *ls);
    REQUIRE(lsHead.has_value());
    REQUIRE(mc.terminal.toggleFoldContaining(*lsHead));

    REQUIRE_FALSE(mc.terminal.viewport().scrolled());
    REQUIRE(mc.terminal.viewport().topLine() == mc.terminal.primaryScreen().grid().addressableTop());
    REQUIRE(mc.terminal.commandBlockAt(mc.terminal.viewport().topLine())->id == makeId);
    REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Evicted);
    CHECK(header->block == makeId);
}

TEST_CASE("StickyHeader.terminal.withoutScrollbackTheLivePageHasNoVeryTop", "[stickyheader][evicted]")
{
    // With no scrollback kept, the live page's first row is also the oldest row there is -- yet it is no top
    // of a scrollback, and a placeholder that showed there whatever the mode would cover a live row for as
    // long as the command runs. The mode decides there, as for any other row.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(0) };
    startCommand(mc, "make");
    printLines(mc, "out", 10);
    auto const id = runningBlock(mc);
    auto const* const record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    REQUIRE_FALSE(headOffsetOf(mc.terminal.primaryScreen().grid(), *record).has_value());
    REQUIRE(mc.terminal.viewport().topLine() == mc.terminal.primaryScreen().grid().addressableTop());

    SECTION("scrolled (the default): no placeholder over the live page")
    {
        REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);
        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("always: the placeholder, as for any row the mode shows a header on")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        auto const header = renderOf(mc.terminal).stickyHeader;
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
    }
}

TEST_CASE("StickyHeader.terminal.aBlockHeadedAtItsOutputShowsThePlaceholder",
          "[stickyheader][evicted][resize]")
{
    // A command whose prompt row has just left a full scrollback, while the row its output started on is
    // the oldest one kept. A reflow renames every row, and re-heads the block among the new names at that
    // surviving row -- which is output: the command row has left, so the header stays the placeholder,
    // never a copy of what the command printed.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) }, LineCount(10) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    // Fifteen rows below the prompt's -- these fourteen lines and the cursor's -- fill the page and the
    // history. The long one wraps once the page narrows, so the narrowing reflows.
    mc.writeToScreen("first output\r\n" + std::string(70, 'x') + "\r\n");
    printLines(mc, "out", 12);
    auto const id = runningBlock(mc);

    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineText(grid.addressableTop()).starts_with("first output"));
    auto header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE(header->kind == StickyHeaderKind::Evicted);

    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(60) });
    auto const* const record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    auto const head = headOffsetOf(grid, *record);
    REQUIRE(head.has_value());
    REQUIRE(*head == grid.addressableTop());
    REQUIRE(grid.lineText(*head).starts_with("first output"));

    header = drawFrame(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Evicted);
    CHECK(header->block == id);
    CHECK(cellText(header->cells) == "⋯ make — earlier output evicted");
    CHECK(mc.terminal.stickyHeaderBlock() == id);

    // A press still lands somewhere: on the row that heads the block now, which nothing then covers.
    REQUIRE(mc.terminal.scrollToCommandBlockHead(id).has_value());
    CHECK(mc.terminal.viewport().topLine() == *head);
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
}

TEST_CASE("StickyHeader.terminal.aBlockWithoutAPromptShowsNoHeader", "[stickyheader][evicted]")
{
    // A shell that marks where its commands start and nothing else -- ;C with no ;A before it -- heads each
    // block at the row its output starts on. There never was a command row to copy, nor one to lose, so
    // there is no header: "earlier output evicted" would claim an eviction that never happened.
    auto const commandWithoutPrompt = std::string { "$ make\r\n\033]133;C;cmdline_url=make\033\\" };

    SECTION("its head on the grid, above the viewport")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(100) };
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        mc.writeToScreen(commandWithoutPrompt);
        printLines(mc, "out", 40);
        auto const id = runningBlock(mc);

        auto const& grid = mc.terminal.primaryScreen().grid();
        auto const* const record = mc.terminal.commandBlocks().find(id);
        REQUIRE(record != nullptr);
        auto const head = headOffsetOf(grid, *record);
        REQUIRE(head.has_value());
        REQUIRE(grid.lineText(*head).starts_with("out 0"));
        REQUIRE(*head < mc.terminal.viewport().topLine());
        REQUIRE(mc.terminal.commandBlockAt(mc.terminal.viewport().topLine()) == record);

        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("its head gone from the scrollback, seen from the very top")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
        mc.writeToScreen(commandWithoutPrompt);
        printLines(mc, "out", 40);
        auto const id = runningBlock(mc);
        REQUIRE(mc.terminal.viewport().scrollToTop());

        auto const* const record = mc.terminal.commandBlocks().find(id);
        REQUIRE(record != nullptr);
        REQUIRE_FALSE(headOffsetOf(mc.terminal.primaryScreen().grid(), *record).has_value());
        REQUIRE(mc.terminal.commandBlockAt(mc.terminal.viewport().topLine()) == record);

        CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }
}

TEST_CASE("StickyHeader.terminal.aBlockHeadedAtItsInputRowStillCopiesIt", "[stickyheader][evicted][resize]")
{
    // The same reflow, for a two-line prompt whose shell starts the command on the line typed at: only the
    // prompt's first line left, and the row the block is headed at now -- where the output started -- is
    // the very row the command was typed on. That one is still there to copy.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) }, LineCount(10) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.writeToScreen("\033]133;A\033\\~/src\r\n> \033]133;B\033\\make\033]133;C\033\\\r\n");
    mc.writeToScreen(std::string(70, 'x') + "\r\n");
    printLines(mc, "out", 12);
    auto const id = runningBlock(mc);

    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineText(grid.addressableTop()).starts_with("> make"));

    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(60) });
    auto const* const record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    auto const head = headOffsetOf(grid, *record);
    REQUIRE(head.has_value());
    REQUIRE(grid.lineText(*head).starts_with("> make"));

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Command);
    CHECK(cellText(header->cells) == "> make");
}

TEST_CASE("StickyHeader.terminal.thePlaceholderShowsAHostileCommandDefanged", "[stickyheader][evicted]")
{
    // The command line came from whatever wrote to the pty.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    mc.writeToScreen(reportedCommandStart("make%1B%5B201~"));
    printLines(mc, "out", 40);
    REQUIRE(mc.terminal.viewport().scrollToTop());

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE(header->kind == StickyHeaderKind::Evicted);
    auto const text = cellText(header->cells);
    CHECK_FALSE(text.contains('\x1b'));
    CHECK(text.contains("�"));
}

TEST_CASE("StickyHeader.terminal.aHeadNotPlacedYetIsNotEvicted", "[stickyheader][evicted]")
{
    // A daemon mirror adopts a host's record before the rows that head it arrive: its head is recorded in
    // no generation this grid has. That head is unknown here, not gone -- so the rows the mirror already
    // labelled with the block show neither a header nor the placeholder that would call it evicted.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    auto record = mirroredRecord(5, CommandBlockState::Running);
    record.headIdGeneration = UnknownHeadGeneration;
    mc.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
    for (auto const row: std::views::iota(0, 5))
        mc.terminal.primaryScreen().grid().lineAt(LineOffset(row)).adoptBlock(CommandBlockId { 5 });

    auto const* const adopted = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(adopted != nullptr);
    REQUIRE(adopted->id == CommandBlockId { 5 });
    REQUIRE(adopted->headIdGeneration != mc.terminal.primaryScreen().grid().stableIdGeneration());
    REQUIRE_FALSE(headOffsetOf(mc.terminal.primaryScreen().grid(), *adopted).has_value());

    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
}

TEST_CASE("StickyHeader.jump.bringsTheHeadToTheTop", "[stickyheader][jump]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);

    REQUIRE(mc.terminal.scrollToCommandBlockHead(id).has_value());

    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(grid.lineText(mc.terminal.viewport().topLine()).starts_with("$ make"));
    // The head is on screen now, so there is nothing left for a header to say.
    CHECK_FALSE(renderOf(mc.terminal).stickyHeader.has_value());
}

TEST_CASE("StickyHeader.jump.flashesTheRowItLandsOn", "[stickyheader][jump]")
{
    // As iTerm2 does: a band across the head's row, fading out, so the eye finds where the view moved to.
    using namespace std::chrono_literals;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().gutter.exitStatus = true;
    printLines(mc, "before", 3); // something above the head, for the view to scroll to
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(2));
    printLines(mc, "after", 3);
    auto const* const record = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    auto const id = record->id;
    auto const at = [&](std::chrono::milliseconds elapsed) {
        mc.terminal.tick(mc.steadyClock.now() + elapsed);
        return renderOf(mc.terminal).lineFlash;
    };

    CHECK_FALSE(renderOf(mc.terminal).lineFlash.has_value());
    REQUIRE(mc.terminal.scrollToCommandBlockHead(id).has_value());

    SECTION("on the head's row, in its block's colour, fading out")
    {
        auto const start = at(0ms);
        REQUIRE(start.has_value());
        CHECK(start->lineOffset == LineOffset(0));
        CHECK(start->color == blockHighlightColor(mc.terminal.colorPalette(), CommandBlockOutcome::Failure));
        auto const later = at(500ms);
        REQUIRE(later.has_value());
        CHECK(later->alpha < start->alpha);
        CHECK(later->alpha > 0.0f);
    }

    SECTION("asking for frames until it has faded, and none after")
    {
        (void) at(100ms);
        auto const wakeup = mc.terminal.nextRender();
        REQUIRE(wakeup.has_value());
        CHECK(*wakeup <= 100ms);
        CHECK_FALSE(at(800ms).has_value());
        CHECK_FALSE(at(900ms).has_value());
    }

    SECTION("on the command's row, below a prompt's opening rows")
    {
        // A prompt that opens with a blank line, as endo's does: the head is that line, the command below it.
        mc.writeToScreen("\033]133;A\033\\\r\n$ \033]133;B\033\\build\r\n\033]133;C\033\\");
        printLines(mc, "built", 10);
        auto const* const build = mc.terminal.commandBlocks().current();
        REQUIRE(build != nullptr);
        REQUIRE(mc.terminal.scrollToCommandBlockHead(build->id).has_value());
        auto const flash = at(0ms);
        REQUIRE(flash.has_value());
        CHECK(flash->lineOffset == LineOffset(1));
        CHECK(mc.terminal.primaryScreen()
                  .grid()
                  .lineText(mc.terminal.viewport().topLine() + LineOffset(1))
                  .starts_with("$ build"));
    }

    SECTION("on the row, not on the screen row, as the view scrolls")
    {
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(1)));
        auto const scrolled = at(100ms);
        REQUIRE(scrolled.has_value());
        CHECK(scrolled->lineOffset == LineOffset(1));

        // Scrolled out of view, it fades unseen.
        REQUIRE(mc.terminal.viewport().scrollDown(LineCount(3)));
        CHECK_FALSE(at(200ms).has_value());
    }
}

TEST_CASE("StickyHeader.jump.expandsTheBlocksCollapsedFold", "[stickyheader][jump][folding]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "make");
    printLines(mc, "more", 20);

    auto const* const ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const lsId = ls->id;
    auto const headStableId = ls->headStableId;
    auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *ls);
    REQUIRE(head.has_value());
    REQUIRE(mc.terminal.toggleFoldContaining(*head));
    REQUIRE(mc.terminal.foldState().isCollapsed(headStableId));

    REQUIRE(mc.terminal.scrollToCommandBlockHead(lsId).has_value());
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(headStableId));
    CHECK(mc.terminal.viewport().topLine() == *head);
}

TEST_CASE("StickyHeader.jump.landsWhileTheCursorWritesToAnotherPage", "[stickyheader][jump][folding]")
{
    // DECPCCM reset: the cursor moves to page 3 and page 0 stays on display, so the header still shows -- the
    // decision keys on the displayed page -- and the viewport, the window onto page 0, scrolls to its head.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "make");
    printLines(mc, "more", 20);
    auto const* const ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const lsId = ls->id;
    auto const headStableId = ls->headStableId;
    auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *ls);
    REQUIRE(head.has_value());
    REQUIRE(mc.terminal.toggleFoldContaining(*head));

    moveCursorOffTheDisplayedPage(mc);
    REQUIRE(renderOf(mc.terminal).stickyHeader.has_value());

    REQUIRE(mc.terminal.scrollToCommandBlockHead(lsId).has_value());
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(headStableId));
    CHECK(mc.terminal.viewport().topLine() == *head);
}

TEST_CASE("StickyHeader.jump.saysWhyItCannotLand", "[stickyheader][jump]")
{
    SECTION("a block nobody has heard of")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
        auto const jump = mc.terminal.scrollToCommandBlockHead(CommandBlockId { 9999 });
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::UnknownBlock);
    }

    SECTION("a block whose head the scrollback no longer holds")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(10) };
        startCommand(mc, "make");
        printLines(mc, "out", 40);
        auto const topLine = mc.terminal.viewport().topLine();
        auto const jump = mc.terminal.scrollToCommandBlockHead(runningBlock(mc));
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::HeadEvicted);
        // Nothing moved for a jump that could not land.
        CHECK(mc.terminal.viewport().topLine() == topLine);
    }

    SECTION("a mirrored block whose head is not placed yet: not known here, which is not gone")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
        auto record = mirroredRecord(5, CommandBlockState::Running);
        record.headIdGeneration = UnknownHeadGeneration;
        mc.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);

        auto const jump = mc.terminal.scrollToCommandBlockHead(CommandBlockId { 5 });
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::HeadNotPlaced);
    }

    SECTION("another page on display: the viewport cannot move")
    {
        // DECPCCM reset: page 3 is on display while the cursor writes to page 0. The viewport is the window
        // onto page 3, which has no history, so a jump into page 0's cannot land.
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        startCommand(mc, "ls");
        printLines(mc, "out", 20);
        mc.writeToScreen(commandEnd(0));
        startCommand(mc, "make");
        printLines(mc, "more", 20);
        auto const* const ls = mc.terminal.commandBlocks().lastFinished();
        REQUIRE(ls != nullptr);
        auto const lsId = ls->id;
        auto const headStableId = ls->headStableId;
        auto const head = headOffsetOf(mc.terminal.primaryScreen().grid(), *ls);
        REQUIRE(head.has_value());
        REQUIRE(mc.terminal.toggleFoldContaining(*head));

        moveDisplayOffTheCursorsPage(mc);
        auto const topLine = mc.terminal.viewport().topLine();

        auto const jump = mc.terminal.scrollToCommandBlockHead(lsId);
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::NotScrollable);
        // Nothing changed: the viewport stayed, and the fold the jump would have opened is still shut.
        CHECK(mc.terminal.viewport().topLine() == topLine);
        CHECK(mc.terminal.foldState().isCollapsed(headStableId));
    }
}

TEST_CASE("StickyHeader.terminal.aFrameThatWasNeverSwappedInNamesNoBlock", "[stickyheader][threading]")
{
    // The render thread still holds the front buffer, so the next frame stays in the back buffer: a press
    // on row 0 meanwhile lands on the header the screen still shows, not on the frame nobody has seen.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    REQUIRE(mc.terminal.stickyHeaderBlock() == id);

    auto holding = std::latch { 1 };
    auto release = std::latch { 1 };
    auto renderThread = std::thread { [&] {
        auto const front = mc.terminal.renderBuffer();
        holding.count_down();
        release.wait();
    } };
    holding.wait();

    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
    auto const swapped = mc.terminal.refreshRenderBuffer(true);
    auto const blockWhileHeld = mc.terminal.stickyHeaderBlock();
    release.count_down();
    renderThread.join();

    CHECK_FALSE(swapped);
    CHECK(blockWhileHeld == id);

    // Once the frame without a header is in front, nothing is there to press.
    CHECK_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
}

TEST_CASE("StickyHeader.terminal.aShownHeaderAllocatesNothingFrameToFrame", "[stickyheader]")
{
    // RenderBuffer::clear() keeps the header's storage, as it keeps the page's: what a long command's copy
    // took is there for the next frame's header, which lays itself out into it.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make all install check");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    mc.writeToScreen(commandEnd(0));

    auto buffer = RenderBuffer {};
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(25)));
    mc.terminal.fillRenderBuffer(buffer, true);
    REQUIRE(buffer.stickyHeader.has_value());
    REQUIRE(cellText(buffer.stickyHeader->cells) == "$ make all install check");
    auto const* const cellStorage = buffer.stickyHeader->cells.data();
    auto const cellCapacity = buffer.stickyHeader->cells.capacity();
    auto const* const chipStorage = buffer.stickyHeader->chip.data();

    REQUIRE(mc.terminal.viewport().scrollToBottom());
    mc.terminal.fillRenderBuffer(buffer, true);
    REQUIRE(buffer.stickyHeader.has_value());
    REQUIRE(cellText(buffer.stickyHeader->cells) == "$ ls");
    CHECK(buffer.stickyHeader->cells.capacity() == cellCapacity);
    CHECK(buffer.stickyHeader->cells.data() == cellStorage);
    CHECK(buffer.stickyHeader->chip.data() == chipStorage);
}

TEST_CASE("StickyHeader.terminal.aCrossfadeSnapshotLeavesTheDrawnBlock", "[stickyheader]")
{
    // A faded page switch renders the outgoing page into a buffer of its own, which is never swapped in.
    // Paging away and straight back, with no frame drawn between, the screen still shows page 0's header.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Fade;
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    REQUIRE(mc.terminal.stickyHeaderBlock() == id);

    mc.writeToScreen("\033[U"); // NP: to page 1, whose snapshot is page 0's
    mc.writeToScreen("\033[V"); // PP: back to page 0, whose snapshot is page 1's -- with no header
    REQUIRE(mc.terminal.isScreenTransitionActive());

    CHECK(mc.terminal.stickyHeaderBlock() == id);
}

TEST_CASE("StickyHeader.terminal.anOverlayKeepsOffTheHeadersRow", "[stickyheader][overlayclearance]")
{
    // Scrolled back into a block, the header is drawn over the page's top row: an overlay placed by the
    // clearance must not take that row for blank, whatever the grid row under the header holds.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    auto const headerRow = unbox<int>(mc.terminal.mainPageTopRow());
    CHECK(std::ranges::contains(mc.terminal.overlayClearance().avoidRows, headerRow));

    // Back at the live output the header is gone (Scrolled mode), and so is the row's reservation.
    REQUIRE(mc.terminal.viewport().scrollToBottom());
    REQUIRE_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK_FALSE(std::ranges::contains(mc.terminal.overlayClearance().avoidRows, headerRow));
}

TEST_CASE("StickyHeader.terminal.aHeaderInTheChromeCarriesItsDataAndCoversNoRow", "[stickyheader]")
{
    // In the pane header or the title band, the frame still carries the header -- what the chrome binds --
    // but no press on row 0 lands on it and no overlay steps aside for it: the row is the grid's.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
    mc.terminal.setStickyHeaderPresentation(StickyHeaderPresentation::Chrome);

    auto const header = drawFrame(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->presentation == StickyHeaderPresentation::Chrome);
    CHECK(header->block == id);
    CHECK(header->outcome == CommandBlockOutcome::Running);
    CHECK(header->commandLine == "make");
    CHECK(cellText(header->cells) == "$ make");
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    auto const headerRow = unbox<int>(mc.terminal.mainPageTopRow());
    CHECK_FALSE(std::ranges::contains(mc.terminal.overlayClearance().avoidRows, headerRow));

    // What the chrome reads between frames is the frame's own header.
    auto const snapshot = mc.terminal.stickyHeaderSnapshot();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->block == header->block);
    CHECK(snapshot->outcome == header->outcome);
    CHECK(snapshot->commandLine == header->commandLine);
    CHECK(cellText(snapshot->cells) == cellText(header->cells));
    CHECK(cellText(snapshot->chip) == cellText(header->chip));

    // A failure is named as one.
    mc.writeToScreen(commandEnd(2));
    auto const failed = mc.terminal.stickyHeaderSnapshot();
    REQUIRE(failed.has_value());
    CHECK(failed->outcome == CommandBlockOutcome::Failure);
    CHECK(cellText(failed->chip).starts_with("✗ 2"));

    // Back over the grid, it takes the press and the row again.
    mc.terminal.setStickyHeaderPresentation(StickyHeaderPresentation::InGrid);
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK(mc.terminal.stickyHeaderBlock() == id);
    CHECK(std::ranges::contains(mc.terminal.overlayClearance().avoidRows, headerRow));
}

TEST_CASE("StickyHeader.terminal.noSnapshotWhereNoHeaderShows", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    // Following live output under the default mode: no header, in the grid or out of it.
    CHECK_FALSE(mc.terminal.stickyHeaderSnapshot().has_value());
    mc.terminal.setStickyHeaderPresentation(StickyHeaderPresentation::Chrome);
    CHECK_FALSE(mc.terminal.stickyHeaderSnapshot().has_value());
}

TEST_CASE("StickyHeader.terminal.aDoubledRowIsDrawnSingleOnTheBand", "[stickyheader]")
{
    // DECDWL and either half of DECDHL, on the command's own row or on the row the band covers: the band is
    // one row of single cells, so nothing it draws is doubled, and the text keeps a blank column short of the
    // chip -- a doubled copy would run twice as far, over the chip and past the page.
    auto const [rendition, name] = GENERATE(std::pair { "\033#6", "DECDWL" },
                                            std::pair { "\033#3", "DECDHL top" },
                                            std::pair { "\033#4", "DECDHL bottom" });
    INFO(name);
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.writeToScreen(
        std::format("\033]133;A\033\\{}$ \033]133;B\033\\make all\r\n\033]133;C\033\\", rendition));
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("{}out {}\r\n", rendition, i));

    auto const header = renderOf(mc.terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(cellText(header->cells) == "$ make all");
    REQUIRE_FALSE(header->chip.empty());
    auto const singleCells = [](std::vector<RenderCell> const& cells) {
        auto column = cells.front().position.column;
        for (auto const& cell: cells)
        {
            if (cell.attributes.lineFlags != LineFlag::None || cell.position.column != column)
                return false;
            column += ColumnOffset::cast_from(cell.width);
        }
        return true;
    };
    CHECK(singleCells(header->cells));
    CHECK(singleCells(header->chip));
    CHECK(header->cells.back().position.column < header->chip.front().position.column - ColumnOffset(1));
    CHECK(header->chip.back().position.column < ColumnOffset(30));
}

namespace
{
/// A MockTerm that counts how often the chrome's sticky header changed.
struct ChromeHeaderCountingTerm final: MockTerm<>
{
    using MockTerm<>::MockTerm;

    int changes = 0; ///< chromeStickyHeaderChanged() calls.

    void chromeStickyHeaderChanged() override { ++changes; }
};
} // namespace

TEST_CASE("StickyHeader.terminal.theChromeHearsOfEachChangeOnce", "[stickyheader]")
{
    // The pane header re-binds on this: once per change of what it shows -- not once per frame, and never
    // for a header the grid draws.
    auto mc = ChromeHeaderCountingTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
    REQUIRE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK(mc.changes == 0);

    mc.terminal.setStickyHeaderPresentation(StickyHeaderPresentation::Chrome);
    (void) drawFrame(mc.terminal);
    CHECK(mc.changes == 1);
    (void) drawFrame(mc.terminal);
    CHECK(mc.changes == 1);

    mc.writeToScreen(commandEnd(2)); // the chip says how it ended
    (void) drawFrame(mc.terminal);
    CHECK(mc.changes == 2);

    REQUIRE(mc.terminal.viewport().scrollToBottom()); // no header following live output
    REQUIRE_FALSE(drawFrame(mc.terminal).stickyHeader.has_value());
    CHECK(mc.changes == 3);
}
