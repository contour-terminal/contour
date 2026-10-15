// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockRows.hpp>
#include <vtbackend/shell/PromptRegion.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

using namespace vtbackend;

namespace
{

/// One row of a FakeRows grid.
struct FakeRow
{
    uint32_t block = 0;
    LineFlags flags {};
    int endColumn = 0; ///< Where an end mark on the row stands: Line::commandEndOffset().
};

/// A grid whose first row is @ref _top, as a scrollback's is (negative offsets are history).
class FakeRows final: public CommandBlockRowSource
{
  public:
    FakeRows(LineOffset top, std::vector<FakeRow> rows): _top { top }, _rows { std::move(rows) } {}

    [[nodiscard]] LineOffset topRow() const override { return _top; }

    [[nodiscard]] LineOffset bottomRow() const override
    {
        return _top + LineOffset::cast_from(_rows.size()) - LineOffset(1);
    }

    [[nodiscard]] CommandBlockId blockIdAt(LineOffset row) const override
    {
        return CommandBlockId(at(row).block);
    }

    [[nodiscard]] LineFlags flagsAt(LineOffset row) const override { return at(row).flags; }

    [[nodiscard]] ColumnOffset commandEndColumnAt(LineOffset row) const override
    {
        return ColumnOffset(at(row).endColumn);
    }

  private:
    [[nodiscard]] FakeRow const& at(LineOffset row) const
    {
        return _rows.at(static_cast<size_t>(unbox(row - _top)));
    }

    LineOffset _top;
    std::vector<FakeRow> _rows;
};

constexpr auto PromptRow = LineFlags { LineFlag::Marked };
constexpr auto OutputRow = LineFlags { LineFlag::OutputStart };
constexpr auto EndRow = LineFlags { LineFlag::CommandEnd };
constexpr auto WrappedRow = LineFlags { LineFlag::Wrapped };

/// The anchor's block finished on its own end mark, as a ;D reports it.
constexpr auto Reported = CommandProgress::EndReported;

/// Rows -2..4: an unstamped line, then `$ ls` with two lines of output, `$ pwd` with one, and the live
/// prompt.
[[nodiscard]] FakeRows threeBlocks()
{
    return FakeRows { LineOffset(-2),
                      {
                          FakeRow { .block = 0, .flags = {} },        // -2: before shell integration
                          FakeRow { .block = 1, .flags = PromptRow }, // -1: $ ls
                          FakeRow { .block = 1, .flags = OutputRow }, //  0: a
                          FakeRow { .block = 1, .flags = {} },        //  1: b
                          FakeRow { .block = 2, .flags = PromptRow }, //  2: $ pwd
                          FakeRow { .block = 2, .flags = OutputRow }, //  3: /tmp
                          FakeRow { .block = 3, .flags = PromptRow }, //  4: $ (live)
                      } };
}

[[nodiscard]] CommandBlockRows rowsOf(int first, int last)
{
    return CommandBlockRows { .first = LineOffset(first), .last = LineOffset(last) };
}

/// The rows [@p first, @p last], ending before logical column @p endColumn of their last line.
[[nodiscard]] CommandBlockRows rowsEndingBefore(int first, int last, int endColumn)
{
    return CommandBlockRows { .first = LineOffset(first),
                              .last = LineOffset(last),
                              .endColumn = ColumnOffset(endColumn) };
}

/// The rows [@p first, @p last], starting at logical column @p firstColumn of their first line.
[[nodiscard]] CommandBlockRows rowsStartingAt(int first, int last, int firstColumn)
{
    return CommandBlockRows { .first = LineOffset(first),
                              .last = LineOffset(last),
                              .firstColumn = ColumnOffset(firstColumn) };
}

} // namespace

TEST_CASE("CommandBlockRows.theOutputStartsAtTheOutputStartRow", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    // Any row of the block anchors it: the prompt row, the first and the last output row.
    for (auto const anchor: { -1, 0, 1 })
    {
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported) == rowsOf(0, 1));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Prompt, Reported)
              == rowsOf(-1, -1));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::PromptAndOutput, Reported)
              == rowsOf(-1, 1));
    }

    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::Output, Reported) == rowsOf(3, 3));
    CHECK(commandBlockRows(rows, LineOffset(3), CommandBlockPart::Prompt, Reported) == rowsOf(2, 2));
}

TEST_CASE("CommandBlockRows.aBlockWithoutOutputHasOnlyAPrompt", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::Prompt, Reported) == rowsOf(4, 4));
    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::PromptAndOutput, Reported) == rowsOf(4, 4));
}

TEST_CASE("CommandBlockRows.anUnstampedOrMissingRowIsNoBlock", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(commandBlockRows(rows, LineOffset(-2), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(-3), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(5), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
}

TEST_CASE("CommandBlockRows.anEmptyOutputLeavesItsFlagOnTheNextPromptRow", "[semanticblocks][blockrows]")
{
    // `cd /tmp` prints nothing: its ;C marks the row the next ;A then claims, so that row carries both
    // flags and belongs to the NEXT block. Neither block may read that stray OutputStart as its own.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow },
                                     FakeRow { .block = 2, .flags = PromptRow | OutputRow },
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Prompt, Reported) == rowsOf(1, 1));
}

TEST_CASE("CommandBlockRows.theNextBlocksOutputStartsBelowTheStrayFlag", "[semanticblocks][blockrows]")
{
    // As above, and the next command does print: its own ;C marks the row below its head, where its output
    // starts. The flag the empty command left on the head is still not the next block's.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow },             // $ cd /tmp
                                     FakeRow { .block = 2, .flags = PromptRow | OutputRow }, // $ ls
                                     FakeRow { .block = 2, .flags = OutputRow },             // a
                                     FakeRow { .block = 2, .flags = {} },                    // b
                                     FakeRow { .block = 3, .flags = PromptRow },             // $
                                 } };

    for (auto const anchor: { 1, 2, 3 })
    {
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported) == rowsOf(2, 3));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Prompt, Reported) == rowsOf(1, 1));
    }
}

TEST_CASE("CommandBlockRows.theOutputEndsOnTheRowTheCommandEndedOn", "[semanticblocks][blockrows]")
{
    // A precmd hook printed a line between the ;D and the next prompt's ;A. The finished block stamps that
    // row too, but the command did not print it: the output, and the block's prompt and output together, end
    // at the ;D -- before its column on the row it marked.
    SECTION("the output ended its line: nothing precedes the end mark on its row")
    {
        auto const rows = FakeRows { LineOffset(0),
                                     {
                                         FakeRow { .block = 1, .flags = PromptRow }, // 0: $ ls
                                         FakeRow { .block = 1, .flags = OutputRow }, // 1: a
                                         FakeRow { .block = 1, .flags = EndRow },    // 2: (;D)
                                         FakeRow { .block = 1, .flags = {} },        // 3: precmd
                                         FakeRow { .block = 2, .flags = PromptRow }, // 4: $
                                     } };

        for (auto const anchor: { 0, 1, 2, 3 })
        {
            CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported)
                  == rowsOf(1, 1));
            CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::PromptAndOutput, Reported)
                  == rowsOf(0, 1));
            CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Prompt, Reported)
                  == rowsOf(0, 0));
        }
    }

    SECTION("the output did not end its line: the end mark cuts its row")
    {
        auto const rows = FakeRows { LineOffset(0),
                                     {
                                         FakeRow { .block = 1, .flags = PromptRow },              // $ ls
                                         FakeRow { .block = 1, .flags = OutputRow },              // a
                                         FakeRow { .block = 1, .flags = EndRow, .endColumn = 1 }, // b(;D)
                                         FakeRow { .block = 1, .flags = {} },                     // precmd
                                         FakeRow { .block = 2, .flags = PromptRow },              // $
                                     } };

        CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported)
              == rowsEndingBefore(1, 2, 1));
        CHECK(commandBlockRows(rows, LineOffset(3), CommandBlockPart::PromptAndOutput, Reported)
              == rowsEndingBefore(0, 2, 1));
    }

    SECTION("the command printed nothing: its output is empty")
    {
        auto const rows = FakeRows { LineOffset(0),
                                     {
                                         FakeRow { .block = 1, .flags = PromptRow },          // $ true
                                         FakeRow { .block = 1, .flags = OutputRow | EndRow }, // (;C;D)precmd
                                         FakeRow { .block = 2, .flags = PromptRow },          // $
                                     } };

        CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported)
              == std::unexpected(CommandBlockActionError::EmptyPart));
        CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::PromptAndOutput, Reported)
              == rowsOf(0, 0));
    }
}

TEST_CASE("CommandBlockRows.theOutputEndsWithTheLineTheCommandEndedOn", "[semanticblocks][blockrows]")
{
    // The command's last line wrapped, and the ;D came before a newline: the mark is on the line's first row,
    // and what the command printed before it runs on into the row the wrap continues it on.
    auto const rows =
        FakeRows { LineOffset(0),
                   {
                       FakeRow { .block = 1, .flags = PromptRow },               // 0: $ cat
                       FakeRow { .block = 1, .flags = OutputRow },               // 1: one
                       FakeRow { .block = 1, .flags = EndRow, .endColumn = 18 }, // 2: a long last li
                       FakeRow { .block = 1, .flags = WrappedRow },              // 3: ne(;D)
                       FakeRow { .block = 1, .flags = {} },                      // 4: precmd
                       FakeRow { .block = 2, .flags = PromptRow },               // 5: $
                   } };

    // Sixteen columns a row: the mark's logical column 18 lies on the row the wrap continues the line on.
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported)
          == rowsEndingBefore(1, 3, 18));
    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::PromptAndOutput, Reported)
          == rowsEndingBefore(0, 3, 18));
}

TEST_CASE("CommandBlockRows.anOutputTailOnTheNextPromptsRowEndsAtTheEndMark", "[semanticblocks][blockrows]")
{
    // `printf foo`: its output start, its end mark and the next prompt all land on one row, which the next
    // prompt's ;A takes for its own block. The columns before the end mark are still the output of printf;
    // the columns from it on are the next block's prompt.
    auto const rows = FakeRows {
        LineOffset(0),
        {
            FakeRow { .block = 1, .flags = PromptRow },                                      // $ printf foo
            FakeRow { .block = 2, .flags = PromptRow | OutputRow | EndRow, .endColumn = 3 }, // foo$ ls
            FakeRow { .block = 2, .flags = OutputRow },                                      // a-file
            FakeRow { .block = 3, .flags = PromptRow },                                      // $
        }
    };

    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, Reported)
          == rowsEndingBefore(1, 1, 3));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::PromptAndOutput, Reported)
          == rowsEndingBefore(0, 1, 3));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Prompt, Reported) == rowsOf(0, 0));

    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Prompt, Reported)
          == rowsStartingAt(1, 1, 3));
    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::PromptAndOutput, Reported)
          == rowsStartingAt(1, 2, 3));
    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::Output, Reported) == rowsOf(2, 2));

    // Only an end the command reported is its own: a block the next prompt closed without one reads no mark
    // below its rows, as the semantic-block query reply reads none.
    auto const unreported = CommandProgress::Unreported;
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, unreported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::PromptAndOutput, unreported)
          == rowsOf(0, 0));
}

TEST_CASE("CommandBlockRows.anEndMarkOnTheHeadEndsThePreviousBlock", "[semanticblocks][blockrows]")
{
    // The ;D that ends `ls` lands on the row the next ;A then claims, so the next block's head carries it.
    // It ends `ls`'s output, not the output of the block it heads.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow },          // 0: $ ls
                                     FakeRow { .block = 1, .flags = OutputRow },          // 1: a
                                     FakeRow { .block = 2, .flags = PromptRow | EndRow }, // 2: $ make
                                     FakeRow { .block = 2, .flags = OutputRow },          // 3: building
                                     FakeRow { .block = 2, .flags = {} },                 // 4: done
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported) == rowsOf(1, 1));
    CHECK(commandBlockRows(rows, LineOffset(3), CommandBlockPart::Output, Reported) == rowsOf(3, 4));
    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::PromptAndOutput, Reported) == rowsOf(2, 4));
}

TEST_CASE("CommandBlockRows.aPromptMaySpanSeveralRows", "[semanticblocks][blockrows]")
{
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow }, // ~/src (main)
                                     FakeRow { .block = 1, .flags = {} },        // $ make
                                     FakeRow { .block = 1, .flags = OutputRow }, // built
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::Prompt, Reported) == rowsOf(0, 1));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, Reported) == rowsOf(2, 2));
}

TEST_CASE("CommandBlockRows.aBlockWhoseHeadWasEvictedIsAllOutput", "[semanticblocks][blockrows]")
{
    // The scrollback dropped the prompt and the output-start row; what survives is output.
    auto const rows = FakeRows { LineOffset(-2),
                                 {
                                     FakeRow { .block = 1, .flags = {} },
                                     FakeRow { .block = 1, .flags = {} },
                                     FakeRow { .block = 2, .flags = PromptRow },
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(-2), CommandBlockPart::Output, Reported) == rowsOf(-2, -1));
    CHECK(commandBlockRows(rows, LineOffset(-1), CommandBlockPart::Prompt, Reported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(-1), CommandBlockPart::PromptAndOutput, Reported)
          == rowsOf(-2, -1));

    // The prompt went, the output-start row survived.
    auto const kept = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = OutputRow },
                                     FakeRow { .block = 1, .flags = {} },
                                 } };
    CHECK(commandBlockRows(kept, LineOffset(1), CommandBlockPart::Output, Reported) == rowsOf(0, 1));
    CHECK(commandBlockRows(kept, LineOffset(1), CommandBlockPart::Prompt, Reported)
          == std::unexpected(CommandBlockActionError::EmptyPart));
}

TEST_CASE("CommandBlockRows.aRunningBlockRunsToTheBottomRow", "[semanticblocks][blockrows]")
{
    // `make` is still printing: its rows reach the page's last row, and so does its output.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow }, // $ make
                                     FakeRow { .block = 1, .flags = OutputRow }, // compiling a
                                     FakeRow { .block = 1, .flags = {} },        // compiling b
                                     FakeRow { .block = 1, .flags = {} },        // compiling c
                                 } };

    for (auto const anchor: { 0, 1, 3 })
    {
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported) == rowsOf(1, 3));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::PromptAndOutput, Reported)
              == rowsOf(0, 3));
    }
    CHECK(bottomRowOfBlock(rows, CommandBlockId(1)) == LineOffset(3));
}

TEST_CASE("CommandBlockRows.aBlockRunsThroughRowsWithNoBlockBetweenItsRows", "[semanticblocks][blockrows]")
{
    // `make` printed its first lines, went back up and erased from there down (ED 0), then moved past the
    // erased rows and printed more. A row an erase blanked away from the cursor carries no block, which says
    // nothing of whose it is: between two rows of one block it is that block's blank line.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow }, // 0: $ make
                                     FakeRow { .block = 1, .flags = OutputRow }, // 1: 1
                                     FakeRow { .block = 0, .flags = {} },        // 2: (erased)
                                     FakeRow { .block = 0, .flags = {} },        // 3: (erased)
                                     FakeRow { .block = 1, .flags = {} },        // 4: 5
                                     FakeRow { .block = 2, .flags = PromptRow }, // 5: $
                                 } };

    // Whether anchored above the erased rows or below them: the block runs through them.
    for (auto const anchor: { 0, 1, 4 })
    {
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported) == rowsOf(1, 4));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Prompt, Reported) == rowsOf(0, 0));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::PromptAndOutput, Reported)
              == rowsOf(0, 4));
    }

    // Anchored on them, it is the source that says whose they are -- for a terminal, as commandBlockAt() says
    // -- and this one names no block for them.
    for (auto const anchor: { 2, 3 })
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output, Reported)
              == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(bottomRowOfBlock(rows, CommandBlockId(1)) == LineOffset(4));
}

TEST_CASE("CommandBlockRows.aRowWithNoBlockOutsideABlocksRowsIsNoBlock", "[semanticblocks][blockrows]")
{
    // Below a block's last row -- between two blocks, or below the cursor while the block runs -- a row with
    // no block is no more the block's than a row the cursor has not reached yet.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow }, // 0: $ ls
                                     FakeRow { .block = 1, .flags = OutputRow }, // 1: a
                                     FakeRow { .block = 0, .flags = {} },        // 2: (erased)
                                     FakeRow { .block = 2, .flags = PromptRow }, // 3: $ make
                                     FakeRow { .block = 2, .flags = OutputRow }, // 4: building
                                     FakeRow { .block = 0, .flags = {} },        // 5: (not reached yet)
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::PromptAndOutput, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(5), CommandBlockPart::PromptAndOutput, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));

    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported) == rowsOf(1, 1));
    CHECK(commandBlockRows(rows, LineOffset(3), CommandBlockPart::PromptAndOutput, Reported) == rowsOf(3, 4));
}

TEST_CASE("CommandBlockRows.aBlockWhoseHeadWasEvictedStartsAtItsFirstStampedRow",
          "[semanticblocks][blockrows]")
{
    // The prompt left the history, and rows the command erased above the cursor (ED 1) scrolled up to the
    // top of it: they say nothing of whose they were, so the block starts at its first row that carries it.
    auto const rows = FakeRows { LineOffset(-3),
                                 {
                                     FakeRow { .block = 0, .flags = {} },        // -3: (erased)
                                     FakeRow { .block = 0, .flags = {} },        // -2: (erased)
                                     FakeRow { .block = 1, .flags = {} },        // -1: 8
                                     FakeRow { .block = 0, .flags = {} },        //  0: (erased)
                                     FakeRow { .block = 1, .flags = {} },        //  1: 9
                                     FakeRow { .block = 2, .flags = PromptRow }, //  2: $
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(-3), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(-2), CommandBlockPart::Output, Reported)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output, Reported) == rowsOf(-1, 1));
}

TEST_CASE("CommandBlockRows.whatTheHistoryKeepsOfAPromptIsStillAPrompt", "[semanticblocks][blockrows]")
{
    // The first line of a two-line prompt left the history; the line the command was typed on did not.
    auto const rows = FakeRows { LineOffset(-1),
                                 {
                                     FakeRow { .block = 1, .flags = {} },        // -1: $ make
                                     FakeRow { .block = 1, .flags = OutputRow }, //  0: built
                                     FakeRow { .block = 2, .flags = PromptRow }, //  1: $
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Prompt, Reported) == rowsOf(-1, -1));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, Reported) == rowsOf(0, 0));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::PromptAndOutput, Reported)
          == rowsOf(-1, 0));
}

TEST_CASE("CommandBlockRows.aBlockAtItsPromptIsAllPrompt", "[semanticblocks][blockrows]")
{
    // A prompt taller than the history: its head is gone, and no command has started.
    auto const rows = FakeRows { LineOffset(-1),
                                 {
                                     FakeRow { .block = 1, .flags = {} }, // -1: p2
                                     FakeRow { .block = 1, .flags = {} }, //  0: $
                                 } };
    auto const atPrompt = CommandProgress::AtPrompt;

    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output, atPrompt)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Prompt, atPrompt) == rowsOf(-1, 0));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::PromptAndOutput, atPrompt)
          == rowsOf(-1, 0));
}

TEST_CASE("CommandBlockRows.bottomRowOfBlockFindsTheLowestStampedRow", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(bottomRowOfBlock(rows, CommandBlockId(1)) == LineOffset(1));
    CHECK(bottomRowOfBlock(rows, CommandBlockId(2)) == LineOffset(3));
    CHECK(bottomRowOfBlock(rows, CommandBlockId(3)) == LineOffset(4));
    CHECK_FALSE(bottomRowOfBlock(rows, CommandBlockId(9)).has_value());
    CHECK_FALSE(bottomRowOfBlock(rows, CommandBlockId {}).has_value());
}

namespace
{

constexpr auto PromptEndRow = LineFlags { LineFlag::PromptEnd };

/// One case of inputRowOf(): rows from 0 down, row 0 the head of block 1, and the row it must answer.
struct InputRowCase
{
    std::string_view name;          ///< What the case shows.
    std::vector<FakeRow> rows;      ///< The rows, from the head down.
    std::optional<int> expectedRow; ///< The input row; nullopt for none.
};

/// A row of block @p block carrying @p flags.
[[nodiscard]] FakeRow rowOf(uint32_t block, LineFlags flags = {})
{
    return FakeRow { .block = block, .flags = flags };
}

[[nodiscard]] std::vector<InputRowCase> inputRowCases()
{
    // A prompt whose ;B lies one row past the scan budget.
    auto farBelow = std::vector<FakeRow> { rowOf(1, PromptRow) };
    for ([[maybe_unused]] auto const row: std::views::iota(0, static_cast<int>(MaxPromptScanLines)))
        farBelow.push_back(rowOf(1));
    farBelow.push_back(rowOf(1, PromptEndRow));

    return {
        InputRowCase { .name = "a one-line prompt: the head itself",
                       .rows = { rowOf(1, PromptRow | PromptEndRow), rowOf(1, OutputRow) },
                       .expectedRow = 0 },
        InputRowCase { .name = "a prompt over two lines: the line ;B marked",
                       .rows = { rowOf(1, PromptRow), rowOf(1, PromptEndRow), rowOf(1, OutputRow) },
                       .expectedRow = 1 },
        InputRowCase { .name = "no ;B at all: the head",
                       .rows = { rowOf(1, PromptRow), rowOf(1), rowOf(1, OutputRow) },
                       .expectedRow = 0 },
        InputRowCase { .name = "a ;B below the output start is no prompt's",
                       .rows = { rowOf(1, PromptRow), rowOf(1, OutputRow), rowOf(1, PromptEndRow) },
                       .expectedRow = 0 },
        InputRowCase { .name = "the head's own ;C ends nothing",
                       .rows = { rowOf(1, PromptRow | OutputRow), rowOf(1, PromptEndRow) },
                       .expectedRow = 1 },
        InputRowCase { .name = "a row with no block is passed over",
                       .rows = { rowOf(1, PromptRow), rowOf(0), rowOf(1, PromptEndRow) },
                       .expectedRow = 2 },
        InputRowCase { .name = "the next block ends the walk",
                       .rows = { rowOf(1, PromptRow), rowOf(2, PromptEndRow) },
                       .expectedRow = 0 },
        InputRowCase {
            .name = "a continuation's flags are not its own",
            .rows = { rowOf(1, PromptRow), rowOf(1, WrappedRow | PromptEndRow), rowOf(1, PromptEndRow) },
            .expectedRow = 2 },
        InputRowCase {
            .name = "a ;B beyond the scan budget is not looked for", .rows = farBelow, .expectedRow = 0 },
        InputRowCase { .name = "a head that is output: none",
                       .rows = { rowOf(1, OutputRow), rowOf(1, PromptEndRow) },
                       .expectedRow = std::nullopt },
        InputRowCase { .name = "a head with no mark at all: none",
                       .rows = { rowOf(1), rowOf(1, PromptEndRow) },
                       .expectedRow = std::nullopt },
    };
}

} // namespace

TEST_CASE("CommandBlockRows.inputRowOfFindsTheRowTheCommandWasTypedOn",
          "[semanticblocks][blockrows][stickyheader]")
{
    for (auto const& testCase: inputRowCases())
    {
        INFO(testCase.name);
        auto const rows = FakeRows { LineOffset(0), testCase.rows };
        auto const expected = testCase.expectedRow.transform([](int row) { return LineOffset(row); });
        CHECK(inputRowOf(rows, LineOffset(0), CommandBlockId(1)) == expected);
    }
}

TEST_CASE("CommandBlockRows.inputRowOfStopsAtTheBottomRow", "[semanticblocks][blockrows][stickyheader]")
{
    // A prompt drawn on the grid's last rows: the walk ends with the grid, never past it.
    auto const rows = FakeRows { LineOffset(-2), { rowOf(1, PromptRow), rowOf(1) } };
    CHECK(inputRowOf(rows, LineOffset(-2), CommandBlockId(1)) == LineOffset(-2));
}

TEST_CASE("CommandBlockRows.describeNamesEveryError", "[semanticblocks][blockrows]")
{
    STATIC_CHECK(!describe(CommandBlockActionError::NoBlock).empty());
    STATIC_CHECK(!describe(CommandBlockActionError::EmptyPart).empty());
}
