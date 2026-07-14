// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CommandBlocks.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace std::string_literals;

namespace
{

/// A command-block line source backed by a plain vector — the whole point of the CommandBlockLineSource
/// seam. Lines are given in the order the scan walks them: the cursor line first, then upwards, one entry
/// per LOGICAL line.
class FakeLines final: public CommandBlockLineSource
{
  public:
    struct Entry
    {
        LineFlags flags;
        std::string text;

        /// How many columns of @ref text a finished command printed before the shell painted its next
        /// prompt onto the very same line. Only meaningful together with LineFlag::CommandEnd, and zero in
        /// the ordinary case, where the command's output ended in a newline and the whole line is prompt.
        size_t commandEndOffset = 0;
    };

    explicit FakeLines(std::vector<Entry> lines): _lines { std::move(lines) } {}

    [[nodiscard]] bool hasLineAt(size_t index) const override { return index < _lines.size(); }
    [[nodiscard]] LineFlags flagsAt(size_t index) const override { return _lines.at(index).flags; }

    [[nodiscard]] std::string textAt(size_t index) const override
    {
        ++_textReads;
        return _lines.at(index).text;
    }

    [[nodiscard]] std::string textBeforeCommandEndAt(size_t index) const override
    {
        ++_textReads;
        auto const& entry = _lines.at(index);
        return entry.text.substr(0, splitOf(entry));
    }

    [[nodiscard]] std::string textFromCommandEndAt(size_t index) const override
    {
        ++_textReads;
        auto const& entry = _lines.at(index);
        return entry.text.substr(splitOf(entry));
    }

    /// How many lines the scan actually read the text of — the flags-only-walk claim, made checkable.
    [[nodiscard]] size_t textReads() const noexcept { return _textReads; }

  private:
    [[nodiscard]] static size_t splitOf(Entry const& entry) noexcept
    {
        return std::min(entry.commandEndOffset, entry.text.size());
    }

    std::vector<Entry> _lines;
    mutable size_t _textReads = 0;
};

} // namespace

TEST_CASE("CommandBlocks.scan.oneBlock", "[commandblocks]")
{
    // The layout a shell leaves behind for `ls`, walked from the cursor upwards. The bottom line carries
    // BOTH the finished command's CommandEnd and the new prompt's Marked, because precmd emits OSC 133;D
    // and ;A back to back and then prints the prompt onto that line.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
        { .flags = LineFlags {}, .text = "file2" },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "file1" },
        { .flags = LineFlags { LineFlag::Marked }, .text = "$ ls" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].prompt == "$ ls");
    CHECK(blocks[0].output == "file1\nfile2");
    CHECK(blocks[0].outputLineCount == 2);
}

TEST_CASE("CommandBlocks.scan.doesNotLeakTheNextPromptIntoTheOutput", "[commandblocks]")
{
    // The regression this scanner was extracted to fix: the CommandEnd line is also Marked and holds the
    // NEXT prompt, so its text must not be taken as the finished command's last output line. The output
    // ended in a newline, so the prompt got a line of its own and nothing on it belongs to the command —
    // which is exactly what a command-end offset of zero says.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "user@host:~$ " },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "hello" },
        { .flags = LineFlags { LineFlag::Marked }, .text = "user@host:~$ echo hello" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].output == "hello");
    CHECK(blocks[0].output.find("user@host") == std::string::npos);
    CHECK(blocks[0].outputLineCount == 1);
}

TEST_CASE("CommandBlocks.scan.outputWithoutTrailingNewlineSharesItsLineWithTheNextPrompt", "[commandblocks]")
{
    // `printf 'a\nb\nc'` leaves the cursor sitting after the `c`, so precmd emits ;D and ;A right there and
    // the shell paints the new prompt onto the same line. That one line has two owners: the `c` is the
    // command's last output line, the `$ ` after it is the next prompt. Dropping the line loses the `c`;
    // taking all of it leaks the prompt. The command-end offset is the border between them.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked },
          .text = "c$ ",
          .commandEndOffset = 1 },
        { .flags = LineFlags {}, .text = "b" },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "a" },
        { .flags = LineFlags { LineFlag::Marked }, .text = R"($ printf 'a\nb\nc')" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].output == "a\nb\nc");
    CHECK(blocks[0].outputLineCount == 3);
    CHECK(blocks[0].prompt == R"($ printf 'a\nb\nc')");
}

TEST_CASE("CommandBlocks.scan.outputThatFitsOnTheLineItStartedOn", "[commandblocks]")
{
    // `printf hello`: the output begins and ends on the same line, and the prompt lands on it too. That one
    // line therefore carries OutputStart, CommandEnd and Marked all at once — and the whole of the
    // command's output is the handful of columns before the ;D.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::OutputStart, LineFlag::CommandEnd, LineFlag::Marked },
          .text = "hello$ ",
          .commandEndOffset = 5 },
        { .flags = LineFlags { LineFlag::Marked }, .text = "$ printf hello" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].output == "hello");
    CHECK(blocks[0].outputLineCount == 1);
    CHECK(blocks[0].prompt == "$ printf hello");
}

TEST_CASE("CommandBlocks.scan.aSharedLinesPromptHalfIsNotSwallowedByThePrompt", "[commandblocks]")
{
    // The mirror image, one block older: the line that BEGINS the newer block's prompt is the same line the
    // older command ended part-way into. The prompt must take only its own half of it — otherwise every
    // "copy last command" of the newer block starts with the tail of the older one's output.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "hi" },
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked },
          .text = "tail$ echo hi",
          .commandEndOffset = 4 },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "head" },
        { .flags = LineFlags { LineFlag::Marked }, .text = "$ print-two-lines" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 2);
    REQUIRE(blocks.size() == 2);

    CHECK(blocks[0].prompt == "$ echo hi");
    CHECK(blocks[0].output == "hi");

    CHECK(blocks[1].prompt == "$ print-two-lines");
    CHECK(blocks[1].output
          == "head\n"
             "tail");
    CHECK(blocks[1].outputLineCount == 2);
}

TEST_CASE("CommandBlocks.scan.multiLinePrompt", "[commandblocks]")
{
    // A two-line prompt (powerlevel10k and friends): everything from the Marked head down to the
    // OutputStart is prompt. This is exactly what the coarse, mark-only extraction gets wrong.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "~ " },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "out" },
        { .flags = LineFlags {}, .text = "> ls" },
        { .flags = LineFlags { LineFlag::Marked }, .text = "~/projects  main" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].prompt == "~/projects  main\n> ls");
    CHECK(blocks[0].output == "out");
}

TEST_CASE("CommandBlocks.scan.chainsAcrossBlocks", "[commandblocks]")
{
    // Two commands back to back. The Marked line that begins the newer block's prompt ALSO carries the
    // older block's CommandEnd, so the walk must chain into it rather than resume searching one line up —
    // otherwise it steps over the boundary it is standing on and mis-attributes every older block.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "second" },
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ echo second" },
        { .flags = LineFlags { LineFlag::OutputStart }, .text = "first" },
        { .flags = LineFlags { LineFlag::Marked }, .text = "$ echo first" },
    } };

    SECTION("the newest block alone")
    {
        auto const blocks = scanCommandBlocksBackward(lines, 1);
        REQUIRE(blocks.size() == 1);
        CHECK(blocks[0].prompt == "$ echo second");
        CHECK(blocks[0].output == "second");
    }

    SECTION("both, most recent first")
    {
        auto const blocks = scanCommandBlocksBackward(lines, 2);
        REQUIRE(blocks.size() == 2);
        CHECK(blocks[0].prompt == "$ echo second");
        CHECK(blocks[0].output == "second");
        CHECK(blocks[1].prompt == "$ echo first");
        CHECK(blocks[1].output == "first");
    }
}

TEST_CASE("CommandBlocks.scan.commandWithNoOutput", "[commandblocks]")
{
    // `cd /tmp` prints nothing, so there is no OutputStart between the two prompts.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
        { .flags = LineFlags { LineFlag::Marked }, .text = "$ cd /tmp" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].prompt == "$ cd /tmp");
    CHECK(blocks[0].output.empty());
    CHECK(blocks[0].outputLineCount == 0);
}

TEST_CASE("CommandBlocks.scan.noShellIntegration", "[commandblocks]")
{
    // The common case for a user whose prompt emits nothing: no flags anywhere. The scan must come back
    // empty AND must not have read a single line's text — this is the cost of a right-click on a long
    // scrollback, so it stays a flags-only walk.
    auto const lines = FakeLines { {
        { .flags = LineFlags {}, .text = "some" },
        { .flags = LineFlags {}, .text = "plain" },
        { .flags = LineFlags {}, .text = "output" },
    } };

    CHECK(scanCommandBlocksBackward(lines, 1).empty());
    CHECK(lines.textReads() == 0);
}

TEST_CASE("CommandBlocks.scan.aWalkThatReconstructsNothingIsNoBlock", "[commandblocks]")
{
    // A freshly cleared screen: `clear` drops the history, and the only line left is the prompt, carrying
    // the CommandEnd of a command that has just scrolled out of existence. There is no command here — and
    // an all-empty block reported as one would have "Copy Last Command Output" put "" on the clipboard,
    // wiping whatever the user had copied for a command that never was.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
    } };

    CHECK(scanCommandBlocksBackward(lines, 1).empty());
}

TEST_CASE("CommandBlocks.scan.truncatedHistory", "[commandblocks]")
{
    // The block's prompt scrolled off the top of the history. Keep what is left rather than dropping it.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, .text = "$ " },
        { .flags = LineFlags {}, .text = "tail of a long output" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].prompt.empty());
    CHECK(blocks[0].output == "tail of a long output");
}

TEST_CASE("CommandBlocks.scan.degenerateRequests", "[commandblocks]")
{
    auto const lines = FakeLines { { { .flags = LineFlags { LineFlag::CommandEnd }, .text = "x" } } };
    CHECK(scanCommandBlocksBackward(lines, 0).empty());
    CHECK(scanCommandBlocksBackward(FakeLines { {} }, 4).empty());
}

TEST_CASE("CommandBlocks.textOf", "[commandblocks]")
{
    auto const block = CommandBlockText { .prompt = "$ ls", .output = "a\nb", .outputLineCount = 2 };

    CHECK(textOf(block, CommandBlockPart::Prompt) == "$ ls");
    CHECK(textOf(block, CommandBlockPart::Output) == "a\nb");
    CHECK(textOf(block, CommandBlockPart::PromptAndOutput) == "$ ls\na\nb");

    SECTION("a command that printed nothing joins to just its prompt")
    {
        auto const silent = CommandBlockText { .prompt = "$ cd /tmp", .output = "", .outputLineCount = 0 };
        CHECK(textOf(silent, CommandBlockPart::PromptAndOutput) == "$ cd /tmp");
        CHECK(textOf(silent, CommandBlockPart::Output).empty());
    }

    SECTION("a block whose prompt scrolled away joins to just its output")
    {
        auto const headless = CommandBlockText { .prompt = "", .output = "a", .outputLineCount = 1 };
        CHECK(textOf(headless, CommandBlockPart::PromptAndOutput) == "a");
    }
}
