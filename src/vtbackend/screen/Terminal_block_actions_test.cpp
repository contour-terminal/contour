// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;
using namespace std::string_view_literals;

namespace
{

/// Rows 0..5: `$ ls` printing a and b (block 1), `$ pwd` printing /tmp (block 2), the live prompt (block 3).
void twoCommandsAndAPrompt(MockTerm<>& mock)
{
    runCommand(mock, "ls", { "a", "b" });
    runCommand(mock, "pwd", { "/tmp" });
    mock.writeToScreen(PromptStart);
}

[[nodiscard]] MockTerm<> makeTerm()
{
    return MockTerm<> { PageSize { LineCount(8), ColumnCount(20) }, LineCount(10) };
}

/// Moves the mouse pointer over @p line (column 0), as hovering would.
void pointAt(MockTerm<>& mock, int line)
{
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) },
                                     PixelCoordinate {},
                                     false);
}

} // namespace

TEST_CASE("Terminal.commandBlockText.thePointerNamesTheBlockUnderIt", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    pointAt(mock, 1);
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Output) == "a\nb");
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Prompt) == "$ ls");
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::PromptAndOutput) == "$ ls\na\nb");

    pointAt(mock, 3);
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::PromptAndOutput) == "$ pwd\n/tmp");
}

TEST_CASE("Terminal.commandBlockRows.lastIsTheMostRecentlyFinishedBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(4), .last = LineOffset(4) });
    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "/tmp");

    auto const* record = mock.terminal.targetedCommandBlock(CommandBlockTarget::Last);
    REQUIRE(record != nullptr);
    CHECK(record == mock.terminal.commandBlocks().lastFinished());
}

TEST_CASE("Terminal.commandBlockRows.theCursorNamesTheLivePrompt", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Prompt)
          == CommandBlockRows { .first = LineOffset(5), .last = LineOffset(5) });

    // In vi normal mode the vi cursor is the cursor.
    mock.terminal.inputHandler().setMode(ViMode::Normal);
    mock.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
    CHECK(textOf(mock, CommandBlockTarget::Cursor, CommandBlockPart::Output) == "a\nb");
}

TEST_CASE("Terminal.commandBlockRows.withoutShellIntegrationThereIsNoBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    mock.writeToScreen("hello\r\nworld\r\n"sv);

    pointAt(mock, 0);
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.targetedCommandBlock(CommandBlockTarget::Last) == nullptr);
    CHECK_FALSE(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Pointer));
    CHECK_FALSE(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Last));
}

TEST_CASE("Terminal.hasCommandBlockRows.whileAnyRowOfTheBlockIsLeft", "[semanticblocks][blockactions]")
{
    // Four rows of page and three of history: `seq 8` prints more than both hold together.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(20) }, LineCount(3) };
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto const lastFinished = [&mock] {
        return mock.terminal.commandBlocks().lastFinished();
    };

    SECTION("its head on the grid")
    {
        twoCommandsAndAPrompt(mock);
        REQUIRE(lastFinished() != nullptr);
        REQUIRE(headOffsetOf(grid, *lastFinished()).has_value());
        CHECK(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Last));
        CHECK(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Cursor));
    }

    SECTION("its head gone from the history, its output not")
    {
        runCommand(mock, "seq 8", { "1", "2", "3", "4", "5", "6", "7", "8" });
        mock.writeToScreen(PromptStart);
        REQUIRE(lastFinished() != nullptr);
        REQUIRE_FALSE(headOffsetOf(grid, *lastFinished()).has_value());
        CHECK(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Last));
        CHECK(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Last, lastFinished()->id));
    }

    SECTION("every row of it taken, its record kept")
    {
        runCommand(mock, "ls", { "a" });
        mock.writeToScreen(PromptStart);
        REQUIRE(mock.terminal.clearToPrompt().has_value());
        REQUIRE(lastFinished() != nullptr);
        CHECK_FALSE(mock.terminal.hasCommandBlockRows(CommandBlockTarget::Last));
    }
}

TEST_CASE("Terminal.selectCommandBlock.selectsWholeLines", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    pointAt(mock, 2);
    REQUIRE(
        mock.terminal.selectCommandBlock(CommandBlockTarget::Pointer, CommandBlockPart::Output).has_value());
    REQUIRE(mock.terminal.selectionAvailable());
    CHECK(mock.terminal.selector()->state() == Selection::State::Complete);
    // A full-line selection copies with a terminating newline (SelectionRenderer::finish()).
    CHECK(mock.terminal.extractSelectionText() == "a\nb\n");

    // A target with nothing to select leaves the selection alone.
    CHECK_FALSE(
        mock.terminal.selectCommandBlock(CommandBlockTarget::Cursor, CommandBlockPart::Output).has_value());
    CHECK(mock.terminal.extractSelectionText() == "a\nb\n");
}

TEST_CASE("Terminal.commandBlockText.joinsSoftWrapsAndKeepsColours", "[semanticblocks][blockactions]")
{
    // Five columns: "abcd fgh" wraps after the space, which is content and must survive the join.
    auto mock = MockTerm<> { PageSize { LineCount(6), ColumnCount(5) }, LineCount(10) };
    runCommand(mock, "x", { "\033[31mabcd fgh\033[m", "red" });
    mock.writeToScreen(PromptStart);

    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "abcd fgh\nred");

    auto const coloured = mock.terminal.commandBlockText(
        CommandBlockTarget::Last, CommandBlockPart::Output, CaptureRendition::WithSgr);
    REQUIRE(coloured.has_value());
    CHECK(coloured->contains("\033["));
    CHECK(coloured->contains("red"));
}

TEST_CASE("Terminal.commandBlockText.aPinnedBlockWinsOverTheTarget", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    auto const* first = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(first != nullptr);

    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output, first->id) == "a\nb");
    CHECK(mock.terminal.targetedCommandBlock(CommandBlockTarget::Last, first->id) == first);
}

TEST_CASE("Terminal.selectCommandBlock.refusesOnTheAlternateScreen", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    mock.writeToScreen("\033[?1049h\033[H"sv);
    REQUIRE_FALSE(mock.terminal.isPrimaryScreen());

    pointAt(mock, 1);
    CHECK(mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));

    // The history still answers: "last" reads the primary screen, and "cursor" the shell's own cursor there,
    // not the one homed on the alternate screen.
    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "/tmp");
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Prompt)
          == CommandBlockRows { .first = LineOffset(5), .last = LineOffset(5) });
}

TEST_CASE("Terminal.commandBlockRows.aPartEndsAboveItsTrailingBlankRows", "[semanticblocks][blockactions]")
{
    // `ls` prints "a", and the shell a newline before its next prompt: the end mark lands on a blank row.
    // `echo` prints one blank line, and its end mark shares the next prompt's row.
    auto mock = makeTerm();
    mock.writeToScreen(commandRun("ls", { "a" }) + "\r\n");
    runCommand(mock, "echo", { "" });
    mock.writeToScreen(PromptStart);

    // The end mark's blank row is the block's last row, but neither copied nor selected.
    pointAt(mock, 1);
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(1), .last = LineOffset(1) });
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::PromptAndOutput)
          == CommandBlockRows { .first = LineOffset(0), .last = LineOffset(1) });
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Output) == "a");
    REQUIRE(
        mock.terminal.selectCommandBlock(CommandBlockTarget::Pointer, CommandBlockPart::Output).has_value());
    CHECK(mock.terminal.extractSelectionText() == "a\n");

    // An output of nothing but a blank row keeps that row: a part never ends above its first row.
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(4), .last = LineOffset(4) });
    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output).empty());
}

TEST_CASE("Terminal.commandBlockText.anOutputTailSharesItsRowWithTheNextPrompt",
          "[semanticblocks][blockactions]")
{
    // `printf foo` ends without a newline, so the next prompt is printed on the row its output ends on, and
    // that prompt's block owns the row. The row is split where the command ended, as the semantic-block query
    // reply splits it: the columns before the end mark are the output of printf, the rest the next prompt.
    auto mock = makeTerm();
    startCommand(mock, "printf foo");
    mock.writeToScreen("foo" + commandEnd(0) + commandRun("echo x", { "x" }) + std::string { PromptStart });

    auto const* const printfBlock = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(printfBlock != nullptr);
    auto const last = CommandBlockTarget::Last;

    CHECK(textOf(mock, last, CommandBlockPart::Output, printfBlock->id) == "foo");
    CHECK(textOf(mock, last, CommandBlockPart::Prompt, printfBlock->id) == "$ printf foo");
    CHECK(textOf(mock, last, CommandBlockPart::PromptAndOutput, printfBlock->id) == "$ printf foo\nfoo");

    CHECK(textOf(mock, last, CommandBlockPart::Prompt) == "$ echo x");
    CHECK(textOf(mock, last, CommandBlockPart::Output) == "x");
    CHECK(textOf(mock, last, CommandBlockPart::PromptAndOutput) == "$ echo x\nx");

    SECTION("selecting the output selects the columns before the end mark")
    {
        REQUIRE(
            mock.terminal.selectCommandBlock(last, CommandBlockPart::Output, printfBlock->id).has_value());
        CHECK(mock.terminal.selector()->state() == Selection::State::Complete);
        CHECK(mock.terminal.extractSelectionText() == "foo");
    }

    SECTION("selecting the next prompt selects the columns from the end mark on")
    {
        REQUIRE(mock.terminal.selectCommandBlock(last, CommandBlockPart::Prompt).has_value());
        CHECK(mock.terminal.extractSelectionText() == "$ echo x");
    }

    SECTION("the prompt and the output end at the end mark too")
    {
        REQUIRE(mock.terminal.selectCommandBlock(last, CommandBlockPart::PromptAndOutput, printfBlock->id)
                    .has_value());
        CHECK(mock.terminal.extractSelectionText() == "$ printf foo\nfoo");
    }
}

TEST_CASE("Terminal.clearToPrompt.leavesTheLivePromptAtTheTop", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto const evictedBefore = grid.evictedRowCount();
    REQUIRE(mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::Output).has_value());

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "$");
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(0));
    CHECK(grid.evictedRowCount() == evictedBefore + 5); // rows 0..4 went
    CHECK_FALSE(mock.terminal.selectionAvailable());    // a selection of dropped rows cannot survive

    // The records stay, as on any eviction; no row is theirs any more.
    CHECK(mock.terminal.commandBlocks().size() == 3);
    auto const* live = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(live != nullptr);
    CHECK(live == mock.terminal.commandBlocks().current());
}

TEST_CASE("Terminal.clearToPrompt.keepsARunningCommandsPromptAndOutput", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    mock.writeToScreen("old\r\n" + commandStarted("sleep") + "zzz\r\n"); // no end mark: still running

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    auto const& grid = mock.terminal.primaryScreen().grid();
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "$ sleep");
    CHECK(grid.lineTextTrimmed(LineOffset(1)) == "zzz");
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(2));
}

TEST_CASE("Terminal.clearToPrompt.aHeadInTheScrollbackKeepsItsRows", "[semanticblocks][blockactions]")
{
    // Four rows of page: the running command's prompt has scrolled into the history.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("old\r\nolder\r\n" + commandStarted("seq 6") + "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n");
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.historyLineCount() == LineCount(6));
    REQUIRE(grid.lineTextTrimmed(LineOffset(-4)) == "$ seq 6");
    auto const evictedBefore = grid.evictedRowCount();

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.lineTextTrimmed(LineOffset(-4)) == "$ seq 6");
    CHECK(grid.evictedRowCount() == evictedBefore + 2); // old, older
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(3));
}

TEST_CASE("Terminal.clearToPrompt.theCursorsMoveUpWithTheirRows", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    mock.writeToScreen("\0337"sv); // DECSC on the live prompt, row 5
    mock.terminal.inputHandler().setMode(ViMode::Normal);
    mock.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(6), .column = ColumnOffset(1) });

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(mock.terminal.normalModeCursorPosition()
          == CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) });
    CHECK(mock.terminal.primaryScreen().savedCursorState().position
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
}

TEST_CASE("Terminal.clearToPrompt.keepsTheHeadRowWholeWhereTheLastOutputEndedOnIt",
          "[semanticblocks][blockactions]")
{
    // `printf foo` ends without a newline, so the live prompt starts on the row its output ends on. The row
    // is the prompt's and stays whole: the columns of the previous output on it stay too.
    auto mock = makeTerm();
    startCommand(mock, "printf foo");
    mock.writeToScreen("foo" + commandEnd(0) + std::string { PromptStart });
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.lineTextTrimmed(LineOffset(1)) == "foo$");

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "foo$");
    CHECK(mock.terminal.primaryScreen().realCursorPosition()
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) });
}

TEST_CASE("Terminal.clearToPrompt.dropsTheFoldOfADroppedBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    auto const headId = mock.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(0));
    mock.terminal.foldState().collapse(headId);

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    mock.terminal.refreshFoldState();
    CHECK_FALSE(mock.terminal.foldState().isCollapsed(headId));
}

TEST_CASE("Terminal.clearToPrompt.withoutAPromptChangesNothing", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    auto const& grid = mock.terminal.primaryScreen().grid();
    mock.writeToScreen("a\r\nb\r\nc\r\n"sv);

    CHECK(mock.terminal.clearToPrompt() == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "a");

    // A prompt hidden under an alternate screen is not cleared to either: the user would not see it go.
    twoCommandsAndAPrompt(mock);
    REQUIRE(grid.historyLineCount() == LineCount(1));
    mock.writeToScreen("\033[?1049h"sv);
    CHECK(mock.terminal.clearToPrompt() == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.lineTextTrimmed(LineOffset(-1)) == "a");
}

namespace
{

/// Records block @p id as having run in @p directory, judged @p locality, and stamps row @p row with it --
/// what a daemon snapshot hands a mirror, so no shell has to report the directory.
void stampBlock(
    MockTerm<>& mock, uint32_t id, std::filesystem::path const& directory, ContextLocality locality, int row)
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId(id);
    record.state = CommandBlockState::Finished;
    record.workingDirectory =
        WorkingDirectorySnapshot { .path = core::platform::normalizePath(directory), .locality = locality };
    mock.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
    mock.terminal.primaryScreen().grid().lineAt(LineOffset(row)).adoptBlock(CommandBlockId(id));
}

/// Fills @p root with `a/notes.txt` and an empty `b`.
void makeTwoDirectories(std::filesystem::path const& root)
{
    std::filesystem::create_directories(root / "a");
    std::filesystem::create_directories(root / "b");
    std::ofstream(root / "a" / "notes.txt") << "x";
}

/// @return The local path under the mouse pointer, as a path; empty where its text is no path, names no
///         existing file, or is refused.
[[nodiscard]] std::optional<std::filesystem::path> localPathAtMouse(MockTerm<> const& mock)
{
    return mock.terminal.localPathAtMousePosition()
        .value_or(std::nullopt)
        .transform([](std::string const& path) {
            return core::pathFromUtf8(path).value_or(std::filesystem::path {});
        });
}

} // namespace

TEST_CASE("Terminal.localPathAtMousePosition.resolvesAgainstTheDirectoryOfTheCommandThatPrintedIt",
          "[semanticblocks][blockpaths]")
{
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-path" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);
    auto const notes = (root / "a" / "notes.txt").lexically_normal();

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(80) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "b"));
    mock.writeToScreen("see notes.txt\r\n"sv);
    // Over the 't' of "notes.txt" (columns 4-12).
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) },
                                     PixelCoordinate {},
                                     false);

    SECTION("a row of a local block resolves against that block's directory")
    {
        stampBlock(mock, 7, root / "a", ContextLocality::Local, 0);
        CHECK(localPathAtMouse(mock) == notes);
    }

    SECTION("a row of a block that ran elsewhere is refused, even where the name exists here")
    {
        mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
        stampBlock(mock, 7, root / "a", ContextLocality::Foreign, 0);
        CHECK(mock.terminal.localPathAtMousePosition() == std::unexpected(PathRefusal::RanElsewhere));
    }

    SECTION("a row of a block whose directory nothing vouches for keeps the OSC 7 directory")
    {
        stampBlock(mock, 7, root / "a", ContextLocality::Unknown, 0);
        CHECK_FALSE(localPathAtMouse(mock).has_value()); // b holds no notes.txt
    }

    SECTION("a row outside any block keeps the OSC 7 directory")
    {
        CHECK_FALSE(localPathAtMouse(mock).has_value()); // b holds no notes.txt
        mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
        CHECK(localPathAtMouse(mock) == notes);
    }

    SECTION("a row of the alternate screen belongs to no block")
    {
        stampBlock(mock, 7, root / "a", ContextLocality::Foreign, 0);
        mock.writeToScreen("\033[?1049h\033[Hsee notes.txt"sv);
        mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
        CHECK(localPathAtMouse(mock) == notes);
    }
}

TEST_CASE("Terminal.localPathAtMousePosition.refusesAnAbsolutePathPrintedOnAnotherMachine",
          "[semanticblocks][blockpaths]")
{
    // The file it names lives on that machine; one of that name here is another file.
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-abs" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(240) }, LineCount(10) };
    mock.writeToScreen("see " + core::platform::normalizePath(root / "a" / "notes.txt"));
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) },
                                     PixelCoordinate {},
                                     false);
    REQUIRE(localPathAtMouse(mock) == (root / "a" / "notes.txt").lexically_normal());

    SECTION("by a command that ran there")
    {
        stampBlock(mock, 7, root / "a", ContextLocality::Foreign, 0);
        CHECK(mock.terminal.localPathAtMousePosition() == std::unexpected(PathRefusal::RanElsewhere));
    }

    SECTION("on a replica of that machine's terminal, outside any block")
    {
        mock.terminal.bindReplica(ReplicaRole::OtherMachine, HostIdentity {});
        CHECK(mock.terminal.localPathAtMousePosition()
              == std::unexpected(PathRefusal::ReplicaOfAnotherMachine));
    }
}

TEST_CASE("Terminal.hintMode.resolvesPathsAgainstTheDirectoryOfTheCommandThatPrintedThem",
          "[semanticblocks][blockpaths]")
{
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-hint" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);
    auto const notes = (root / "a" / "notes.txt").lexically_normal();

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.writeToScreen("local notes.txt\r\nremote notes.txt\r\nplain notes.txt"sv);
    stampBlock(mock, 7, root / "a", ContextLocality::Local, 0);
    stampBlock(mock, 8, root / "a", ContextLocality::Foreign, 1);

    auto const activate = [&] {
        mock.terminal.flushInput();
        mock.terminal.activateHintMode(
            vtbackend::HintModeRequest { .patterns = vtbackend::HintModeHandler::builtinPatterns(),
                                         .action = vtbackend::HintAction::Copy });
        return mock.terminal.hintMatches();
    };

    SECTION("beside an OSC 7 directory")
    {
        // Row 0 resolves into a; row 1 ran on another machine; row 2 resolves into b, which has no notes.txt
        // -- though the same name was found in a a row earlier.
        mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "b"));
        auto const matches = activate();
        REQUIRE(matches.size() == 1);
        CHECK(matches[0].start.line == LineOffset(0));
        CHECK(core::pathFromUtf8(matches[0].matchedText) == notes);
    }

    SECTION("without one: a block's own directory is enough")
    {
        auto const matches = activate();
        REQUIRE(matches.size() == 1);
        CHECK(matches[0].start.line == LineOffset(0));
        CHECK(core::pathFromUtf8(matches[0].matchedText) == notes);
    }

    SECTION("a command that ran elsewhere is refused even where the OSC 7 directory holds the name")
    {
        mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
        auto const matches = activate();
        REQUIRE(matches.size() == 2);
        CHECK(matches[0].start.line == LineOffset(0));
        CHECK(matches[1].start.line == LineOffset(2));
    }
}

TEST_CASE("Terminal.hintMode.blocksWithoutALocalDirectoryLeavePathsAsWritten", "[semanticblocks][blockpaths]")
{
    // A shell that marks its commands but reports no directory: with nothing to resolve a path against, it is
    // offered as written, as before there were blocks -- not refused for want of a directory.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.writeToScreen("\033]133;A\033\\$ make\r\n\033]133;C\033\\see src/main.cpp\r\n\033]133;D;0\033\\"sv);
    REQUIRE(mock.terminal.commandBlockAt(LineOffset(1)) != nullptr);
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });

    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].matchedText == "src/main.cpp");
}

namespace
{

/// @return The hint matches of a fresh activation over @p mock's page, with the builtin patterns.
[[nodiscard]] std::vector<HintMatch> hintsOf(MockTerm<>& mock, HintAction action = HintAction::Copy)
{
    mock.terminal.flushInput();
    mock.terminal.activateHintMode(
        HintModeRequest { .patterns = HintModeHandler::builtinPatterns(), .action = action });
    return mock.terminal.hintMatches();
}

} // namespace

TEST_CASE("Terminal.hintMode.aRowOutsideAnyBlockKeepsItsPathsAsWrittenBesideALocalBlock",
          "[semanticblocks][blockpaths]")
{
    // Whether a row's paths resolve is the row's own question: a block elsewhere that ran in a local
    // directory gives a row outside any block nothing to resolve against.
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-hint-mixed" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.writeToScreen("local notes.txt\r\nsee src/main.cpp"sv);
    stampBlock(mock, 7, root / "a", ContextLocality::Local, 0);

    auto const matches = hintsOf(mock);
    REQUIRE(matches.size() == 2);
    CHECK(core::pathFromUtf8(matches[0].matchedText) == (root / "a" / "notes.txt").lexically_normal());
    CHECK(matches[1].start.line == LineOffset(1));
    CHECK(matches[1].matchedText == "src/main.cpp");
}

TEST_CASE("Terminal.hintMode.offersNoPathACommandPrintedOnAnotherMachineWithoutALocalDirectory",
          "[semanticblocks][blockpaths]")
{
    // No directory on this machine anywhere -- a local shell that reports none, ssh-ing to a host whose shell
    // does -- and the remote command's absolute path names a file there all the same: Open must not open
    // this machine's file of that name.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.writeToScreen("cat /etc/hosts\r\nsee /etc/hosts"sv);
    stampBlock(mock, 7, "/srv/app", ContextLocality::Foreign, 0);

    auto const matches = hintsOf(mock, HintAction::Open);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].start.line == LineOffset(1)); // outside any block: as written
    CHECK(matches[0].matchedText == "/etc/hosts");
}

TEST_CASE("Terminal.localPathAtMousePosition.refusesWhatARemoteShellPrintsInsideALocalBlock",
          "[semanticblocks][blockpaths]")
{
    // `ssh host` is a local command. A remote shell that reports its directory but marks no commands leaves
    // everything it prints in that block, which started in a local directory holding the same name.
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-ssh" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
    mock.writeToScreen("\033]133;A\033\\$ ssh host\r\n\033]133;C\033\\"sv);

    SECTION("while the session reports no other directory: the block's own")
    {
        mock.writeToScreen("notes.txt\r\n"sv);
        pointAt(mock, 1);
        CHECK(localPathAtMouse(mock) == (root / "a" / "notes.txt").lexically_normal());
    }

    SECTION("once it reports one on another host: refused, by the mouse and by hint mode")
    {
        mock.writeToScreen("\033]7;file://otherhost.invalid/srv/app\033\\notes.txt\r\n"sv);
        pointAt(mock, 1);
        CHECK(mock.terminal.localPathAtMousePosition() == std::unexpected(PathRefusal::RanElsewhere));
        CHECK(hintsOf(mock).empty());
    }
}

TEST_CASE("Terminal.localPathAtMousePosition.keepsRefusingWhatARemoteShellPrintedOnceTheLocalPromptIsBack",
          "[semanticblocks][blockpaths]")
{
    // The remote shell of `ssh host` reports its directory and prints a name the block's local directory
    // holds too. Back at the local prompt the terminal's directory is local again, yet those rows were
    // printed there.
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-ssh-done" };
    auto const& root = tmpDir.path();
    makeTwoDirectories(root);

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root / "a"));
    mock.writeToScreen("\033]133;A\033\\$ ssh host\r\n\033]133;C\033\\"sv);
    mock.writeToScreen("\033]7;file://otherhost.invalid/srv/app\033\\notes.txt\r\n"sv);
    auto const localPrompt = "\033]7;" + fileUrlOf(root / "a") + "\033\\\033]133;A\033\\$ ";

    SECTION("ssh's end reported")
    {
        mock.writeToScreen("\033]133;D;0\033\\" + localPrompt);
    }

    SECTION("ssh's end implied by the local prompt")
    {
        mock.writeToScreen(localPrompt);
    }

    auto const* const ssh = mock.terminal.commandBlockAt(LineOffset(1));
    REQUIRE(ssh != nullptr);
    REQUIRE(ssh->state == CommandBlockState::Finished);
    pointAt(mock, 1);
    CHECK(mock.terminal.localPathAtMousePosition() == std::unexpected(PathRefusal::RanElsewhere));
    CHECK(hintsOf(mock).empty());
}

TEST_CASE("Terminal.localPathAtMousePosition.followsACdInTheCommandLine", "[semanticblocks][blockpaths]")
{
    // `cd build && ninja` started in proj and printed paths relative to proj/build, which the shell reports
    // (OSC 7) only as the next prompt starts. The file is proj/src/f; proj/../src/f is no file.
    auto const tmpDir = core::testing::ScopedTempDir { "contour-block-cd" };
    auto const root = tmpDir.path() / "proj";
    std::filesystem::create_directories(root / "build");
    std::filesystem::create_directories(root / "src");
    std::ofstream(root / "src" / "f") << "x";
    auto const file = (root / "src" / "f").lexically_normal();

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory(fileUrlOf(root));
    mock.writeToScreen("\033]133;A\033\\$ cd build && ninja\r\n\033]133;C\033\\../src/f:12: error\r\n"
                       "\033]133;D;1\033\\\033]7;"
                       + fileUrlOf(root / "build") + "\033\\\033]133;A\033\\$ ");

    // Over the '/' of "../src/f".
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) },
                                     PixelCoordinate {},
                                     false);
    CHECK(localPathAtMouse(mock) == file);

    auto const matches = hintsOf(mock);
    auto const onOutput =
        std::ranges::find(matches, LineOffset(1), [](HintMatch const& match) { return match.start.line; });
    REQUIRE(onOutput != matches.end());
    CHECK(core::pathFromUtf8(onOutput->matchedText) == file);
}

TEST_CASE("Terminal.hintMode.offersNoPathOnAReplicaOfAnotherMachine", "[semanticblocks][blockpaths]")
{
    // Every path on it names a file on the daemon's machine, an absolute one included.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.writeToScreen("see /etc/hosts"sv);
    REQUIRE(hintsOf(mock).size() == 1);

    mock.terminal.bindReplica(ReplicaRole::OtherMachine, HostIdentity {});
    CHECK(hintsOf(mock).empty());
}

namespace
{

/// The path under a mouse pointer hovering over @p column of the first row of @p mock.
[[nodiscard]] std::optional<std::string> pathTextAtColumn(MockTerm<>& mock, int column)
{
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(column) },
                                     PixelCoordinate {},
                                     false);
    auto const _ = std::scoped_lock { mock.terminal };
    return mock.terminal.pathTextAtMousePosition();
}

} // namespace

TEST_CASE("Terminal.pathTextAtMousePosition.countsAWideCharacterAsTwoColumns", "[semanticblocks][blockpaths]")
{
    auto mock = MockTerm<> { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("\xe6\x9d\xb1\xe4\xba\xac src/a.txt"sv); // 東京 takes columns 0-3; the path 5-13
    mock.terminal.flushInput();

    CHECK(pathTextAtColumn(mock, 13) == "src/a.txt");
    CHECK(pathTextAtColumn(mock, 5) == "src/a.txt");
    CHECK(pathTextAtColumn(mock, 4) == std::nullopt);
    CHECK(pathTextAtColumn(mock, 14) == std::nullopt);
}

TEST_CASE("Terminal.pathTextAtMousePosition.countsAClusterAsOneColumn", "[semanticblocks][blockpaths]")
{
    auto mock = MockTerm<> { PageSize { LineCount(3), ColumnCount(40) } };
    // Two "e" + COMBINING ACUTE ACCENT clusters, one column each; the path takes columns 3-11.
    mock.writeToScreen("e\xcc\x81"
                       "e\xcc\x81 src/a.txt"sv);
    mock.terminal.flushInput();

    CHECK(pathTextAtColumn(mock, 11) == "src/a.txt");
    CHECK(pathTextAtColumn(mock, 3) == "src/a.txt");
    CHECK(pathTextAtColumn(mock, 2) == std::nullopt);
}

TEST_CASE("Terminal.pathTextAtMousePosition.findsASecondPathRightOfWideCharacters",
          "[semanticblocks][blockpaths]")
{
    auto mock = MockTerm<> { PageSize { LineCount(3), ColumnCount(40) } };
    mock.writeToScreen("src/a.txt \xe6\x9d\xb1\xe4\xba\xac b.txt"sv); // the path 0-8; 東京 10-13; b.txt 15-19
    mock.terminal.flushInput();

    CHECK(pathTextAtColumn(mock, 8) == "src/a.txt");
    CHECK(pathTextAtColumn(mock, 15) == "b.txt");
    CHECK(pathTextAtColumn(mock, 19) == "b.txt");
    CHECK(pathTextAtColumn(mock, 12) == std::nullopt);
}
