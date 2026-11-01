// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/Selector.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/SemanticBlockTracker.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Functions.hpp>
#include <vtbackend/vt/SemanticBlockQuery.hpp>
#include <vtbackend/vt/Sequence.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cctype>
#include <chrono>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;
using namespace std::string_view_literals;

namespace
{

/// Extracts the 4-integer token from a DECSET 2034 DCS reply.
///
/// The reply format is: ESC P > 2034 ; 1 b T1 ; T2 ; T3 ; T4 ESC backslash
/// @param replyData The raw reply data from the terminal.
/// @return The token as a Token array, or std::nullopt if not found.
std::optional<SemanticBlockTracker::Token> extractTokenFromDecsetReply(std::string const& replyData)
{
    // Look for "\033P>2034;1b" prefix
    auto const prefix = std::string_view("\033P>2034;1b");
    auto const pos = replyData.find(prefix);
    if (pos == std::string::npos)
        return std::nullopt;

    // Find the ST terminator
    auto const dataStart = pos + prefix.size();
    auto const stPos = replyData.find("\033\\", dataStart);
    if (stPos == std::string::npos)
        return std::nullopt;

    // Parse 4 semicolon-separated integers: "T1;T2;T3;T4"
    auto const tokenStr = replyData.substr(dataStart, stPos - dataStart);
    auto token = SemanticBlockTracker::Token {};
    auto start = size_t { 0 };
    for (auto i = 0; i < 4; ++i)
    {
        auto const sep = (i < 3) ? tokenStr.find(';', start) : tokenStr.size();
        if (sep == std::string::npos)
            return std::nullopt;
        token[i] = static_cast<uint16_t>(std::stoul(tokenStr.substr(start, sep - start)));
        start = sep + 1;
    }
    return token;
}

/// Enables mode 2034 and returns the session token from the DCS reply.
///
/// @param mc The MockTerm to operate on.
/// @return The extracted token.
SemanticBlockTracker::Token enableModeAndGetToken(MockTerm<>& mc)
{
    mc.resetReplyData();
    mc.writeToScreen(DECSM(2034));
    mc.terminal.flushInput();
    auto const token = extractTokenFromDecsetReply(mc.replyData());
    REQUIRE(token.has_value());
    return *token;
}

/// Builds an authenticated SBQUERY escape sequence with token parameters.
///
/// @param queryType The SBQUERY query type (Ps).
/// @param count The count parameter (Pn).
/// @param token The session token to embed as parameters 2..5.
/// @return The complete CSI escape sequence string.
std::string authenticatedSBQuery(unsigned queryType, unsigned count, SemanticBlockTracker::Token const& token)
{
    return SBQUERY(queryType, count, token[0], token[1], token[2], token[3]);
}

/// Sends an authenticated SBQUERY and collects the terminal's whole reply to it, and nothing else.
///
/// @param mc The MockTerm to query.
/// @param query The SBQUERY sequence.
/// @return The reply, as the shell reads it from its stdin.
std::string queryBlocksWhole(MockTerm<>& mc, std::string_view query)
{
    mc.resetReplyData();
    mc.writeToScreen(query);
    mc.terminal.flushInput();
    return mc.replyData();
}

/// @p reply without the keys JSON version 2 appended to every block (`id`, `cwd`, `startedAt`, `durationMs`).
///
/// What the cases that pin version 1's keys compare: that they are byte for byte what they were is the
/// promise version 2 makes, and the appended keys are pinned by cases of their own.
std::string withoutVersion2Keys(std::string const& reply)
{
    static auto const appended = std::regex {
        R"re(,"id":[0-9]+,"cwd":(null|"(\\.|[^"\\])*"),"startedAt":(null|"[^"]*"),"durationMs":(null|[0-9]+)\})re"
    };
    return std::regex_replace(reply, appended, "}");
}

/// Sends an authenticated SBQUERY and collects the terminal's reply to it, and nothing else.
///
/// @param mc The MockTerm to query.
/// @param queryType The SBQUERY query type (Ps).
/// @param count The count parameter (Pn).
/// @param token The session token to authenticate with.
/// @return The reply, as the shell reads it from its stdin, with version 1's keys only (withoutVersion2Keys).
std::string queryBlocks(MockTerm<>& mc,
                        unsigned queryType,
                        unsigned count,
                        SemanticBlockTracker::Token const& token)
{
    return withoutVersion2Keys(queryBlocksWhole(mc, authenticatedSBQuery(queryType, count, token)));
}

/// Simulates a complete command cycle: prompt -> command -> output -> finish.
/// Inserts newlines to mirror real shell behavior (Enter after prompt, newline after output).
void simulateCommand(MockTerm<>& mc,
                     std::string_view promptText,
                     std::string_view commandLine,
                     std::string_view outputText,
                     int exitCode)
{
    // OSC 133;A - Prompt Start
    mc.writeToScreen("\033]133;A\033\\");
    // Write prompt text
    mc.writeToScreen(promptText);
    // OSC 133;B - Prompt End
    mc.writeToScreen("\033]133;B\033\\");
    // Newline simulates the Enter keypress after the command
    mc.writeToScreen("\n");
    // OSC 133;C - Command Output Start (with cmdline_url)
    mc.writeToScreen(std::string("\033]133;C;cmdline_url=") + std::string(commandLine) + "\033\\");
    // Write command output
    mc.writeToScreen(outputText);
    // Trailing newline (most commands end their output with one)
    mc.writeToScreen("\n");
    // OSC 133;D - Command Finished
    mc.writeToScreen(std::string("\033]133;D;") + std::to_string(exitCode) + "\033\\");
}

/// @param text The text to search.
/// @param needles What to look for.
/// @return Whether each of @p needles occurs in @p text, every one after the one before it.
bool appearInOrder(std::string_view text, std::initializer_list<std::string_view> needles)
{
    auto from = size_t { 0 };
    for (auto const needle: needles)
    {
        auto const pos = text.find(needle, from);
        if (pos == std::string_view::npos)
            return false;
        from = pos + needle.size();
    }
    return true;
}

} // namespace

TEST_CASE("ShellIntegration.OSC_133")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const& blocks = mc.terminal.commandBlocks();

    SECTION("A: Prompt Start")
    {
        mc.writeToScreen("\033]133;A\033\\");
        REQUIRE(blocks.size() == 1);
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Prompting);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("A: Prompt Start with click_events")
    {
        // click_events= asks for click-to-move in the prompt, a follow-up rather than part of a block: the
        // parameter is accepted and ignored, and the prompt is recorded exactly as without it.
        mc.writeToScreen("\033]133;A;click_events=1\033\\");
        CHECK(blocks.size() == 1);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("B: Prompt End")
    {
        mc.writeToScreen("\033]133;B\033\\");
        // The border lives on the line; the store has nothing to record for a prompt it never saw start.
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::PromptEnd));
        CHECK(blocks.size() == 0);
    }

    SECTION("C: Command Output Start")
    {
        // A ;C with no ;A before it -- a shell integration sourced halfway through a session -- mints the
        // record it needs.
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Running);
        CHECK(blocks.current()->commandLineSource == CommandLineSource::None);
        CHECK(blocks.current()->commandLine.empty());
    }

    SECTION("C: Command Output Start with cmdline_url")
    {
        // "foo%20bar" unescapes to "foo bar"
        mc.writeToScreen("\033]133;C;cmdline_url=foo%20bar\033\\");
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->commandLine.raw() == "foo bar");
        CHECK(blocks.current()->commandLineSource == CommandLineSource::Reported);
    }

    SECTION("D: Command Finished")
    {
        mc.writeToScreen("\033]133;C\033\\");
        mc.writeToScreen("\033]133;D\033\\");
        REQUIRE(blocks.lastFinished() != nullptr);
        CHECK(blocks.lastFinished()->exitCode == 0);
        CHECK(mc.finishedCommandBlocks.size() == 1);
    }

    SECTION("D: Command Finished with exit code")
    {
        mc.writeToScreen("\033]133;C\033\\");
        mc.writeToScreen("\033]133;D;123\033\\");
        REQUIRE(blocks.lastFinished() != nullptr);
        CHECK(blocks.lastFinished()->exitCode == 123);
    }

    SECTION("D: with no command running is ignored")
    {
        // tcsh emits ;D unconditionally, and a nested shell's exit leaves one behind: neither finished a
        // command this terminal saw start.
        mc.writeToScreen("\033]133;D;1\033\\");
        CHECK(blocks.size() == 0);
        CHECK(mc.finishedCommandBlocks.empty());
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::CommandEnd));
    }

    SECTION("N: New Command starts a prompt as A does")
    {
        mc.writeToScreen("\033]133;N\033\\");
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Prompting);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
        CHECK(mc.terminal.currentScreen().cursor().position == CellLocation {}); // and does no fresh-line

        // A command still running when the next one is announced has ended, as at a new ;A.
        mc.writeToScreen("$ \033]133;B\033\\make\r\n\033]133;C\033\\built\r\n\033]133;N;aid=7\033\\");
        CHECK(blocks.size() == 2);
        REQUIRE(blocks.current() != nullptr);
        CHECK(blocks.current()->state == CommandBlockState::Prompting);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(2)).contains(LineFlag::Marked));
    }

    SECTION("I: Prompt End whose input ends with its line")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;I\033\\");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::PromptEnd));
        auto const span = mc.terminal.livePromptSpan();
        REQUIRE(span.has_value());
        CHECK(span->inputBegin == ColumnOffset(2));
    }

    SECTION("L: Fresh Line")
    {
        auto const& screen = mc.terminal.currentScreen();
        mc.writeToScreen("abc\033]133;L\033\\");
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
        mc.writeToScreen("\033]133;L\033\\"); // already at the start of a line: nothing to do
        CHECK(screen.cursor().position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
    }
}

TEST_CASE("ShellIntegration.OSC 133;L after a full line starts the next one")
{
    // The cursor stands on the last column, its wrap pending. The fresh line is the next one, which then
    // continues nothing: what is written there starts a line of its own.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    mc.writeToScreen("0123456789\033]133;L\033\\x");

    auto const& screen = mc.terminal.currentScreen();
    CHECK(screen.cursor().position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) });
    CHECK(screen.grid().lineAt(LineOffset(1)).toUtf8Trimmed() == "x");
    CHECK_FALSE(screen.grid().lineAt(LineOffset(1)).wrapped());
}

// ==================== Semantic Block Protocol Tests ====================

TEST_CASE("SemanticBlockProtocol.LineFlags")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("OutputStart and CommandEnd flags set when mode 2034 is on")
    {
        // Enable mode 2034
        mc.writeToScreen(DECSM(2034));

        // OSC 133;C should set OutputStart on the current line
        mc.writeToScreen("\033]133;C\033\\");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::OutputStart));

        // Write some output and move to a new line
        mc.writeToScreen("output\n");

        // OSC 133;D should set CommandEnd on the current line
        mc.writeToScreen("\033]133;D\033\\");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(1)).contains(LineFlag::CommandEnd));
    }

    SECTION("Flags are recorded even when mode 2034 is off")
    {
        // Mode 2034 is off by default, and the line flags are recorded anyway: they are terminal MEMORY of
        // what the shell said, exactly like OSC 133;A's Marked flag has always been. Mode 2034 gates the
        // semantic-block READER PROTOCOL (see the section below), not what the terminal knows about its own
        // scrollback -- gating the flags too would leave "copy last command output" with nothing to read
        // for every shell that speaks plain OSC 133.
        mc.writeToScreen("\033]133;C\033\\");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::OutputStart));

        mc.writeToScreen("output\n");

        mc.writeToScreen("\033]133;D\033\\");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(1)).contains(LineFlag::CommandEnd));
    }

    SECTION("What mode 2034 gates: the reader protocol, not the record store")
    {
        // The other half of the split above, pinned so it cannot quietly drift: with mode 2034 off the flags
        // land AND the terminal records the block -- its gutter and notifications read the same
        // store -- but nothing is reported over the protocol.
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::OutputStart));
        REQUIRE(mc.terminal.commandBlocks().lastFinished() != nullptr);
        CHECK(mc.terminal.commandBlocks().lastFinished()->commandLine.raw() == "ls");

        mc.resetReplyData();
        mc.writeToScreen(SBQUERY(SBQueryType::LastCommand));
        mc.terminal.flushInput();
        CHECK(mc.replyData().contains("\033P>0b\033\\"));
    }
}

TEST_CASE("SemanticBlockProtocol.theLastCommandWithoutMode2034")
{
    // The GUI scenario: a shell emits plain OSC 133 and never enables mode 2034 (nothing in the wild does),
    // and the context menu's "Copy Last Command Output" must still have something to copy.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ls\r\n");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("file1\r\n");
    mc.writeToScreen("file2\r\n");
    // precmd: the command ends and the next prompt starts on the very same line.
    mc.writeToScreen("\033]133;D;0\033\\");
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ ls");

    // The new prompt must not have leaked into the finished command's output.
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == "file1\nfile2");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput) == "$ ls\nfile1\nfile2");
}

TEST_CASE("SemanticBlockProtocol.noLastCommandWithoutShellIntegration")
{
    // No OSC 133 anywhere: the menu entries must be hidden, so the answer has to be "nothing", not garbage.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.writeToScreen("just some plain output\n");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput) == "<error>");
}

// {{{ output that does not end in a newline
namespace
{
/// What the bundled shell integration actually puts on the wire between two commands.
///
/// precmd emits ;D (the command finished) and then ;A (a new prompt starts) back to back at the cursor,
/// before PS1 prints a single character. So whenever a command's output did not end in a newline, both
/// marks land on the output's last line, and the prompt is painted onto it too.
void writePrecmdAndPrompt(MockTerm<>& mc, std::string_view prompt, int exitCode = 0)
{
    mc.writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen(prompt);
}

/// The other half: a prompt (;A), the command echoed and entered, then its output (;C).
///
/// Deliberately NOT the file's simulateCommand() above, which always terminates the output with a newline.
/// These tests exist for the case where it does not.
void writePromptAndRun(MockTerm<>& mc, std::string_view promptLine, std::string_view output)
{
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen(promptLine);
    mc.writeToScreen("\r\n"); // the Enter that runs it
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen(output); // no trailing newline: that is the whole point
}
} // namespace

TEST_CASE("SemanticBlockProtocol.outputWithoutTrailingNewline")
{
    // `printf 'a\nb\nc'` leaves the cursor after the `c`, so the new prompt shares that line with the
    // command's last output line. Neither owner may swallow the other: dropping the line loses the `c`,
    // taking all of it copies the next prompt along with the output.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    writePromptAndRun(mc, R"($ printf 'a\nb\nc')", "a\r\nb\r\nc");
    writePrecmdAndPrompt(mc, "$ ");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == "a\nb\nc");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == R"($ printf 'a\nb\nc')");
}

TEST_CASE("SemanticBlockProtocol.outputThatFitsOnTheLineItStartedOn")
{
    // `printf hello`: OutputStart, CommandEnd and the next prompt's Marked all land on one line, and the
    // whole of the command's output is the five columns before the ;D. Getting this wrong loses the output
    // in its entirety.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    writePromptAndRun(mc, "$ printf hello", "hello");
    writePrecmdAndPrompt(mc, "$ ");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == "hello");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ printf hello");
}

TEST_CASE("SemanticBlockProtocol.marksSurviveResize")
{
    // The marks name the LOGICAL line, and the command-end offset counts that line's columns — so a resize,
    // which only re-chops a logical line into different physical pieces, must leave every block untouched.
    // Widening used to erase outright any mark that a wrap had left on a continuation line.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(10) } };

    // 25 columns of output on a 10-column page: two wraps, so the cursor — and with it every mark precmd is
    // about to emit — ends up on the second continuation line.
    writePromptAndRun(mc, "$ wrap", std::string(25, 'x'));
    writePrecmdAndPrompt(mc, "$ ");

    auto const expectedOutput = std::string(25, 'x');

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == expectedOutput);
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ wrap");

    SECTION("widening the window")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(40) });

        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == expectedOutput);
        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ wrap");
    }

    SECTION("narrowing the window")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(6) });

        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == expectedOutput);
        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ wrap");
    }
}

TEST_CASE("SemanticBlockProtocol.noCommandAtAllIsNoBlock")
{
    // A prompt carrying the CommandEnd of a command whose every line has scrolled away describes no
    // command. Reporting it as a block would have "Copy Last Command Output" wipe the clipboard with "".
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    writePrecmdAndPrompt(mc, "$ ");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput) == "<error>");
}
// }}}

TEST_CASE("SemanticBlockProtocol.StoreMetadata", "[semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("The store keeps the command line and the exit code")
    {
        mc.writeToScreen(DECSM(2034));

        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls%20-la\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        auto const* block = mc.terminal.commandBlocks().lastFinished();
        REQUIRE(block != nullptr);
        CHECK(block->state == CommandBlockState::Finished);
        CHECK(block->commandLine.raw() == "ls -la");
        CHECK(block->commandLineSource == CommandLineSource::Reported);
        CHECK(block->exitCode == 0);
    }

    SECTION("Disabling mode 2034 ends the reader session but keeps the store")
    {
        auto const token = enableModeAndGetToken(mc);
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("\033]133;C;cmdline_url=test\033\\");
        mc.writeToScreen("\033]133;D;0\033\\");

        mc.writeToScreen(DECRM(2034));

        auto const& tracker = mc.terminal.semanticBlockTracker();
        CHECK_FALSE(tracker.token().has_value());
        CHECK_FALSE(tracker.validateToken(token));
        CHECK(mc.terminal.commandBlocks().size() == 1);
    }
}

TEST_CASE("SemanticBlockProtocol.DECRQM")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("DECRQM reports mode 2034 as reset by default")
    {
        mc.resetReplyData();
        mc.writeToScreen(DECRQM(2034));
        mc.terminal.flushInput();
        // Response should be CSI ? 2034 ; 2 $ y  (2 = reset)
        CHECK(mc.replyData().contains("2034;2$y"));
    }

    SECTION("DECRQM reports mode 2034 as set after enabling")
    {
        mc.writeToScreen(DECSM(2034));
        mc.terminal.flushInput();
        mc.resetReplyData();
        mc.writeToScreen(DECRQM(2034));
        mc.terminal.flushInput();
        // Response should be CSI ? 2034 ; 1 $ y  (1 = set)
        CHECK(mc.replyData().contains("2034;1$y"));
    }
}

TEST_CASE("SemanticBlockProtocol.QueryDisabled")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("Query without enabling mode returns error DCS")
    {
        mc.resetReplyData();
        // Query last command
        mc.writeToScreen(SBQUERY(SBQueryType::LastCommand));
        mc.terminal.flushInput();
        // Should get error DCS: ESC P > 0 b ESC backslash
        CHECK(mc.replyData().contains("\033P>0b\033\\"));
    }
}

TEST_CASE("SemanticBlockProtocol.QueryLastCommand")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture the session token
    auto const token = enableModeAndGetToken(mc);

    // Simulate a complete command
    simulateCommand(mc, "$ ", "ls", "file1\nfile2", 0);

    // Start a new prompt so the previous command is finalized
    mc.writeToScreen("\033]133;A\033\\");

    mc.terminal.flushInput();
    // Query last command with token
    auto const& reply = queryBlocks(mc, SBQueryType::LastCommand, 1, token);

    // Should get success DCS
    REQUIRE(reply.contains("\033P>1b"));
    // Should contain JSON with version
    CHECK(reply.contains("\"version\":2"));
    // Should contain the command
    CHECK(reply.contains("\"command\":\"ls\""));
    // Should contain exit code
    CHECK(reply.contains("\"exitCode\":0"));
    // Should be marked as finished
    CHECK(reply.contains("\"finished\":true"));
}

TEST_CASE("SemanticBlockProtocol.QueryLastNCommands")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture the session token
    auto const token = enableModeAndGetToken(mc);

    // Simulate two commands
    simulateCommand(mc, "$ ", "cmd1", "out1", 0);
    simulateCommand(mc, "$ ", "cmd2", "out2", 1);

    // Start new prompt to finalize
    mc.writeToScreen("\033]133;A\033\\");

    // Query last 2 commands with token
    auto const& reply = queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token);
    REQUIRE(reply.contains("\033P>1b"));
    // Both commands should be present
    CHECK(reply.contains("\"command\":\"cmd1\""));
    CHECK(reply.contains("\"command\":\"cmd2\""));
}

TEST_CASE("SemanticBlockProtocol.QueryInProgress")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture the session token
    auto const token = enableModeAndGetToken(mc);

    // Start a command but don't finish it (no OSC 133;D)
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");
    mc.writeToScreen("\033]133;C;cmdline_url=running\033\\");
    mc.writeToScreen("partial output");

    // Query in-progress command with token
    auto const& reply = queryBlocks(mc, SBQueryType::InProgress, 1, token);
    REQUIRE(reply.contains("\033P>1b"));
    // Should be marked as not finished
    CHECK(reply.contains("\"finished\":false"));
    // Should contain the command
    CHECK(reply.contains("\"command\":\"running\""));
}

TEST_CASE("SemanticBlockProtocol.aHostileCommandLineIsRepliedAsSafeJson", "[semanticblocks]")
{
    // The command line arrives from whatever wrote to the PTY, and the reply goes back to it inside a DCS. So
    // the line is sanitised as for insertion -- ESC, DEL and C1 dropped, ill-formed UTF-8 replaced -- and the
    // control codes left are escaped: no byte of it may end the DCS early (ESC \, or the 0x9C a C1-aware
    // reader takes for ST), nor leave the JSON ill-formed UTF-8.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    simulateCommand(mc, "$ ", "a%9Cb%1B%5B31mc%0Ad%09e%C2%85f%7Fg", "x", 0);
    mc.writeToScreen("\033]133;A\033\\");

    auto const& reply = queryBlocks(mc, SBQueryType::LastCommand, 1, token);

    REQUIRE(reply.starts_with("\033P>1b"));
    REQUIRE(reply.ends_with("\033\\"));
    auto const json = std::string_view { reply }.substr(5, reply.size() - 5 - 2);
    CHECK(json.contains(R"("command":"a)"
                        "\xEF\xBF\xBD"
                        R"(b[31mc\nd\tefg")"));
    CHECK(std::ranges::none_of(json, [](char ch) {
        auto const byte = static_cast<unsigned char>(ch);
        return byte < 0x20 || byte == 0x7F;
    }));
}

TEST_CASE("SemanticBlockProtocol.AnUnknownQueryTypeIsRefused", "[semanticblocks]")
{
    // Only Ps 1 to 4 mean something. Any other is answered as a query with no data, not as Ps 2's "the last N
    // blocks": a reader asking a question this terminal does not know must not read an answer to another.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    simulateCommand(mc, "$ ", "cmd1", "out1", 0);
    simulateCommand(mc, "$ ", "cmd2", "out2", 0);
    mc.writeToScreen("\033]133;A\033\\");

    for (auto const queryType: { 5U, 6U, 65535U })
    {
        INFO("Ps " << queryType);
        CHECK(queryBlocks(mc, queryType, 2, token) == "\033P>0b\033\\");
    }

    // A zero, like an omitted Ps, is the default: the last command.
    auto const& reply = queryBlocks(mc, 0, 2, token);
    CHECK(reply.contains(R"("command":"cmd2")"));
    CHECK_FALSE(reply.contains(R"("command":"cmd1")"));
}

TEST_CASE("SemanticBlockProtocol.NoCompletedCommands")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture the session token
    auto const token = enableModeAndGetToken(mc);

    // Query last command when none exists, with valid token
    auto const& reply = queryBlocks(mc, SBQueryType::LastCommand, 1, token);
    // Should get error DCS (no data)
    CHECK(reply.contains("\033P>0b\033\\"));
}

// ==================== Token Authentication Tests ====================

TEST_CASE("SemanticBlockProtocol.TokenOnEnable")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // The session's token is what the injected source hands out, and the reply carries it as is.
    mc.nextSemanticBlockToken = { 41394, 50132, 58870, 1816 };
    mc.resetReplyData();
    mc.writeToScreen(DECSM(2034));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    CHECK(reply == "\033P>2034;1b41394;50132;58870;1816\033\\");

    // Extract and validate the token
    auto const token = extractTokenFromDecsetReply(reply);
    REQUIRE(token.has_value());

    // Token should match what the tracker has
    auto const& tracker = mc.terminal.semanticBlockTracker();
    REQUIRE(tracker.token().has_value());
    CHECK(tracker.token().value() == token.value());
}

TEST_CASE("SemanticBlockProtocol.TokenInvalidatedOnDisable")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable and capture token
    auto const token = enableModeAndGetToken(mc);

    // Disable mode
    mc.writeToScreen(DECRM(2034));

    // Token should be cleared
    auto const& tracker = mc.terminal.semanticBlockTracker();
    CHECK_FALSE(tracker.token().has_value());

    // Validating the old token should fail
    CHECK_FALSE(tracker.validateToken(token));
}

TEST_CASE("SemanticBlockProtocol.ResetEndsTheReaderSession", "[semanticblocks]")
{
    // RIS returns every mode to its power-up state, this one included: the reader session ends with the mode,
    // and the token it was handed is no longer valid, as after DECRST 2034.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);

    mc.writeToScreen("\033c"); // RIS

    auto const& tracker = mc.terminal.semanticBlockTracker();
    CHECK_FALSE(mc.terminal.isModeEnabled(DECMode::SemanticBlockProtocol));
    CHECK_FALSE(tracker.isEnabled());
    CHECK_FALSE(tracker.token().has_value());
    CHECK_FALSE(tracker.validateToken(token));
}

TEST_CASE("SemanticBlockProtocol.QueryWithoutToken")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034
    mc.writeToScreen(DECSM(2034));
    mc.terminal.flushInput();

    // Simulate a complete command
    simulateCommand(mc, "$ ", "ls", "file1", 0);
    mc.writeToScreen("\033]133;A\033\\");

    mc.resetReplyData();
    // Query WITHOUT token (only 2 params, not 6)
    mc.writeToScreen(SBQUERY(SBQueryType::LastCommand, 1));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    // Should get status 2: authentication required
    CHECK(reply.contains("\033P>2b\033\\"));
}

TEST_CASE("SemanticBlockProtocol.QueryWithWrongToken")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture token
    enableModeAndGetToken(mc);

    // Simulate a complete command
    simulateCommand(mc, "$ ", "ls", "file1", 0);
    mc.writeToScreen("\033]133;A\033\\");

    // Query with a fabricated wrong token
    auto const wrongToken = SemanticBlockTracker::Token { 0xDEAD, 0xBEEF, 0xCAFE, 0xBABE };
    auto const& reply = queryBlocks(mc, SBQueryType::LastCommand, 1, wrongToken);
    // Should get status 3: authentication failed
    CHECK(reply.contains("\033P>3b\033\\"));
}

TEST_CASE("SemanticBlockProtocol.ATokenPartOutOfRangeFailsAuthentication", "[semanticblocks]")
{
    // The parser reads every parameter past 65535 as 65535, so that value stands for all of them and may
    // match no token part; nor may a part that was left out match the part zero.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const query = [&](std::string_view parts) -> std::string const& {
        mc.resetReplyData();
        mc.writeToScreen(std::format("\033[>1;1;{}b", parts));
        mc.terminal.flushInput();
        return mc.replyData();
    };

    SECTION("a part 65536 above the token's")
    {
        mc.nextSemanticBlockToken = { 41394, 50132, 58870, 1816 };
        enableModeAndGetToken(mc);
        CHECK(query("106930;50132;58870;1816") == "\033P>3b\033\\");
        CHECK(query("41394;50132;58870;1816") == "\033P>0b\033\\"); // the token itself: no data yet
    }

    SECTION("any part past 65535, against a part of 65535")
    {
        mc.nextSemanticBlockToken = { 65535, 50132, 58870, 1816 };
        enableModeAndGetToken(mc);
        CHECK(query("70000;50132;58870;1816") == "\033P>3b\033\\");
        CHECK(query("65535;50132;58870;1816") == "\033P>3b\033\\");
    }

    SECTION("a part left out, against a part of zero")
    {
        mc.nextSemanticBlockToken = { 0, 50132, 58870, 1816 };
        enableModeAndGetToken(mc);
        CHECK(query(";50132;58870;1816") == "\033P>3b\033\\");
        CHECK(query("0;50132;58870;1816") == "\033P>0b\033\\");
    }
}

TEST_CASE("SemanticBlockProtocol.ATokenNeverHasAPartTheParserSaturatesTo", "[semanticblocks]")
{
    // A source of random bits that starts at its largest value and counts down, so the top of the range comes
    // first. Not one that only ever yields its largest value: libc++'s uniform_int_distribution draws again
    // after a value past the range, and would never finish.
    struct CountingDown
    {
        using result_type = uint32_t; // NOLINT(readability-identifier-naming)
        static constexpr result_type min() { return 0; }
        static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
        result_type operator()() { return next--; }
        result_type next = max();
    };

    auto bits = CountingDown {};
    for (auto const part: drawSemanticBlockToken(bits))
        CHECK(part <= MaxSemanticBlockTokenPart);
    CHECK(MaxSemanticBlockTokenPart < std::numeric_limits<uint16_t>::max());
}

TEST_CASE("SemanticBlockProtocol.TokenChangesOnReEnable")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable, get first token
    auto const token1 = enableModeAndGetToken(mc);

    // Disable, then re-enable
    mc.writeToScreen(DECRM(2034));
    auto const token2 = enableModeAndGetToken(mc);

    // Each session draws a token of its own.
    CHECK(token1 != token2);

    // Simulate a command
    simulateCommand(mc, "$ ", "ls", "output", 0);
    mc.writeToScreen("\033]133;A\033\\");

    // Old token should be rejected
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token1).contains("\033P>3b\033\\"));

    // New token should work
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token2).contains("\033P>1b"));
}

TEST_CASE("SemanticBlockProtocol.BlocksFromBeforeEnablingStayHidden", "[semanticblocks]")
{
    // The store records every block whether or not mode 2034 is on, but a reader must not be able to read
    // back commands that ran before it asked: enabling the mode starts a fresh reader session, as it did
    // when the tracker kept its own, initially empty, copy.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    simulateCommand(mc, "$ ", "secret", "out", 0);
    mc.writeToScreen("\033]133;A\033\\");

    auto const token = enableModeAndGetToken(mc);
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains("\033P>0b\033\\"));

    // ...while a command that starts afterwards is reported.
    simulateCommand(mc, "$ ", "visible", "out", 0);
    mc.writeToScreen("\033]133;A\033\\");
    auto const& reply = queryBlocks(mc, SBQueryType::LastNumberOfCommands, 5, token);
    CHECK(reply.contains("\"command\":\"visible\""));
    CHECK_FALSE(reply.contains("secret"));
}

TEST_CASE("SemanticBlockProtocol.RecoveredCommandLineIsReported", "[semanticblocks]")
{
    // tcsh has no clean hook for cmdline_url, so it marks ;B only; the store recovers what was typed from
    // the grid, and a reader of the protocol gets that rather than null.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);

    runCommand(mc, "ls -la", { "file1" });

    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains("\"command\":\"ls -la\""));
}

TEST_CASE("SemanticBlockProtocol.LastCommandsAreTheNewestOldestFirst", "[semanticblocks]")
{
    // The reply lists the newest blocks the count allows, oldest first, each command with its own output.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    simulateCommand(mc, "$ ", "cmd1", "out1", 0);
    simulateCommand(mc, "$ ", "cmd2", "out2", 0);
    simulateCommand(mc, "$ ", "cmd3", "out3", 0);
    mc.writeToScreen("\033]133;A\033\\");

    SECTION("the last N")
    {
        auto const& reply = queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token);
        CHECK_FALSE(reply.contains("cmd1"));
        // Each command comes before its own output, and the older pair before the newer.
        CHECK(appearInOrder(reply, { R"("command":"cmd2")", R"(out2")", R"("command":"cmd3")", R"(out3")" }));
    }

    SECTION("the last one, whatever the count says")
    {
        auto const& reply = queryBlocks(mc, SBQueryType::LastCommand, 5, token);
        CHECK(reply.contains(R"("command":"cmd3")"));
        CHECK(reply.contains(R"(out3")"));
        CHECK_FALSE(reply.contains("cmd2"));
    }
}

TEST_CASE("SemanticBlockProtocol.ImplicitlyClosedBlockIsNotReported", "[semanticblocks]")
{
    // `ssh` does not end before the remote shell's prompt starts a new block, so the store closes it
    // implicitly, with no exit code -- not a completed command a reader could be told about. Leaving it out
    // moves no other command's text.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    simulateCommand(mc, "$ ", "cmd1", "out1", 0);
    mc.writeToScreen("\033]133;A\033\\$ \033]133;C;cmdline_url=ssh%20host\033\\\r\n");
    simulateCommand(mc, "remote$ ", "ls", "file1", 0);
    mc.writeToScreen("\033]133;A\033\\");

    auto const& reply = queryBlocks(mc, SBQueryType::LastNumberOfCommands, 5, token);
    CHECK_FALSE(reply.contains("ssh"));
    CHECK(appearInOrder(reply, { R"("command":"cmd1")", R"(out1")", R"("command":"ls")", R"(file1")" }));
}

TEST_CASE("SemanticBlockProtocol.ReaderDoesNotSeeTheCommandItRunsIn", "[semanticblocks]")
{
    // A reader is usually a command itself, enabling the mode from inside a running block. That block began
    // before the session did, so it is neither in progress for the reader nor, once it ends, reported.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;C;cmdline_url=reader\033\\\r\n");

    auto const token = enableModeAndGetToken(mc);
    CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token).contains("\033P>0b\033\\"));

    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\");
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains("\033P>0b\033\\"));

    // The prompt that follows is the reader's to see, with nothing known yet about its command.
    auto const& reply = queryBlocks(mc, SBQueryType::InProgress, 1, token);
    CHECK(reply.contains(R"("command":null)"));
    CHECK(reply.contains(R"("exitCode":-1)"));
    CHECK(reply.contains(R"("finished":false)"));
}

TEST_CASE("SemanticBlockProtocol.ReaderEnabledAtThePromptSeesItsCommand", "[semanticblocks]")
{
    // A shell that enables the mode from its prompt hook does so after the prompt's ;A. The command typed at
    // that prompt starts after the session did, so it is the reader's to see -- at the prompt, and once run.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.writeToScreen("\033]133;A\033\\$ ");
    auto const token = enableModeAndGetToken(mc);

    CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token).contains(R"("finished":false)"));

    mc.writeToScreen("make\r\n\033]133;C;cmdline_url=make\033\\");
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains(R"("command":"make")"));
}

TEST_CASE("SemanticBlockProtocol.ReaderSeesTheCommandsRunAfterTheIdsStartOver", "[semanticblocks]")
{
    // A finished record a daemon host handed over, numbered just below the wrap, is the newest block when the
    // reader's session starts. The next prompt starts the ids over, so the command run there is numbered
    // below every id the session began with -- and must be reported all the same.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    runCommand(mc, "ls", { "a-file" });
    auto const* const finished = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(finished != nullptr);
    auto record = *finished;
    record.id = CommandBlockId { std::numeric_limits<CommandBlockId::inner_type>::max() - 1 };
    std::ignore = mc.terminal.commandBlocks().adopt(record, AdoptMode::Snapshot);
    auto const token = enableModeAndGetToken(mc);

    runCommand(mc, "make", { "built" });
    mc.writeToScreen("\033]133;A\033\\$ ");
    REQUIRE(mc.terminal.commandBlocks().currentId() == CommandBlockId { 2 }); // make's block was the 1
    CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains(R"("command":"make")"));
}

namespace
{
/// The reply to an SBQUERY that found blocks.
/// @param blocks The JSON of each block, comma-separated, oldest first.
[[nodiscard]] std::string blocksReply(std::string_view blocks)
{
    return std::format("\033P>1b{{\"version\":2,\"blocks\":[{}]}}\033\\", blocks);
}

/// @p text as OSC 133;C's cmdline_url carries it: every byte but an ASCII letter or digit percent-encoded,
/// the parameter separator among them.
[[nodiscard]] std::string urlEncoded(std::string_view text)
{
    auto encoded = std::string {};
    for (auto const ch: text)
    {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0)
            encoded += ch;
        else
            encoded += std::format("%{:02X}", static_cast<unsigned char>(ch));
    }
    return encoded;
}
} // namespace

TEST_CASE("SemanticBlockProtocol.EachBlockIsReportedWithTheTextOfItsOwnRows", "[semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);

    SECTION("an end mark no block was recorded for")
    {
        // tcsh emits ;D on an empty Enter too, with nothing running. The grid then holds an end mark the
        // store has no block for, and it must not hand one command's text to another.
        runCommand(mc, "echo one", { "one" });
        typeAtPrompt(mc, "");
        mc.writeToScreen("\033]133;D;0\033\\");
        runCommand(mc, "echo two", { "two" });
        mc.writeToScreen("\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token)
              == blocksReply(R"({"command":"echo one","prompt":"$ echo one","output":"one",)"
                             R"("exitCode":0,"finished":true,"outputLineCount":1},)"
                             R"({"command":"echo two","prompt":"$ echo two","output":"two",)"
                             R"("exitCode":0,"finished":true,"outputLineCount":1})"));
    }

    SECTION("a block still at its prompt")
    {
        // In progress, its command not yet started: a prompt, and no output -- not the command before it's.
        runCommand(mc, "ls", { "a-file" });
        mc.writeToScreen("\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token)
              == blocksReply(R"({"command":null,"prompt":"$","output":"","exitCode":-1,"finished":false,)"
                             R"("outputLineCount":0})"));
    }

    SECTION("a block whose command runs")
    {
        // Its output so far, down to the line the cursor stands on.
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\building\r\n");

        CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token)
              == blocksReply(R"({"command":"make","prompt":"$ make","output":"building\n","exitCode":-1,)"
                             R"("finished":false,"outputLineCount":2})"));
    }

    SECTION("queried from the alternate screen")
    {
        // The rows a block is stamped on are the primary screen's, whichever screen the query arrives on.
        runCommand(mc, "ls", { "a-file" });
        mc.writeToScreen("\033]133;A\033\\$ \033[?1049h");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(
                  R"({"command":"ls","prompt":"$ ls","output":"a-file","exitCode":0,"finished":true,)"
                  R"("outputLineCount":1})"));
    }

    SECTION("output the next prompt shares a line with")
    {
        // `printf hello`: its output start, its end and the next prompt all land on one line, which the next
        // prompt's block owns. The columns before the end mark are still this block's output, and the output
        // start there is its own too; the columns after it are the next block's prompt.
        writePromptAndRun(mc, "$ printf hello", "hello");
        writePrecmdAndPrompt(mc, "$ ");
        mc.writeToScreen("ls\r\n\033]133;C\033\\a-file\r\n");
        writePrecmdAndPrompt(mc, "$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token)
              == blocksReply(R"({"command":null,"prompt":"$ printf hello","output":"hello","exitCode":0,)"
                             R"("finished":true,"outputLineCount":1},)"
                             R"({"command":null,"prompt":"$ ls","output":"a-file","exitCode":0,)"
                             R"("finished":true,"outputLineCount":1})"));
    }

    SECTION("a block a command start began")
    {
        // No ;A before the ;C: the block's first line is where its output starts, and it has no prompt.
        mc.writeToScreen("\033]133;C\033\\out\r\n\033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(R"({"command":null,"prompt":"","output":"out","exitCode":0,"finished":true,)"
                             R"("outputLineCount":1})"));
    }
}

TEST_CASE("SemanticBlockProtocol.TheBlockActionsReadTheTextTheReplyReports", "[semanticblocks]")
{
    // A block action copies or selects a part of a block, and the reply reports the same parts. Where an
    // output tail shares its row with the next prompt, both must split the row at the same column, or what
    // the user copies would disagree with what a reader of the reply is told.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    writePromptAndRun(mc, "$ printf hello", "hello");
    writePrecmdAndPrompt(mc, "$ ");
    mc.writeToScreen("ls\r\n\033]133;C\033\\a-file\r\n");
    writePrecmdAndPrompt(mc, "$ ");

    auto const reported = [&mc](CommandBlockRecord const& record) {
        return std::format(R"({{"command":null,"prompt":"{}","output":"{}","exitCode":0,"finished":true,)"
                           R"("outputLineCount":1}})",
                           textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt, record.id),
                           textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output, record.id));
    };

    auto finished = std::vector<CommandBlockRecord const*> {};
    for (auto const& record: mc.terminal.commandBlocks().records())
        if (record.state == CommandBlockState::Finished)
            finished.push_back(&record);
    REQUIRE(finished.size() == 2);

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output, finished[0]->id) == "hello");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt, finished[1]->id) == "$ ls");
    CHECK(queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token)
          == blocksReply(std::format("{},{}", reported(*finished[0]), reported(*finished[1]))));
}

TEST_CASE("SemanticBlockProtocol.TheBlockActionsReadThePartsTheReplyReportsAtTheEdges", "[semanticblocks]")
{
    // The reply and the actions read one split of a block into prompt and output, so where a block has lost
    // its head, has run no command, or ends on blanks before its end mark, both still report the same parts.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
    auto const token = enableModeAndGetToken(mc);

    SECTION("the head of a two-line prompt left the history")
    {
        mc.writeToScreen(
            "\033]133;A\033\\~/src\r\n$ \033]133;B\033\\make\r\n\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n");
        mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(R"({"command":"make","prompt":"$ make","output":"1\n2\n3\n4","exitCode":0,)"
                             R"("finished":true,"outputLineCount":4})"));
        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ make");
        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == "1\n2\n3\n4");
    }

    SECTION("a block at its prompt whose head left the history")
    {
        mc.writeToScreen("\033]133;A\033\\p1\r\np2\r\np3\r\np4\r\np5\r\np6\r\n$ ");

        CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token)
              == blocksReply(R"({"command":null,"prompt":"p2\np3\np4\np5\np6\n$","output":"","exitCode":-1,)"
                             R"("finished":false,"outputLineCount":0})"));
        CHECK(textOf(mc, CommandBlockTarget::Cursor, CommandBlockPart::Prompt) == "p2\np3\np4\np5\np6\n$");
        CHECK(textOf(mc, CommandBlockTarget::Cursor, CommandBlockPart::Output) == "<error>");
    }

    SECTION("an output of blanks before the end mark")
    {
        // `printf '   '`: its only line ends at the end mark, with nothing but blanks before it.
        typeAtPrompt(mc, "printf");
        mc.writeToScreen("\033]133;C\033\\   \033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(R"({"command":"printf","prompt":"$ printf","output":"","exitCode":0,)"
                             R"("finished":true,"outputLineCount":0})"));
        CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ printf");
        CHECK(mc.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
              == std::unexpected(CommandBlockActionError::EmptyPart));
    }
}

TEST_CASE("SemanticBlockProtocol.ABlockReportsWhatTheHistoryStillHoldsOfIt", "[semanticblocks]")
{
    // Four lines of page and two of history: a command's prompt and first output line scroll out of it.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
    auto const token = enableModeAndGetToken(mc);
    runCommand(mc, "seq 6", { "1", "2", "3", "4", "5", "6" });

    SECTION("its prompt scrolled out")
    {
        mc.writeToScreen("\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(R"({"command":"seq 6","prompt":"","output":"2\n3\n4\n5\n6","exitCode":0,)"
                             R"("finished":true,"outputLineCount":5})"));
    }

    SECTION("all of it scrolled out")
    {
        // Still a command that ran and ended, with no text left to it.
        runCommand(mc, "yes | head -6", { "y", "y", "y", "y", "y", "y" });
        mc.writeToScreen("\033]133;A\033\\$ ");

        CHECK(
            queryBlocks(mc, SBQueryType::LastNumberOfCommands, 2, token)
            == blocksReply(R"({"command":"seq 6","prompt":"","output":"","exitCode":0,"finished":true,)"
                           R"("outputLineCount":0},)"
                           R"({"command":"yes | head -6","prompt":"","output":"y\ny\ny\ny\ny","exitCode":0,)"
                           R"("finished":true,"outputLineCount":5})"));
    }
}

TEST_CASE("SemanticBlockProtocol.ABlockReportsWhatAnEraseAboveTheCursorLeftOfIt", "[semanticblocks]")
{
    // `seq 1 5; printf '\033[1J'; seq 6 8` as one command: the ED 1 blanks the page above the cursor, the
    // prompt and the first five lines with it, and a row blanked away from the cursor carries no block. What
    // the block still holds is the three lines written after it, as its rows stand and once a reflow has
    // re-headed it. The command before it heads the history, so a block read from there would read nothing.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(20) };
    auto const token = enableModeAndGetToken(mc);
    runCommand(mc, "ls", { "a" });
    typeAtPrompt(mc, "seq");
    mc.writeToScreen("\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n5\r\n\033[1J6\r\n7\r\n8\r\n");
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineTextTrimmed(grid.addressableTop()) == "$ ls");
    auto const reply = blocksReply(R"({"command":"seq","prompt":"","output":"6\n7\n8","exitCode":0,)"
                                   R"("finished":true,"outputLineCount":3})");

    SECTION("as its rows stand")
    {
        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token) == reply);
    }

    SECTION("reflowed")
    {
        auto const generation = grid.stableIdGeneration();
        mc.terminal.resizeScreen(PageSize { LineCount(8), ColumnCount(15) });
        REQUIRE(grid.stableIdGeneration() != generation);

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token) == reply);
    }
}

TEST_CASE("SemanticBlockProtocol.RowsWithNoBlockBetweenABlocksRowsAreItsBlankLines", "[semanticblocks]")
{
    // A command prints four lines, goes back up to the second and erases from there down (ED 0), then moves
    // down past the erased rows and prints two more. A row blanked away from the cursor carries no block,
    // which says nothing about whose it is: between two rows of the block it is the block's blank line.
    // Below the block's last row -- below the cursor, while it runs -- it is not.
    auto mc = MockTerm { PageSize { LineCount(12), ColumnCount(20) }, LineCount(10) };
    auto const token = enableModeAndGetToken(mc);
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n\033[3;1H\033[J\033[8;1H5\r\n6\r\n");
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineAt(LineOffset(4)).blockId() == CommandBlockId {});

    SECTION("running")
    {
        CHECK(queryBlocks(mc, SBQueryType::InProgress, 1, token)
              == blocksReply(R"({"command":"make","prompt":"$ make","output":"1\n\n\n\n\n\n5\n6\n",)"
                             R"("exitCode":-1,"finished":false,"outputLineCount":9})"));
    }

    SECTION("finished")
    {
        mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
              == blocksReply(R"({"command":"make","prompt":"$ make","output":"1\n\n\n\n\n\n5\n6",)"
                             R"("exitCode":0,"finished":true,"outputLineCount":8})"));
    }
}

TEST_CASE("SemanticBlockProtocol.ABlockWhoseHeadScrolledOutIsReadPastRowsWithNoBlock", "[semanticblocks]")
{
    // The prompt scrolls out of a four-line history; the command erases the page above the cursor, and its
    // later output scrolls the erased rows up to the top of the history. The head is evicted, so the block is
    // read from the top of the history down: past the rows there that carry no block, which say nothing of
    // whose they were, to the rows that carry its id.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    auto const token = enableModeAndGetToken(mc);
    typeAtPrompt(mc, "seq");
    mc.writeToScreen("\033]133;C\033\\");
    for (auto const i: std::views::iota(1, 8))
        mc.writeToScreen(std::format("{}\r\n", i));
    mc.writeToScreen("\033[1J");
    for (auto const i: std::views::iota(8, 12))
        mc.writeToScreen(std::format("{}\r\n", i));
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineAt(grid.addressableTop()).blockId() == CommandBlockId {});

    CHECK(
        queryBlocks(mc, SBQueryType::LastCommand, 1, token)
        == blocksReply(R"({"command":"seq","prompt":"","output":"8\n9\n10\n11","exitCode":0,"finished":true,)"
                       R"("outputLineCount":4})"));
}

TEST_CASE("SemanticBlockProtocol.TheDocumentedExampleSession", "[semanticblocks]")
{
    // The "Example Session" of docs/vt-extensions/semantic-block-query.md, step by step, at a shell with OSC
    // 133 integration that reports each command line: every reply that page shows is the one sent here, and
    // the JSON it lays out is step 4's.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.nextSemanticBlockToken = { 41394, 50132, 58870, 1816 };

    // Runs @p command at a prompt, the command writing @p writes, and returns what the terminal replied.
    auto const run = [&mc](std::string_view command, std::string_view writes) {
        mc.writeToScreen(std::format("\033]133;A\033\\user@host:~$ \033]133;B\033\\{}\r\n", command));
        mc.writeToScreen(std::format("\033]133;C;cmdline_url={}\033\\", urlEncoded(command)));
        mc.resetReplyData();
        mc.writeToScreen(writes);
        mc.terminal.flushInput();
        // The keys version 2 appends are left out, as the page's step 4 leaves them out.
        auto reply = withoutVersion2Keys(mc.replyData());
        mc.writeToScreen("\033]133;D;0\033\\");
        return reply;
    };

    // 1. and 2.
    CHECK(run(R"(printf '\033[?2034h')", "\033[?2034h") == "\033P>2034;1b41394;50132;58870;1816\033\\");
    CHECK(run(R"(printf '\033[?2034$p')", "\033[?2034$p") == "\033[?2034;1$y");

    // 3.
    run("ls", "notes.txt  todo.txt\r\n");
    run("echo hello", "hello\r\n");

    // 4.
    CHECK(run(R"(printf '\033[>1;1;41394;50132;58870;1816b')", "\033[>1;1;41394;50132;58870;1816b")
          == blocksReply(R"({"command":"echo hello","prompt":"user@host:~$ echo hello","output":"hello",)"
                         R"("exitCode":0,"finished":true,"outputLineCount":1})"));

    // 5. Each printf is a command, and so a block, of its own: step 4's is among the last three.
    CHECK(run(R"(printf '\033[>2;3;41394;50132;58870;1816b')", "\033[>2;3;41394;50132;58870;1816b")
          == blocksReply(R"({"command":"ls","prompt":"user@host:~$ ls","output":"notes.txt  todo.txt",)"
                         R"("exitCode":0,"finished":true,"outputLineCount":1},)"
                         R"({"command":"echo hello","prompt":"user@host:~$ echo hello","output":"hello",)"
                         R"("exitCode":0,"finished":true,"outputLineCount":1},)"
                         R"({"command":"printf '\\033[>1;1;41394;50132;58870;1816b'",)"
                         R"("prompt":"user@host:~$ printf '\\033[>1;1;41394;50132;58870;1816b'","output":"",)"
                         R"("exitCode":0,"finished":true,"outputLineCount":0})"));

    // 6. The command in progress is the printf asking: its output so far is the line the cursor is on.
    CHECK(run(R"(printf '\033[>3;1;41394;50132;58870;1816b')", "\033[>3;1;41394;50132;58870;1816b")
          == blocksReply(R"({"command":"printf '\\033[>3;1;41394;50132;58870;1816b'",)"
                         R"("prompt":"user@host:~$ printf '\\033[>3;1;41394;50132;58870;1816b'","output":"",)"
                         R"("exitCode":-1,"finished":false,"outputLineCount":1})"));

    // 7. Step 3's `echo hello` by its id, which step 4's reply carried.
    CHECK(run(R"(printf '\033[>4;4;41394;50132;58870;1816;0b')", "\033[>4;4;41394;50132;58870;1816;0b")
          == blocksReply(R"({"command":"echo hello","prompt":"user@host:~$ echo hello","output":"hello",)"
                         R"("exitCode":0,"finished":true,"outputLineCount":1})"));

    // 8.
    CHECK(run(R"(printf '\033[?2034l')", "\033[?2034l").empty());
    CHECK_FALSE(mc.terminal.semanticBlockTracker().isEnabled());
}

TEST_CASE("SemanticBlockProtocol.ReaderWithoutSessionSeesNoBlocks", "[semanticblocks]")
{
    // The query handler refuses before it ever asks, but the reader's answer must not rest on that: without
    // a session it sees nothing, whatever the store holds.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    simulateCommand(mc, "$ ", "ls", "file1", 0);
    mc.writeToScreen("\033]133;A\033\\$ \033]133;C;cmdline_url=sleep\033\\");

    auto const& tracker = mc.terminal.semanticBlockTracker();
    auto const& store = mc.terminal.commandBlocks();
    REQUIRE(store.lastFinished() != nullptr);
    REQUIRE(store.current() != nullptr);
    CHECK(tracker.finishedBlocks(store, 5).empty());
    CHECK(tracker.inProgressBlock(store) == nullptr);
}

TEST_CASE("SemanticBlockProtocol.textTakenFromTheGridLeavesOutWrapPadding", "[semanticblocks]")
{
    // A narrowing moves a wide character the new edge would cut in two on to the next row whole, leaving
    // padding where it stood, as foot leaves its spacer. Padding is not text: however the output is taken
    // from the grid, it reads as it was printed, as foot's extraction skips the spacer (extract.c:161).
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(10) }, LineCount(10) };
    auto const token = enableModeAndGetToken(mc);
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\cat\r\n\033]133;C\033\\ABCD\xE4\xB8\xAD"
                     "E\r\n\033]133;D;0\033\\\033]133;A\033\\$ ");
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(5) });

    // "$ cat" "ABCD" "中E" "$ ", the padding at the end of "ABCD".
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const& padded = grid.lineAt(LineOffset(1));
    REQUIRE(padded.storage().sgr[4].flags.contains(CellFlag::WrapPadding));

    SECTION("read off its row")
    {
        CHECK(padded.toUtf8() == "ABCD");
        CHECK(padded.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(5)) == "ABCD");
        // Still a codepoint per column for a reader that maps one back to its column, as a wide
        // character's second half is.
        CHECK(padded.toUtf8ColumnAligned() == "ABCD ");
    }

    SECTION("copied")
    {
        mc.terminal.setSelector(std::make_unique<LinearSelection>(
            mc.terminal.selectionHelper(),
            CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) },
            []() {}));
        (void) mc.terminal.selector()->extend(
            CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) });
        mc.terminal.selector()->complete();
        CHECK(mc.terminal.extractSelectionText()
              == "ABCD\xE4\xB8\xAD"
                 "E");
    }

    SECTION("taken as a word")
    {
        CHECK(grid.extractText(CellLocationRange {
                  .first = CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) },
                  .second = CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) } })
              == U"ABCD");
        // Nor is a wide character's second half text.
        CHECK(grid.extractText(CellLocationRange {
                  .first = CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) },
                  .second = CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) } })
              == U"\u4E2DE");
    }

    SECTION("reported to SBQUERY")
    {
        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token)
                  .contains("\"output\":\"ABCD\xE4\xB8\xAD"
                            "E\""));
    }
}

TEST_CASE("SemanticBlockProtocol.textTakenFromTheGridLeavesOutATallBlocksLowerPart", "[semanticblocks]")
{
    // An OSC 66 block scaled to two rows claims the cells beneath its head, and they hold no text of their
    // own: the text is the head's. However the output is taken from the grid, the row below reads as what
    // was written there, as kitty's text extraction skips a multicell character's lower rows (line.c:493).
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(10) }, LineCount(10) };
    auto const token = enableModeAndGetToken(mc);
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\cat\r\n\033]133;C\033\\\033]66;s=2;X\a\r\n\033[2Cyz"
                     "\r\n\033]133;D;0\033\\\033]133;A\033\\$ ");

    // "$ cat", the block's head "X", then its lower part and "yz".
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const& lower = grid.lineAt(LineOffset(2));
    REQUIRE(lower.storage().sgr[0].flags.contains(CellFlag::MulticellContinuation));

    SECTION("read off its row")
    {
        CHECK(lower.toUtf8(ColumnOffset(0), ColumnOffset(4)) == "yz");
        CHECK(lower.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(4)) == "yz");
        // Still a codepoint per column for a reader that maps one back to its column.
        CHECK(lower.toUtf8ColumnAligned().starts_with("  yz"));
    }

    SECTION("copied")
    {
        mc.terminal.setSelector(std::make_unique<LinearSelection>(
            mc.terminal.selectionHelper(),
            CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) },
            []() {}));
        (void) mc.terminal.selector()->extend(
            CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) });
        mc.terminal.selector()->complete();
        CHECK(mc.terminal.extractSelectionText() == "X\nyz");
    }

    SECTION("reported to SBQUERY")
    {
        CHECK(queryBlocks(mc, SBQueryType::LastCommand, 1, token).contains(R"("output":"X\nyz")"));
    }
}

TEST_CASE("SemanticBlockProtocol.aRecoveredCommandLineLeavesOutWrapPadding", "[semanticblocks]")
{
    // A wide character typed where it does not fit what is left of its row leaves padding there too, and
    // the command line recovered at ;C reads as it was typed.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(5) }, LineCount(10) };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\AB\xE4\xB8\xAD");
    REQUIRE(mc.terminal.primaryScreen()
                .grid()
                .lineAt(LineOffset(0))
                .storage()
                .sgr[4]
                .flags.contains(CellFlag::WrapPadding));

    mc.writeToScreen("\r\n\033]133;C\033\\");
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "AB\xE4\xB8\xAD");
}

// {{{ OSC 133;B — the prompt/input border

TEST_CASE("ShellIntegration.OSC_133_B stamps PromptEnd on the logical head")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");

    auto const& head = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(head.isFlagEnabled(LineFlag::PromptEnd));
    // "$ " is two columns, so the user's input begins at logical column 2.
    CHECK(head.promptEndOffset() == ColumnOffset(2));
}

TEST_CASE("ShellIntegration.OSC_133_B on a multi-line prompt marks the line it ended on")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("first\r\n");
    mc.writeToScreen("> ");
    mc.writeToScreen("\033]133;B\033\\");

    auto const& grid = mc.terminal.primaryScreen().grid();
    // ;A marked the line the prompt STARTED on...
    CHECK(grid.lineAt(LineOffset(0)).isFlagEnabled(LineFlag::Marked));
    CHECK_FALSE(grid.lineAt(LineOffset(0)).isFlagEnabled(LineFlag::PromptEnd));
    // ...and ;B the line it ENDED on, which is a different logical line.
    CHECK(grid.lineAt(LineOffset(1)).isFlagEnabled(LineFlag::PromptEnd));
    CHECK(grid.lineAt(LineOffset(1)).promptEndOffset() == ColumnOffset(2));
}

TEST_CASE("ShellIntegration.PromptEnd survives reflow")
{
    // The offset is a LOGICAL column, so re-chopping the line into different physical pieces, none of them
    // padded, must not move it. Without the re-application in Grid::addNewWrappedLines the flag would survive
    // on the head with a ZEROED offset, which reads as "the prompt ended at column 0" -- a silent, plausible
    // lie. The prompt is one the shell does not redraw (redraw=0), or the resize would leave it unreflowed.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A;redraw=0\033\\");
    mc.writeToScreen("0123456789abcdef> "); // 18 columns of prompt, fits in 20
    mc.writeToScreen("\033]133;B\033\\");

    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(0)).promptEndOffset() == ColumnOffset(18));

    SECTION("narrowing splits the logical line")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(10) });

        auto const& grid = mc.terminal.primaryScreen().grid();
        // Splitting the line pushed the head up: reached through logicalLineHead rather than a fixed
        // offset, exactly as production code finds it.
        auto const headOffset = grid.logicalLineHead(mc.terminal.currentScreen().cursor().position.line);
        auto const& head = grid.lineAt(headOffset);

        CHECK(head.isFlagEnabled(LineFlag::PromptEnd));
        CHECK(head.promptEndOffset() == ColumnOffset(18));

        // The continuation must NOT claim to be a second prompt end.
        CHECK_FALSE(grid.lineAt(headOffset + 1).isFlagEnabled(LineFlag::PromptEnd));
        CHECK(grid.lineAt(headOffset + 1).isFlagEnabled(LineFlag::Wrapped));
    }

    SECTION("widening rejoins it")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(10) });
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(40) });

        auto const& grid = mc.terminal.primaryScreen().grid();
        auto const headOffset = grid.logicalLineHead(mc.terminal.currentScreen().cursor().position.line);
        auto const& head = grid.lineAt(headOffset);

        CHECK(head.isFlagEnabled(LineFlag::PromptEnd));
        CHECK(head.promptEndOffset() == ColumnOffset(18));
    }
}

TEST_CASE("ShellIntegration.LineFlags formatter names PromptEnd")
{
    // The formatter is generated from VTBACKEND_LINE_FLAGS, so this also pins that a flag added to that
    // table cannot be left out of its name list -- the hand-copied array this replaced could.
    CHECK(std::format("{}", LineFlags { LineFlag::PromptEnd }) == "PromptEnd");
    CHECK(std::format("{}", LineFlags { LineFlag::Marked, LineFlag::PromptEnd }) == "Marked,PromptEnd");
}

// }}}

// {{{ livePromptSpan — the grid-coordinate view of the live prompt

TEST_CASE("ShellIntegration.livePromptSpan reports a fresh prompt")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");

    auto const span = mc.terminal.livePromptSpan();

    REQUIRE(span.has_value());
    CHECK(span->firstLine == LineOffset(0));
    CHECK(span->lastLine == LineOffset(0));
    REQUIRE(span->inputBegin.has_value());
    CHECK(*span->inputBegin == ColumnOffset(2));
}

TEST_CASE("ShellIntegration.livePromptSpan spans a reflowed multi-line prompt")
{
    // A prompt the shell does not redraw (redraw=0), which a resize therefore reflows.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A;redraw=0\033\\");
    mc.writeToScreen("line one of prompt\r\n");
    mc.writeToScreen("> ");
    mc.writeToScreen("\033]133;B\033\\");

    auto const before = mc.terminal.livePromptSpan();
    REQUIRE(before.has_value());
    CHECK(before->firstLine == LineOffset(0));
    CHECK(before->lastLine == LineOffset(1));

    // Narrowing splits the first prompt line in two; the span must still cover the whole prompt, which is
    // now one physical line taller.
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(10) });

    auto const after = mc.terminal.livePromptSpan();
    REQUIRE(after.has_value());
    CHECK(*after->inputBegin == ColumnOffset(2));
    CHECK(after->lastLine - after->firstLine == LineOffset(2));
}

TEST_CASE("ShellIntegration.livePromptSpan declines while a command is running")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    typeAtPrompt(mc, "ls");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("a-file\r\n");

    auto const span = mc.terminal.livePromptSpan();

    REQUIRE_FALSE(span.has_value());
    CHECK(span.error() == PromptRegionError::InCommandOutput);
}

TEST_CASE("ShellIntegration.livePromptSpan reports a prompt again once the command finished")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    runCommand(mc, "ls", { "a-file" });
    // ... and the shell paints its next prompt.
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");

    auto const span = mc.terminal.livePromptSpan();

    REQUIRE(span.has_value());
    CHECK(span->firstLine == LineOffset(2));
    REQUIRE(span->inputBegin.has_value());
    CHECK(*span->inputBegin == ColumnOffset(2));
}

TEST_CASE("ShellIntegration.livePromptSpan finds the prompt after a command that printed nothing")
{
    // The bundled integrations' order, which marks no ;B: `cd` prints nothing, so its ;C, its ;D and the next
    // prompt's ;A all land on one line. The prompt there is live; the output start beside it is `cd`'s.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ cd /tmp\r\n\033]133;C\033\\");
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    auto const span = mc.terminal.livePromptSpan();

    REQUIRE(span.has_value());
    CHECK(span->firstLine == LineOffset(1));
    CHECK(span->lastLine == LineOffset(1));
    CHECK_FALSE(span->inputBegin.has_value());
}

TEST_CASE("ShellIntegration.livePromptSpan reports no integration for a plain shell")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.writeToScreen("$ ");

    auto const span = mc.terminal.livePromptSpan();

    REQUIRE_FALSE(span.has_value());
    CHECK(span.error() == PromptRegionError::NoPromptMark);
}

TEST_CASE("ShellIntegration.livePromptSpan is silent on the alternate screen")
{
    // An alt-screen application owns the page. The primary screen's marks are still there below, but they
    // say nothing about where the caret is now.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");
    REQUIRE(mc.terminal.livePromptSpan().has_value());

    mc.writeToScreen("\033[?1049h"); // enter alt screen

    auto const span = mc.terminal.livePromptSpan();
    REQUIRE_FALSE(span.has_value());
    CHECK(span.error() == PromptRegionError::NoPromptMark);
}

// }}}

TEST_CASE("ShellIntegration.OSC 133;D invalidates the fold ranges it changes")
{
    // A regression guard for a real defect: the ;B and ;D handlers used to stamp their flag directly on
    // the line head, bypassing Screen::setLogicalLineFlags() and therefore never calling
    // Terminal::invalidateFoldRanges(). Both flags are in ShellMarkLineFlags, and Terminal::foldRanges()
    // caches on {generation, stableBase, markRevision} -- so a ;D landing on a page that has not
    // scrolled left the cached ranges reporting the state from before the command finished.
    //
    // It was masked only by the ;A of the NEXT prompt invalidating one sequence later, i.e. by an
    // accident of shell-script ordering rather than by design. This case closes the window: it reads
    // the ranges between the ;D and that ;A, which is exactly where the stale answer lived.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };

    typeAtPrompt(mock, "ls");
    mock.writeToScreen("\033]133;C\033\\");
    mock.writeToScreen("one\r\ntwo\r\n");

    // Warm the cache while the command is still open: no block has finished, so there is no range yet.
    REQUIRE(mock.terminal.foldRanges().empty());

    mock.writeToScreen("\033]133;D;0\033\\");

    // Without the invalidation this still reports the warmed, empty answer.
    CHECK(!mock.terminal.foldRanges().empty());
}

// {{{ SBQUERY 4 and JSON version 2

namespace
{
/// An SBQUERY 4 for the block @p id, Pn and Ph split as a reader splits it.
std::string queryBlockById(SemanticBlockTracker::Token const& token, uint32_t id)
{
    return SBQUERY(SBQueryType::BlockById, id & 0xFFFF, token[0], token[1], token[2], token[3], id >> 16);
}

/// A CSI sequence carrying @p values as its parameters.
vtbackend::Sequence sequenceWith(std::initializer_list<uint16_t> values)
{
    auto seq = vtbackend::Sequence {};
    auto builder = vtbackend::SequenceParameterBuilder { seq.parameters() };
    auto first = true;
    for (auto const value: values)
    {
        if (!std::exchange(first, false))
            builder.nextParameter();
        builder.set(value);
    }
    builder.fixiate();
    return seq;
}
} // namespace

TEST_CASE("SBQUERY 4 answers a block by id with cwd and timing", "[vtbackend][sbquery]")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.writeToScreen("\033]7;file://h/tmp\033\\");
    auto const token = enableModeAndGetToken(mc);

    // The first command: started a second into the session, running for 1500 ms.
    mc.wallClock.advance(std::chrono::seconds { 1 });
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\built\r\n");
    mc.steadyClock.advance(std::chrono::milliseconds { 1500 });
    mc.writeToScreen("\033]133;D;0\033\\");
    auto const first = unbox(mc.terminal.commandBlocks().lastFinished()->id);
    runCommand(mc, "ls", { "a-file" });
    mc.writeToScreen("\033]133;A\033\\$ ");

    CHECK(queryBlocksWhole(mc, queryBlockById(token, first))
          == std::format("\033P>1b{{\"version\":2,\"blocks\":[{{\"command\":\"make\",\"prompt\":\"$ make\","
                         "\"output\":\"built\",\"exitCode\":0,\"finished\":true,\"outputLineCount\":1,"
                         "\"id\":{},\"cwd\":\"/tmp\",\"startedAt\":\"1970-01-01T00:00:01.000Z\","
                         "\"durationMs\":1500}}]}}\033\\",
                         first));

    // A block in any state, the one at its prompt included: no command has started in it, so it has no
    // directory (taken as its command starts), no start and no duration yet.
    auto const current = unbox(mc.terminal.commandBlocks().currentId());
    auto const atPrompt = queryBlocksWhole(mc, queryBlockById(token, current));
    CHECK(atPrompt.contains(
        std::format(R"("id":{},"cwd":null,"startedAt":null,"durationMs":null}})", current)));
    CHECK(atPrompt.contains(R"("finished":false)"));
}

TEST_CASE("SBQUERY 4 refuses blocks the reader may not see or that do not exist", "[vtbackend][sbquery]")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    runCommand(mc, "before", { "secret" });
    auto const before = unbox(mc.terminal.commandBlocks().lastFinished()->id);
    mc.writeToScreen("\033]133;A\033\\$ ");
    auto const token = enableModeAndGetToken(mc);
    runCommand(mc, "after", { "out" });

    auto constexpr NoData = std::string_view { "\033P>0b\033\\" };
    // Run before the reader's session began: hidden, as the other queries hide it.
    CHECK(queryBlocksWhole(mc, queryBlockById(token, before)) == NoData);
    // No such block.
    CHECK(queryBlocksWhole(mc, queryBlockById(token, 4242)) == NoData);
    // Zero is no block's id, and is what an absent Pn reads as.
    CHECK(queryBlocksWhole(mc, queryBlockById(token, 0)) == NoData);
    // The token is still required.
    CHECK(queryBlocksWhole(mc, SBQUERY(SBQueryType::BlockById, before)) == "\033P>2b\033\\");
}

TEST_CASE("sbQueryBlockId joins the high part", "[vtbackend][sbquery]")
{
    CHECK(vtbackend::sbQueryBlockId(sequenceWith({ 4, 1, 9, 9, 9, 9, 1 })) == CommandBlockId { 65537 });
    CHECK(vtbackend::sbQueryBlockId(sequenceWith({ 4, 1, 9, 9, 9, 9 })) == CommandBlockId { 1 });
    CHECK(vtbackend::sbQueryBlockId(sequenceWith({ 4, 0xFFFF, 9, 9, 9, 9, 2 }))
          == CommandBlockId { 0x2FFFF });
    CHECK(vtbackend::sbQueryBlockId(sequenceWith({ 4 })) == CommandBlockId { 0 });
}

TEST_CASE("SBQUERY replies are version 2", "[vtbackend][sbquery]")
{
    // A reader of version 1 finds every key it knows, in the place and the form it knew it; the keys version
    // 2 adds come after them.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    auto const token = enableModeAndGetToken(mc);
    runCommand(mc, "ls", { "a-file" });
    mc.writeToScreen("\033]133;A\033\\$ ");

    auto const reply = queryBlocksWhole(mc, authenticatedSBQuery(SBQueryType::LastCommand, 1, token));
    auto const id = unbox(mc.terminal.commandBlocks().lastFinished()->id);
    CHECK(reply
          == std::format(
              "\033P>1b{{\"version\":2,\"blocks\":[{{\"command\":\"ls\",\"prompt\":\"$ ls\","
              "\"output\":\"a-file\",\"exitCode\":0,\"finished\":true,\"outputLineCount\":1,"
              "\"id\":{},\"cwd\":null,\"startedAt\":\"1970-01-01T00:00:00.000Z\",\"durationMs\":0}}]}}"
              "\033\\",
              id));
    CHECK(withoutVersion2Keys(reply)
          == blocksReply(R"({"command":"ls","prompt":"$ ls","output":"a-file","exitCode":0,"finished":true,)"
                         R"("outputLineCount":1})"));
}

// }}}

TEST_CASE("ShellIntegration.a row wrapped onto sheds the marks of the line it headed", "[vtbackend][resize]")
{
    // A command's output stops part-way into row 2, so its head carries ;D's column. A full-screen
    // program then redraws from row 1 with a line long enough to wrap onto row 2 -- without scrolling,
    // as an agent's UI does -- and row 2 becomes a continuation of the line row 1 heads.
    auto mc =
        MockTerm { PageSize { LineCount(5), ColumnCount(10) }, HistoryLimits::plain(Infinite {}), 1024 };
    mc.writeToScreen("\033[3;1Habc\033]133;D\033\\");
    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(2)).commandEndOffset() == ColumnOffset(3));
    mc.writeToScreen("\033[2;1H0123456789abcde");

    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(2));
    CHECK(row.wrapped());
    CHECK_FALSE(row.isFlagEnabled(HeadOnlyLineFlags));
    CHECK(row.commandEndOffset() == ColumnOffset(0));

    // Reflow rebuilds a continuation from its parts and must not find a head's border on it.
    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(6) });
    CHECK(mc.terminal.primaryScreen().renderMainPageText().contains("6789ab"));
}

TEST_CASE("ShellIntegration.a block whose head row is wrapped onto is headed afresh",
          "[vtbackend][semanticblocks]")
{
    // A command's prompt on row 2 and its output on row 3. A redraw from row 1 wraps onto row 2, which
    // stops heading anything: the block is headed afresh by the logical line row 2 is now a part of.
    auto mc =
        MockTerm { PageSize { LineCount(6), ColumnCount(10) }, HistoryLimits::plain(Infinite {}), 1024 };
    mc.writeToScreen("\033[3;1H\033]133;A\033\\$ ls\033]133;C\033\\\r\nout");
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    auto const id = record->id;
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineOffsetOf(record->headStableId) == LineOffset(2));

    mc.writeToScreen("\033[2;1H0123456789abcde");

    auto const* after = mc.terminal.commandBlocks().find(id);
    REQUIRE(after != nullptr);
    CHECK(grid.lineOffsetOf(after->headStableId) == LineOffset(1));
}

TEST_CASE("ShellIntegration.a wrap that cannot leave its row keeps the row's own marks",
          "[vtbackend][resize]")
{
    // The cursor sits below a scrolling region, where a line feed goes nowhere: the prompt wraps over its
    // own row, which stays the head of its own line, marks and all.
    auto mc =
        MockTerm { PageSize { LineCount(5), ColumnCount(10) }, HistoryLimits::plain(Infinite {}), 1024 };
    mc.writeToScreen("\033[1;3r\033[5;1H\033]133;A\033\\$ 0123456789abc");

    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(4));
    CHECK(row.isFlagEnabled(LineFlag::Marked));
    CHECK_FALSE(row.wrapped());
}

TEST_CASE("ShellIntegration.a row standing in for a head gone from the scrollback becomes the head",
          "[vtbackend][resize]")
{
    // With no scrollback, a line longer than the page leaves its continuation at the top: the head it
    // continues is gone. A ;D then marks that row, and the row becomes the head -- for a continuation may
    // carry no head's marks, and reflow refuses one that does.
    auto const history = GENERATE(LineCount(0), LineCount(3));
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(10) }, HistoryLimits::plain(history), 1024 };
    mc.writeToScreen(std::string(95, 'a') + "\033]133;D\033\\");

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const top = -boxed_cast<LineOffset>(grid.historyLineCount());
    auto const& row = grid.lineAt(top);
    CHECK_FALSE(row.wrapped());
    CHECK(row.isFlagEnabled(LineFlag::CommandEnd));

    // Scrolled down into the page and written over, then narrowed: the marked row reflows as a head, and
    // no continuation anywhere comes out of it carrying a head's marks.
    mc.writeToScreen("\033[T\033[1;1H0123456789");
    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(6) });
    auto const& narrowed = mc.terminal.primaryScreen().grid();
    auto const rows = std::views::iota(-unbox<int>(narrowed.historyLineCount()), 5);
    CHECK(std::ranges::none_of(rows, [&](int line) {
        auto const& row = narrowed.lineAt(LineOffset(line));
        return row.wrapped() && row.isFlagEnabled(HeadOnlyLineFlags);
    }));
}

TEST_CASE("ShellIntegration.a wrap within left and right margins keeps the row's own marks",
          "[vtbackend][resize]")
{
    // Left and right margins narrower than the page confine a wrap to the band of the row below, whose
    // cells around the band are still the prompt's: that row heads its own line, marks and all -- whether
    // the band scrolled or the cursor moved down, and whichever margin is moved in.
    struct Case
    {
        std::string_view margins;
        std::string_view write;
    };
    auto const [margins, write] = GENERATE(Case { "\033[3;8s", "\033[5;3H0123456" },
                                           Case { "\033[3;10s", "\033[5;3H012345678" },
                                           Case { "\033[1;8s", "\033[5;1H012345678" },
                                           Case { "\033[3;8s", "\033[4;3H0123456" });
    auto mc =
        MockTerm { PageSize { LineCount(5), ColumnCount(10) }, HistoryLimits::plain(Infinite {}), 1024 };
    mc.writeToScreen("\033[5;1H\033]133;A\033\\$ x\033]133;D\033\\");
    mc.writeToScreen(std::format("\033[?69h{}{}", margins, write));

    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(4));
    CHECK(row.isFlagEnabled(LineFlag::Marked));
    CHECK_FALSE(row.wrapped());
}
