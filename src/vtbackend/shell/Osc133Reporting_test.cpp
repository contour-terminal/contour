// SPDX-License-Identifier: Apache-2.0
//
// OSC 133 as the shells actually send it -- the bundled integrations of
// src/contour/cli/shell-integration/ and the native ones of fish and Nushell -- read right.

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace vtbackend::test;

namespace
{

/// @return The command line the record holds once @p encoded was reported with OSC 133;C, after a prompt.
[[nodiscard]] std::string recordedCommandLine(std::string_view encoded)
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen(reportedCommandStart(encoded));

    auto const* const record = mock.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLineSource == CommandLineSource::Reported);
    return record->commandLine.raw();
}

} // namespace

TEST_CASE("cmdline_url decodes a command line encoded byte by byte", "[osc133][cmdline_url]")
{
    // One %XX per UTF-8 byte, upper-case hex: what fish, and the bundled bash, zsh and pwsh integrations,
    // send.
    CHECK(recordedCommandLine("echo%20%22a%20%20b%22%20%C3%BC%E2%82%AC%F0%9F%98%80")
          == "echo \"a  b\" \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80");
    CHECK(recordedCommandLine("%c3%bc") == "\xc3\xbc"); // lower-case hex decodes as well
}

TEST_CASE("cmdline_url carries what only percent-encoding can", "[osc133][cmdline_url]")
{
    // ';' would end the parameter for a terminal that splits on it, and ESC or BEL the sequence; encoded,
    // they arrive. The record keeps them RAW -- sanitizeCommandLine() makes them safe per use.
    CHECK(recordedCommandLine("a%3Bb%3Dc") == "a;b=c");
    CHECK(recordedCommandLine("x%1B%5B201~") == "x\x1b[201~");
    CHECK(recordedCommandLine("50%25") == "50%");
    CHECK(recordedCommandLine("l1%0Al2") == "l1\nl2");
}

TEST_CASE("cmdline_url keeps a broken escape as it came", "[osc133][cmdline_url]")
{
    CHECK(recordedCommandLine("100%") == "100%");
    CHECK(recordedCommandLine("%zz") == "%zz");
}

TEST_CASE("OSC 133;A k=s continues the prompt in progress", "[osc133]")
{
    // A two-line command typed into Nushell, marked byte for byte as reedline marks it.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen("\033]133;A;k=i;click_events=1\033\\> \033]133;B\033\\for x in [1 2] {\r\n");
    mock.writeToScreen("\033]133;A;k=s;click_events=1\033\\::: \033]133;B\033\\print $x }\r\n");
    mock.writeToScreen("\033]133;C\033\\1\r\n2\r\n\033]133;D;0\033\\");

    // One block, headed by the first line only.
    auto const& screen = mock.terminal.currentScreen();
    CHECK(screen.lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    CHECK_FALSE(screen.lineFlagsAt(LineOffset(1)).contains(LineFlag::Marked));

    auto const* const record = mock.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->exitCode == std::optional { 0 });
    auto const* const head = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(head != nullptr);
    CHECK(head->id == record->id);

    // Nushell reports no command line, so it is read off the grid: every line's input, and no prompt.
    CHECK(record->commandLine.raw() == "for x in [1 2] {\nprint $x }");
    CHECK(record->commandLineSource == CommandLineSource::Recovered);
}

TEST_CASE("OSC 133 k=r beside a prompt does not start another", "[osc133]")
{
    // Nushell marks its right prompt with ;P, which starts nothing anyway; an A of that kind is the same
    // mark.
    auto const mark = GENERATE('P', 'A');
    INFO("OSC 133;" << mark << ";k=r");
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mock.writeToScreen(PromptStart);
    auto const first = mock.terminal.commandBlocks().currentId();
    mock.writeToScreen(std::format("\033]133;{};k=r\033\\\033]133;B\033\\ls\r\n", mark));
    mock.writeToScreen("\033]133;C\033\\a\r\n\033]133;D;0\033\\");

    auto const* const record = mock.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->id == first); // the right prompt's mark started no record of its own
    auto const* const head = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(head != nullptr);
    CHECK(head->id == record->id);
}

TEST_CASE("OSC 133;A k=r says where a right prompt begins, as ;P k=r does", "[osc133]")
{
    // One blank apart from the command, the right prompt is too close to pass for one by its looks alone:
    // only the mark keeps it out of the command line read off the grid.
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mock.writeToScreen(std::format("{}\033]133;B\033\\", PromptStart));
    auto const block = mock.terminal.commandBlocks().currentId();
    mock.writeToScreen("\0337\033[10G\033]133;A;k=r\033\\~/src 10:42\0338"); // DECSC, CHA, DECRC
    mock.writeToScreen("ls -la\r\n\033]133;C\033\\");

    auto const* const record = mock.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->id == block);
    CHECK(record->commandLine.raw() == "ls -la");
}
