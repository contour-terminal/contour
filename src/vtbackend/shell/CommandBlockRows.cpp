// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockRows.hpp>

#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/PromptRegion.hpp>

#include <algorithm>
#include <ranges>

namespace vtbackend
{

namespace
{
    /// Which way a walk over the rows goes.
    enum class Direction : uint8_t
    {
        Up = 0,
        Down,
    };

    /// @p start and the rows beyond it in @p direction, nearest first, up to the edge of the grid.
    [[nodiscard]] auto rowsFrom(CommandBlockRowSource const& rows, LineOffset start, Direction direction)
    {
        auto const sign = direction == Direction::Up ? -1 : 1;
        auto const edge = direction == Direction::Up ? rows.topRow() : rows.bottomRow();
        auto const count = std::max(unbox(edge - start) * sign, -1) + 1;
        return std::views::iota(0, count) | std::views::transform([start, sign](int distance) {
                   return start + LineOffset(distance * sign);
               });
    }

    /// The farthest row carrying @p id that a walk from @p start, which carries it, reaches in @p direction:
    /// past rows that carry @p id or no block, up to a row of another block or the edge of the grid.
    [[nodiscard]] LineOffset farthestRowOf(CommandBlockRowSource const& rows,
                                           LineOffset start,
                                           CommandBlockId id,
                                           Direction direction)
    {
        auto farthest = start;
        for (auto const row: rowsFrom(rows, start, direction))
        {
            auto const rowId = rows.blockIdAt(row);
            if (rowId == id)
                farthest = row;
            else if (rowId != CommandBlockId {})
                break;
        }
        return farthest;
    }

    /// The first row in [@p from, @p to] carrying @p flag.
    [[nodiscard]] std::optional<LineOffset> firstRowWith(CommandBlockRowSource const& rows,
                                                         LineFlag flag,
                                                         LineOffset from,
                                                         LineOffset to)
    {
        for (auto const row: std::views::iota(unbox(from), unbox(to) + 1))
            if (rows.flagsAt(LineOffset(row)).test(flag))
                return LineOffset(row);
        return std::nullopt;
    }

    /// The last row of the logical line @p head starts: the rows a wrap continues it on, none below @p limit.
    [[nodiscard]] LineOffset lineEndOf(CommandBlockRowSource const& rows, LineOffset head, LineOffset limit)
    {
        auto end = head;
        for (auto const row: std::views::iota(unbox(head) + 1, unbox(limit) + 1))
        {
            if (!rows.flagsAt(LineOffset(row)).test(LineFlag::Wrapped))
                break;
            end = LineOffset(row);
        }
        return end;
    }

    /// The output that starts at @p outputStart and ends at the command's end mark (LineFlag::CommandEnd),
    /// which the logical line @p marked heads.
    ///
    /// It ends before the mark's column, which a wrap may carry onto the rows that continue the line; rows
    /// stamped below that line, up to the next prompt, are not output. Where nothing precedes the mark -- the
    /// output ended its line, so the mark opens the next one -- that line is none of the output's.
    /// @param rows The grid to walk.
    /// @param outputStart The output's first row.
    /// @param marked The head of the line the mark is on; not above @p outputStart.
    /// @param limit The lowest row the marked line may run on to.
    /// @return The extent, or nullopt when nothing of the output precedes the mark.
    [[nodiscard]] std::optional<CommandBlockRows> outputEndingAtMark(CommandBlockRowSource const& rows,
                                                                     LineOffset outputStart,
                                                                     LineOffset marked,
                                                                     LineOffset limit)
    {
        if (auto const column = rows.commandEndColumnAt(marked); column != ColumnOffset(0))
            return CommandBlockRows { .first = outputStart,
                                      .last = lineEndOf(rows, marked, limit),
                                      .endColumn = column };
        if (marked == outputStart)
            return std::nullopt;
        return CommandBlockRows { .first = outputStart, .last = marked - LineOffset(1) };
    }
} // namespace

CommandProgress progressOf(CommandBlockRecord const& record) noexcept
{
    if (record.state == CommandBlockState::Prompting)
        return CommandProgress::AtPrompt;
    return record.state == CommandBlockState::Finished && record.end == CommandBlockEnd::Reported
               ? CommandProgress::EndReported
               : CommandProgress::Unreported;
}

std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(CommandBlockRowSource const& rows,
                                                                          LineOffset anchor,
                                                                          CommandBlockPart part,
                                                                          CommandProgress progress)
{
    if (anchor < rows.topRow() || anchor > rows.bottomRow())
        return std::unexpected(CommandBlockActionError::NoBlock);

    // The anchor's block is the one the source names for it, and nothing else: guessing one for a row that
    // carries none would make an action name a block the gutter does not show there.
    auto const id = rows.blockIdAt(anchor);
    if (id == CommandBlockId {})
        return std::unexpected(CommandBlockActionError::NoBlock);

    auto const block = CommandBlockRows { .first = farthestRowOf(rows, anchor, id, Direction::Up),
                                          .last = farthestRowOf(rows, anchor, id, Direction::Down) };
    auto const headSurvives = rows.flagsAt(block.first).test(LineFlag::Marked);

    // A command whose output did not end its line left its end mark on the line below the block's rows, which
    // the next prompt then took. Only a mark the command reported is its own -- a ;D with nothing running
    // marks a line too -- so the record decides.
    auto const below = block.last + LineOffset(1);
    auto const endsBelow = progress == CommandProgress::EndReported && below <= rows.bottomRow()
                           && rows.flagsAt(below).test(LineFlag::CommandEnd);
    auto const outputStartsBelow = endsBelow && rows.flagsAt(below).test(LineFlag::OutputStart);

    // An OutputStart on a surviving head is the block before's. Without one below it, the output starts on
    // the line below the block's rows, or nowhere; a block whose head was evicted is output from its top row.
    // A block whose command has not started has none, whatever its rows say once its head is gone.
    auto const outputStart = [&]() -> std::optional<LineOffset> {
        if (progress == CommandProgress::AtPrompt)
            return std::nullopt;
        auto const from = headSurvives ? block.first + LineOffset(1) : block.first;
        if (auto const marked = firstRowWith(rows, LineFlag::OutputStart, from, block.last))
            return marked;
        if (headSurvives || outputStartsBelow)
            return std::nullopt;
        return block.first;
    }();

    auto const output = [&]() -> std::optional<CommandBlockRows> {
        if (!outputStart)
            return outputStartsBelow ? outputEndingAtMark(rows, below, below, rows.bottomRow())
                                     : std::nullopt;
        if (auto const marked = firstRowWith(rows, LineFlag::CommandEnd, *outputStart, block.last))
            return outputEndingAtMark(rows, *outputStart, *marked, block.last);
        if (endsBelow)
            return outputEndingAtMark(rows, *outputStart, below, rows.bottomRow());
        return CommandBlockRows { .first = *outputStart, .last = block.last };
    }();

    // The rows above the output, whether the head survives or the history has kept only the rest of them.
    // The block before may have ended part-way into the top line, where this prompt then started.
    auto const prompt = [&]() -> std::optional<CommandBlockRows> {
        if (outputStart == block.first)
            return std::nullopt;
        return CommandBlockRows {
            .first = block.first,
            .last = outputStart ? *outputStart - LineOffset(1) : block.last,
            .firstColumn = rows.flagsAt(block.first).test(LineFlag::CommandEnd)
                               ? rows.commandEndColumnAt(block.first)
                               : ColumnOffset(0),
        };
    }();

    auto const either = [](std::optional<CommandBlockRows> const& extent)
        -> std::expected<CommandBlockRows, CommandBlockActionError> {
        if (!extent)
            return std::unexpected(CommandBlockActionError::EmptyPart);
        return *extent;
    };
    switch (part)
    {
        case CommandBlockPart::Output: return either(output);
        case CommandBlockPart::Prompt: return either(prompt);
        case CommandBlockPart::PromptAndOutput:
            if (!prompt || !output)
                return either(prompt ? prompt : output);
            return CommandBlockRows { .first = prompt->first,
                                      .last = output->last,
                                      .firstColumn = prompt->firstColumn,
                                      .endColumn = output->endColumn };
    }
    return std::unexpected(CommandBlockActionError::NoBlock);
}

std::vector<CommandBlockLine> logicalLinesOf(Grid const& grid, CommandBlockRows const& rows)
{
    auto lines = std::vector<CommandBlockLine> {};
    auto head = rows.first;
    while (head <= rows.last)
    {
        auto const last = grid.logicalLineEnd(head, rows.last);
        lines.push_back(CommandBlockLine { .head = head,
                                           .last = last,
                                           .begin = head == rows.first ? rows.firstColumn : ColumnOffset(0),
                                           .end = last == rows.last ? rows.endColumn : std::nullopt });
        head = last + LineOffset(1);
    }

    // An end mark with nothing but blanks before it on its line opened that line: the output ended the one
    // above.
    if (auto const& cut = lines.back();
        cut.end && grid.isLogicalLineBlank(cut.head, cut.last, cut.begin, cut.end))
        lines.pop_back();
    return lines;
}

std::optional<LineOffset> bottomRowOfBlock(CommandBlockRowSource const& rows, CommandBlockId id)
{
    if (id == CommandBlockId {})
        return std::nullopt;

    for (auto const row: rowsFrom(rows, rows.bottomRow(), Direction::Up))
        if (rows.blockIdAt(row) == id)
            return row;
    return std::nullopt;
}

std::optional<LineOffset> inputRowOf(CommandBlockRowSource const& rows, LineOffset head, CommandBlockId id)
{
    // A reflow heads a block whose prompt row scrolled out at the row its output started on, and a block
    // with no mark left at its topmost row: either way the head is output.
    if (!rows.flagsAt(head).any(LineFlags { LineFlag::Marked, LineFlag::PromptEnd }))
        return std::nullopt;

    for (auto const row:
         rowsFrom(rows, head, Direction::Down) | std::views::take(static_cast<int>(MaxPromptScanLines)))
    {
        auto const blockId = rows.blockIdAt(row);
        if (blockId == CommandBlockId {})
            continue;
        if (blockId != id)
            break;

        auto const flags = rows.flagsAt(row);
        // A continuation carries no marks of its own: they live on its logical line's head.
        if (flags.test(LineFlag::Wrapped))
            continue;

        if (flags.test(LineFlag::PromptEnd))
            return row;

        // The head may carry the ;C itself, from a shell that ran the command without saying where its prompt
        // ended; below it, an output start ends the prompt.
        if (row != head && flags.test(LineFlag::OutputStart))
            break;
    }
    return head;
}

} // namespace vtbackend
