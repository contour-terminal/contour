// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/SemanticBlockTracker.h>
#include <vtbackend/ShellIntegration.h>
#include <vtbackend/Terminal.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace vtbackend;
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

class MockShellIntegration: public ShellIntegration
{
  public:
    int promptStartCount = 0;
    bool lastPromptStartClickEvents = false;
    int promptEndCount = 0;
    int commandOutputStartCount = 0;
    std::optional<std::string> lastCommandOutputStartUrl;
    int commandFinishedCount = 0;
    int lastCommandFinishedExitCode = -1;

    void promptStart(bool clickEvents) override
    {
        promptStartCount++;
        lastPromptStartClickEvents = clickEvents;
    }

    void promptEnd() override { promptEndCount++; }

    void commandOutputStart(std::optional<std::string> const& commandLineUrl) override
    {
        commandOutputStartCount++;
        lastCommandOutputStartUrl = commandLineUrl;
    }

    void commandFinished(int exitCode) override
    {
        commandFinishedCount++;
        lastCommandFinishedExitCode = exitCode;
    }
};

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

} // namespace

TEST_CASE("ShellIntegration.OSC_133")
{
    auto mockUnique = std::make_unique<MockShellIntegration>();
    auto* mock = mockUnique.get();

    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.terminal.setShellIntegration(std::move(mockUnique));

    SECTION("A: Prompt Start")
    {
        mc.writeToScreen("\033]133;A\033\\");
        CHECK(mock->promptStartCount == 1);
        CHECK(mock->lastPromptStartClickEvents == false);
    }

    SECTION("A: Prompt Start with click_events")
    {
        mc.writeToScreen("\033]133;A;click_events=1\033\\");
        CHECK(mock->promptStartCount == 1);
        CHECK(mock->lastPromptStartClickEvents == true);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("B: Prompt End")
    {
        mc.writeToScreen("\033]133;B\033\\");
        CHECK(mock->promptEndCount == 1);
    }

    SECTION("C: Command Output Start")
    {
        mc.writeToScreen("\033]133;C\033\\");
        CHECK(mock->commandOutputStartCount == 1);
        CHECK(mock->lastCommandOutputStartUrl == std::nullopt);
    }

    SECTION("C: Command Output Start with cmdline_url")
    {
        // "foo%20bar" unescapes to "foo bar"
        mc.writeToScreen("\033]133;C;cmdline_url=foo%20bar\033\\");
        CHECK(mock->commandOutputStartCount == 1);
        REQUIRE(mock->lastCommandOutputStartUrl.has_value());
        CHECK(mock->lastCommandOutputStartUrl.value() == "foo bar");
    }

    SECTION("D: Command Finished")
    {
        mc.writeToScreen("\033]133;D\033\\");
        CHECK(mock->commandFinishedCount == 1);
        CHECK(mock->lastCommandFinishedExitCode == 0);
    }

    SECTION("D: Command Finished with exit code")
    {
        mc.writeToScreen("\033]133;D;123\033\\");
        CHECK(mock->commandFinishedCount == 1);
        CHECK(mock->lastCommandFinishedExitCode == 123);
    }
}

TEST_CASE("ShellIntegration.SETMARK")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("SETMARK triggers promptStart")
    {
        mc.writeToScreen("\033[>M");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }
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

    SECTION("What mode 2034 still gates: the tracker, not the flags")
    {
        // The other half of the split above, pinned so it cannot quietly drift: with mode 2034 off, the
        // flags land but the block tracker records nothing at all.
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::OutputStart));
        CHECK_FALSE(mc.terminal.semanticBlockTracker().currentBlock().has_value());
    }
}

TEST_CASE("SemanticBlockProtocol.lastCommandBlockWithoutMode2034")
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

    auto const block = mc.terminal.lastCommandBlock();
    REQUIRE(block.has_value());
    CHECK(block->prompt == "$ ls");
    CHECK(block->output == "file1\nfile2");
    CHECK(block->outputLineCount == 2);

    // The new prompt must not have leaked into the finished command's output.
    CHECK(textOf(*block, CommandBlockPart::Output) == "file1\nfile2");
    CHECK(textOf(*block, CommandBlockPart::PromptAndOutput) == "$ ls\nfile1\nfile2");
}

TEST_CASE("SemanticBlockProtocol.lastCommandBlockWithoutShellIntegration")
{
    // No OSC 133 anywhere: the menu entries must be hidden, so the answer has to be "nothing", not garbage.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.writeToScreen("just some plain output\n");

    CHECK_FALSE(mc.terminal.lastCommandBlock().has_value());
}

// {{{ output that does not end in a newline
namespace
{
/// What the bundled shell integration actually puts on the wire between two commands.
///
/// precmd emits ;D (the command finished), then SETMARK, then ;A (a new prompt starts) — all three at the
/// cursor, back to back, before PS1 prints a single character. So whenever a command's output did not end
/// in a newline, every one of those marks lands on the output's last line, and the prompt is painted onto
/// it too.
void writePrecmdAndPrompt(MockTerm<>& mc, std::string_view prompt, int exitCode = 0)
{
    mc.writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
    mc.writeToScreen("\033[>M");
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

    auto const block = mc.terminal.lastCommandBlock();
    REQUIRE(block.has_value());
    CHECK(block->output == "a\nb\nc");
    CHECK(block->outputLineCount == 3);
    CHECK(block->prompt == R"($ printf 'a\nb\nc')");
}

TEST_CASE("SemanticBlockProtocol.outputThatFitsOnTheLineItStartedOn")
{
    // `printf hello`: OutputStart, CommandEnd and the next prompt's Marked all land on one line, and the
    // whole of the command's output is the five columns before the ;D. Getting this wrong loses the output
    // in its entirety.
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    writePromptAndRun(mc, "$ printf hello", "hello");
    writePrecmdAndPrompt(mc, "$ ");

    auto const block = mc.terminal.lastCommandBlock();
    REQUIRE(block.has_value());
    CHECK(block->output == "hello");
    CHECK(block->outputLineCount == 1);
    CHECK(block->prompt == "$ printf hello");
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

    auto const before = mc.terminal.lastCommandBlock();
    REQUIRE(before.has_value());
    CHECK(before->output == expectedOutput);
    CHECK(before->prompt == "$ wrap");

    SECTION("widening the window")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(40) });

        auto const after = mc.terminal.lastCommandBlock();
        REQUIRE(after.has_value());
        CHECK(after->output == expectedOutput);
        CHECK(after->prompt == "$ wrap");
    }

    SECTION("narrowing the window")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(6) });

        auto const after = mc.terminal.lastCommandBlock();
        REQUIRE(after.has_value());
        CHECK(after->output == expectedOutput);
        CHECK(after->prompt == "$ wrap");
    }
}

TEST_CASE("SemanticBlockProtocol.noCommandAtAllIsNoBlock")
{
    // A prompt carrying the CommandEnd of a command whose every line has scrolled away describes no
    // command. Reporting it as a block would have "Copy Last Command Output" wipe the clipboard with "".
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    writePrecmdAndPrompt(mc, "$ ");

    CHECK_FALSE(mc.terminal.lastCommandBlock().has_value());
}
// }}}

TEST_CASE("SemanticBlockProtocol.TrackerMetadata")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("Tracker stores command line and exit code")
    {
        mc.writeToScreen(DECSM(2034));

        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("$ ");
        mc.writeToScreen("\033]133;C;cmdline_url=ls%20-la\033\\");
        mc.writeToScreen("file1\n");
        mc.writeToScreen("\033]133;D;0\033\\");

        auto const& tracker = mc.terminal.semanticBlockTracker();
        REQUIRE(tracker.currentBlock().has_value());
        CHECK(tracker.currentBlock()->finished == true);
        REQUIRE(tracker.currentBlock()->commandLine.has_value());
        CHECK(tracker.currentBlock()->commandLine.value() == "ls -la");
        CHECK(tracker.currentBlock()->exitCode == 0);
    }

    SECTION("Tracker disabled clears data")
    {
        mc.writeToScreen(DECSM(2034));
        mc.writeToScreen("\033]133;A\033\\");
        mc.writeToScreen("\033]133;C;cmdline_url=test\033\\");
        mc.writeToScreen("\033]133;D;0\033\\");

        auto const& tracker = mc.terminal.semanticBlockTracker();
        CHECK(tracker.currentBlock().has_value());

        // Disable mode
        mc.writeToScreen(DECRM(2034));
        CHECK_FALSE(tracker.currentBlock().has_value());
        CHECK(tracker.completedBlocks().empty());
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
        CHECK(mc.replyData().find("2034;2$y") != std::string::npos);
    }

    SECTION("DECRQM reports mode 2034 as set after enabling")
    {
        mc.writeToScreen(DECSM(2034));
        mc.terminal.flushInput();
        mc.resetReplyData();
        mc.writeToScreen(DECRQM(2034));
        mc.terminal.flushInput();
        // Response should be CSI ? 2034 ; 1 $ y  (1 = set)
        CHECK(mc.replyData().find("2034;1$y") != std::string::npos);
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
        CHECK(mc.replyData().find("\033P>0b\033\\") != std::string::npos);
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
    mc.resetReplyData();
    // Query last command with token
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();

    // Should get success DCS
    REQUIRE(reply.find("\033P>1b") != std::string::npos);
    // Should contain JSON with version
    CHECK(reply.find("\"version\":1") != std::string::npos);
    // Should contain the command
    CHECK(reply.find("\"command\":\"ls\"") != std::string::npos);
    // Should contain exit code
    CHECK(reply.find("\"exitCode\":0") != std::string::npos);
    // Should be marked as finished
    CHECK(reply.find("\"finished\":true") != std::string::npos);
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

    mc.resetReplyData();
    // Query last 2 commands with token
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastNumberOfCommands, 2, token));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    REQUIRE(reply.find("\033P>1b") != std::string::npos);
    // Both commands should be present
    CHECK(reply.find("\"command\":\"cmd1\"") != std::string::npos);
    CHECK(reply.find("\"command\":\"cmd2\"") != std::string::npos);
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

    mc.resetReplyData();
    // Query in-progress command with token
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::InProgress, 1, token));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    REQUIRE(reply.find("\033P>1b") != std::string::npos);
    // Should be marked as not finished
    CHECK(reply.find("\"finished\":false") != std::string::npos);
    // Should contain the command
    CHECK(reply.find("\"command\":\"running\"") != std::string::npos);
}

TEST_CASE("SemanticBlockProtocol.NoCompletedCommands")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture the session token
    auto const token = enableModeAndGetToken(mc);

    mc.resetReplyData();
    // Query last command when none exists, with valid token
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    // Should get error DCS (no data)
    CHECK(reply.find("\033P>0b\033\\") != std::string::npos);
}

// ==================== Token Authentication Tests ====================

TEST_CASE("SemanticBlockProtocol.TokenOnEnable")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    mc.resetReplyData();
    mc.writeToScreen(DECSM(2034));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();

    // Should receive DCS reply with token
    REQUIRE(reply.find("\033P>2034;1b") != std::string::npos);

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
    CHECK(reply.find("\033P>2b\033\\") != std::string::npos);
}

TEST_CASE("SemanticBlockProtocol.QueryWithWrongToken")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable mode 2034 and capture token
    enableModeAndGetToken(mc);

    // Simulate a complete command
    simulateCommand(mc, "$ ", "ls", "file1", 0);
    mc.writeToScreen("\033]133;A\033\\");

    mc.resetReplyData();
    // Query with a fabricated wrong token
    auto const wrongToken = SemanticBlockTracker::Token { 0xDEAD, 0xBEEF, 0xCAFE, 0xBABE };
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, wrongToken));
    mc.terminal.flushInput();

    auto const& reply = mc.replyData();
    // Should get status 3: authentication failed
    CHECK(reply.find("\033P>3b\033\\") != std::string::npos);
}

TEST_CASE("SemanticBlockProtocol.TokenChangesOnReEnable")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    // Enable, get first token
    auto const token1 = enableModeAndGetToken(mc);

    // Disable, then re-enable
    mc.writeToScreen(DECRM(2034));
    auto const token2 = enableModeAndGetToken(mc);

    // Tokens should be different (with overwhelming probability)
    CHECK(token1 != token2);

    // Simulate a command
    simulateCommand(mc, "$ ", "ls", "output", 0);
    mc.writeToScreen("\033]133;A\033\\");

    // Old token should be rejected
    mc.resetReplyData();
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token1));
    mc.terminal.flushInput();
    CHECK(mc.replyData().find("\033P>3b\033\\") != std::string::npos);

    // New token should work
    mc.resetReplyData();
    mc.writeToScreen(authenticatedSBQuery(SBQueryType::LastCommand, 1, token2));
    mc.terminal.flushInput();
    CHECK(mc.replyData().find("\033P>1b") != std::string::npos);
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
    // The offset is a LOGICAL column, so re-chopping the line into different physical pieces must not
    // move it. Without the re-application in Grid::addNewWrappedLines the flag would survive on the head
    // with a ZEROED offset, which reads as "the prompt ended at column 0" -- a silent, plausible lie.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    mc.writeToScreen("\033]133;A\033\\");
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
