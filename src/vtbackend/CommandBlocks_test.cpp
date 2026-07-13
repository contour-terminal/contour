// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CommandBlocks.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vtbackend;
using namespace std::string_literals;

namespace
{

/// A command-block line source backed by a plain vector — the whole point of the CommandBlockLineSource
/// seam. Lines are given in the order the scan walks them: the cursor line first, then upwards.
class FakeLines final: public CommandBlockLineSource
{
  public:
    struct Entry
    {
        LineFlags flags;
        std::string text;
    };

    explicit FakeLines(std::vector<Entry> lines): _lines { std::move(lines) } {}

    [[nodiscard]] size_t lineCount() const override { return _lines.size(); }
    [[nodiscard]] LineFlags flagsAt(size_t index) const override { return _lines.at(index).flags; }

    [[nodiscard]] std::string textAt(size_t index) const override
    {
        ++_textReads;
        return _lines.at(index).text;
    }

    /// How many lines the scan actually read the text of — the flags-only-walk claim, made checkable.
    [[nodiscard]] size_t textReads() const noexcept { return _textReads; }

  private:
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
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "$ " },
        { LineFlags {}, "file2" },
        { LineFlags { LineFlag::OutputStart }, "file1" },
        { LineFlags { LineFlag::Marked }, "$ ls" },
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
    // NEXT prompt, so its text must not be taken as the finished command's last output line.
    auto const lines = FakeLines { {
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "user@host:~$ " },
        { LineFlags { LineFlag::OutputStart }, "hello" },
        { LineFlags { LineFlag::Marked }, "user@host:~$ echo hello" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].output == "hello");
    CHECK(blocks[0].output.find("user@host") == std::string::npos);
    CHECK(blocks[0].outputLineCount == 1);
}

TEST_CASE("CommandBlocks.scan.commandEndWithoutMarkIsRealOutput", "[commandblocks]")
{
    // The other side of that coin: a command whose output does not end in a newline (`printf foo`) leaves
    // the cursor on its last output line, so CommandEnd lands there WITHOUT a Marked. That line's text is
    // genuine output and must be kept.
    auto const lines = FakeLines { {
        { LineFlags { LineFlag::CommandEnd }, "foo" },
        { LineFlags { LineFlag::OutputStart }, "bar" },
        { LineFlags { LineFlag::Marked }, "$ printf 'bar\\nfoo'" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].output == "bar\nfoo");
    CHECK(blocks[0].outputLineCount == 2);
}

TEST_CASE("CommandBlocks.scan.multiLinePrompt", "[commandblocks]")
{
    // A two-line prompt (powerlevel10k and friends): everything from the Marked head down to the
    // OutputStart is prompt. This is exactly what the coarse, mark-only extraction gets wrong.
    auto const lines = FakeLines { {
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "~ " },
        { LineFlags { LineFlag::OutputStart }, "out" },
        { LineFlags {}, "> ls" },
        { LineFlags { LineFlag::Marked }, "~/projects  main" },
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
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "$ " },
        { LineFlags { LineFlag::OutputStart }, "second" },
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "$ echo second" },
        { LineFlags { LineFlag::OutputStart }, "first" },
        { LineFlags { LineFlag::Marked }, "$ echo first" },
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
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "$ " },
        { LineFlags { LineFlag::Marked }, "$ cd /tmp" },
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
        { LineFlags {}, "some" },
        { LineFlags {}, "plain" },
        { LineFlags {}, "output" },
    } };

    CHECK(scanCommandBlocksBackward(lines, 1).empty());
    CHECK(lines.textReads() == 0);
}

TEST_CASE("CommandBlocks.scan.truncatedHistory", "[commandblocks]")
{
    // The block's prompt scrolled off the top of the history. Keep what is left rather than dropping it.
    auto const lines = FakeLines { {
        { LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, "$ " },
        { LineFlags {}, "tail of a long output" },
    } };

    auto const blocks = scanCommandBlocksBackward(lines, 1);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].prompt.empty());
    CHECK(blocks[0].output == "tail of a long output");
}

TEST_CASE("CommandBlocks.scan.degenerateRequests", "[commandblocks]")
{
    auto const lines = FakeLines { { { LineFlags { LineFlag::CommandEnd }, "x" } } };
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
