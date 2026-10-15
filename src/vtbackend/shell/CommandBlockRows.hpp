// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlockAddress.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace vtbackend
{

class Grid;

/// Why an action on a command block found nothing to act on.
enum class CommandBlockActionError : uint8_t
{
    NoBlock = 0, ///< The targeted row belongs to no block, or the block has left the grid.
    EmptyPart,   ///< The block exists but has none of the requested part (no output yet, prompt evicted).
};

/// A short, human-readable reason for @p error, for logs and status messages.
/// @param error The error.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(CommandBlockActionError error) noexcept
{
    switch (error)
    {
        case CommandBlockActionError::NoBlock: return "no command block there";
        case CommandBlockActionError::EmptyPart: return "the command block has none of the requested part";
    }
    return "unknown command block error";
}

/// The extent of one part of a command block: an inclusive range of grid rows (0 = page top, negative = into
/// the scrollback), cut where a command's end mark falls part-way into a line.
///
/// Whole lines, but at an end mark: an output ends before its command's mark, and a prompt the next ;A
/// started on the line a command ended on -- one whose output did not end its line -- starts at that mark.
/// The columns are logical ones, counted across the line's wraps from its head as Line::commandEndOffset()
/// counts them.
struct CommandBlockRows
{
    LineOffset first {};         ///< Topmost row.
    LineOffset last {};          ///< Bottommost row; never above @ref first.
    ColumnOffset firstColumn {}; ///< Where the part starts on the logical line @ref first heads.
    /// Where the part ends on the logical line that ends at @ref last: before this column, or at the end of
    /// the line when there is none.
    std::optional<ColumnOffset> endColumn {};

    bool operator==(CommandBlockRows const&) const = default;
};

/// One logical line of a block's part: its rows, and the logical columns of them the part takes.
struct CommandBlockLine
{
    LineOffset head {};                 ///< Its first row (the part's first row may continue a wrap).
    LineOffset last {};                 ///< Its last row within the part.
    ColumnOffset begin {};              ///< The logical column the part starts at on it.
    std::optional<ColumnOffset> end {}; ///< The logical column the part ends before on it; nullopt: its end.

    bool operator==(CommandBlockLine const&) const = default;
};

/// The logical lines of @p rows, top down: the one reader of a block's part, which the block actions and
/// the semantic-block query reply both go through, so they read the same lines from the same columns.
///
/// A line is read from its head through the rows a wrap continues it on, none below @p rows' last; the
/// first starts at the part's first column, the last ends before its end column. A last line an end mark
/// cuts, with nothing but blanks before the mark, is no line of the part: the command's output ended its
/// line, and the mark opened the next one.
/// @param grid The grid the rows are in.
/// @param rows The part.
/// @return Its lines, each to be read with Grid::logicalLineText(); none when the part is only such a line.
[[nodiscard]] std::vector<CommandBlockLine> logicalLinesOf(Grid const& grid, CommandBlockRows const& rows);

/// How far the command of the block a lookup names has got, as its record says: what its rows cannot tell.
///
/// It decides two things. A block whose command has not started is all prompt, whatever its rows say once
/// its head has left the history. And only a command that ended on an end mark of its own (OSC 133;D, or an
/// OSC 3008 end=) owns an end mark below its rows -- on the line the next prompt took -- as where its output
/// ends.
enum class CommandProgress : uint8_t
{
    Unreported = 0, ///< Still running, or closed by the next prompt without a mark of its own.
    AtPrompt,       ///< No command has started: the prompt is up.
    EndReported,    ///< Finished on its own end mark.
};

struct CommandBlockRecord;

/// How far the command of @p record has got, as commandBlockRows() reads it.
/// @param record The block.
/// @return AtPrompt while the prompt is up, EndReported once it finished on its own end mark, else
///         Unreported.
[[nodiscard]] CommandProgress progressOf(CommandBlockRecord const& record) noexcept;

/// The physical rows a block lookup walks, with the block id and the flags each one carries.
///
/// The seam that keeps commandBlockRows() a pure function: a terminal adapts its primary screen to it,
/// attributing each row as Terminal::commandBlockAt() does, and the tests a vector. Only rows in [topRow(),
/// bottomRow()] are ever asked about.
class CommandBlockRowSource
{
  public:
    CommandBlockRowSource() = default;
    CommandBlockRowSource(CommandBlockRowSource&&) = default;
    CommandBlockRowSource(CommandBlockRowSource const&) = default;
    CommandBlockRowSource& operator=(CommandBlockRowSource&&) = default;
    CommandBlockRowSource& operator=(CommandBlockRowSource const&) = default;
    virtual ~CommandBlockRowSource() = default;

    /// The topmost row that still holds valid data (Grid::addressableTop()).
    [[nodiscard]] virtual LineOffset topRow() const = 0;

    /// The bottommost row (the page's last one).
    [[nodiscard]] virtual LineOffset bottomRow() const = 0;

    /// The id of the block @p row belongs to; zero when it belongs to none.
    ///
    /// The one answer to "whose row is this": a terminal's source gives Terminal::commandBlockAt()'s, so a
    /// block action names the block the gutter and the tooltip show for the same row.
    [[nodiscard]] virtual CommandBlockId blockIdAt(LineOffset row) const = 0;

    /// The line flags of @p row.
    [[nodiscard]] virtual LineFlags flagsAt(LineOffset row) const = 0;

    /// Where the command's end mark stands on the logical line @p row heads (Line::commandEndOffset()): a
    /// logical column, meaningful only while flagsAt() carries LineFlag::CommandEnd.
    [[nodiscard]] virtual ColumnOffset commandEndColumnAt(LineOffset row) const = 0;
};

/// The rows of one part of the block that @p anchor belongs to.
///
/// The anchor's block is the one CommandBlockRowSource::blockIdAt() names for it; a row it names none for
/// anchors nothing. The block runs from its first row that carries its id to its last, through the rows
/// between them that carry none: an erase away from the cursor (ED 0 below it, ED 1 above it, DECSED,
/// DECALN) leaves rows so, even between rows of one block. Its prompt is its rows above its output: from its
/// head, the top row when that row carries LineFlag::Marked, or what the history still holds of it when the
/// head has been evicted. Its output starts at the first row below the head carrying LineFlag::OutputStart.
/// An OutputStart on the head row itself is ignored: a command that printed nothing leaves its ;C flag on the
/// row the next ;A claims. A block whose head has been evicted is output from its top row, unless an
/// OutputStart row survives or its output started on the line below its rows. A block whose command has not
/// started has no output.
///
/// The output, and with it PromptAndOutput, ends at the command's end mark (LineFlag::CommandEnd, from ;D or
/// an OSC 3008 end=): before its column on the line it is on, the end the semantic-block query reply reads;
/// the rows a finished block stamps after it, up to the next prompt, are not output. While there is no end
/// mark, the output runs to the block's last row. A command whose output did not end its line leaves its mark
/// on the line just below the block's rows, which the next prompt took: when the command reported that end,
/// the output runs on into that line, up to the mark -- its only line, when the output started there. The
/// next block's prompt starts at that mark in turn. A part splits a line nowhere else; trimming the blank
/// rows that end it is the caller's.
///
/// A lookup may walk to the top of the history, so it runs once per user action, never per frame.
/// @param rows The grid to walk.
/// @param anchor Any row of the block.
/// @param part Which part of the block to return.
/// @param progress How far the anchor's block has got, which only its record can say.
/// @return The extent, or NoBlock (anchor outside the grid or in no block) or EmptyPart (none of the part,
///         or nothing of it before the end mark).
[[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(
    CommandBlockRowSource const& rows, LineOffset anchor, CommandBlockPart part, CommandProgress progress);

/// The lowest row the source names @p id for, scanning upwards from the bottom of the grid.
/// @param rows The grid to walk.
/// @param id The block; zero never matches.
/// @return The row, or nullopt when no row carries @p id any more.
[[nodiscard]] std::optional<LineOffset> bottomRowOfBlock(CommandBlockRowSource const& rows,
                                                         CommandBlockId id);

/// The row holding what the user typed in block @p id, whose head is @p head: the row the sticky command
/// header copies.
///
/// The first logical line of the block's prompt carrying LineFlag::PromptEnd -- where OSC 133;B handed the
/// line to the user -- else the head itself, for a shell that never says where its prompt ends. Walks DOWN
/// from the head, at most MaxPromptScanLines rows, and stops at the first sign of output or of the next
/// block, so it costs the prompt's height and never the output's length: it runs once per frame.
///
/// A row carrying no block is passed over rather than taken for the next block's: an erase with the cursor
/// elsewhere leaves a row unlabelled until the cursor reaches it again, even inside one prompt. So @p rows
/// should name each row's block as its line was stamped (Line::blockId()), which costs nothing to read.
///
/// @param rows The rows to walk.
/// @param head The block's head row.
/// @param id The block.
/// @return The input row; nullopt when the head row is no row of a prompt -- neither LineFlag::Marked nor
///         LineFlag::PromptEnd -- because the block is headed at what the command printed: its command row
///         has left the grid, or the shell never drew a prompt for it.
[[nodiscard]] std::optional<LineOffset> inputRowOf(CommandBlockRowSource const& rows,
                                                   LineOffset head,
                                                   CommandBlockId id);

} // namespace vtbackend
