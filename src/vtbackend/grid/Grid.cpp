// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Grid.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/WordWalk.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

using std::max;
using std::min;
using std::u32string;
using std::u32string_view;
using std::vector;

namespace vtbackend
{

auto inline const gridLog = core::log::Category(
    "vt.grid", "Grid related", core::log::Category::State::Disabled, core::log::Category::Visibility::Hidden);

namespace detail
{
    static Lines createLines(PageSize pageSize,
                             LineCount maxHistoryLineCount,
                             bool reflowOnResize,
                             GraphicsAttributes initialSGR)
    {
        auto const defaultLineFlags = reflowOnResize ? LineFlag::Wrappable : LineFlag::None;
        auto const totalLineCount = unbox<size_t>(pageSize.lines + maxHistoryLineCount);

        Lines lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: std::views::iota(0u, totalLineCount))
            lines.emplace_back(pageSize.columns, defaultLineFlags, initialSGR);

        return lines;
    }

    /// Internal linkage, matching the `static` helpers around it: this is reflow's own vocabulary and
    /// nothing outside this translation unit names it.
    namespace
    {
        /// Everything a logical line carries that survives being re-chopped into physical rows.
        ///
        /// One parameter rather than four, and one local rather than four kept in step by hand: reflow
        /// assigns these together, captures them together and passes them together, so a further attribute
        /// is a member here (or in LineProvenance) instead of an edit at every site. It also un-swaps the
        /// two ColumnOffsets, which as adjacent parameters of one type could be exchanged at any call site
        /// without a diagnostic.
        struct LogicalLineAttributes
        {
            LineFlags flags = LineFlag::None;

            /// An offset into the LOGICAL line, carried by its head alone. Re-chopping the line moves it
            /// with the cell it names (ReflowedCursors::startLine), past any padding a row gains or loses.
            ColumnOffset commandEndOffset {};

            /// Likewise head-only.
            /// @see commandEndOffset
            ColumnOffset promptEndOffset {};

            /// NOT head-only, unlike the two above.
            LineProvenance provenance {};
        };

        /// What a logical line takes from @p first, the first of its rows that reflow sees.
        ///
        /// That row is normally the head, and the logical line takes everything from it. It is a continuation
        /// only when the head is gone -- evicted off the top of the history, or erased to a blank row that is
        /// emitted on its own -- and then the logical line takes what the continuation itself carries, minus
        /// what only a head may: defaults, or whatever an unrelated earlier line left behind, would be wrong.
        /// @param first The first row of the logical line.
        /// @return What every physical piece of the logical line is cut with.
        [[nodiscard]] LogicalLineAttributes logicalLineAttributesFrom(Line const& first) noexcept
        {
            if (first.wrapped())
                return LogicalLineAttributes {
                    .flags = first.flags().without(LineFlag::Wrapped).without(HeadOnlyLineFlags),
                    .provenance = first.provenance(),
                };
            return LogicalLineAttributes {
                .flags = first.flags(),
                .commandEndOffset = first.commandEndOffset(),
                .promptEndOffset = first.promptEndOffset(),
                .provenance = first.provenance(),
            };
        }

        /// Where the row a logical line is cut into ends when it starts at @p start of @p text, whose first
        /// @p textLength cells count, and holds @p rowWidth cells: a full row, but for a wide character the
        /// edge would cut in two, which goes on to the next row whole -- as foot moves it (grid.c:1024) and
        /// kitty does (find_space_in_dest_line, resize.c:180).
        [[nodiscard]] size_t rowEnd(LineSoA const& text,
                                    size_t start,
                                    size_t textLength,
                                    size_t rowWidth) noexcept
        {
            auto const end = std::min(start + rowWidth, textLength);
            return end < textLength ? wideCharacterSafeEnd(text, start, end) : end;
        }

        /// Whether a row's logical line goes on in the row below it.
        enum class LineGoesOn : uint8_t
        {
            No,
            Yes,
        };

        /// The cells of @p text up to its first column of trailing CellFlag::WrapPadding: the cells a wide
        /// character left when it moved on to the next row, which are not part of the text.
        [[nodiscard]] size_t trimWrapPaddingRight(LineSoA const& text, size_t columns) noexcept
        {
            // A blank row stores no cells, and so no padding either.
            if (text.codepoints.size() < columns)
                return columns;
            auto end = columns;
            while (end > 0 && isWrapPadding(text, end - 1))
                --end;
            return end;
        }

        /// How many of @p row's cells a reflow carries into its logical line. Only the blanks that END a
        /// logical line may go: a row whose line goes on below gives all of its cells, blank or not, but
        /// for the padding a wide character left at its end, and any other row its cells up to the last
        /// one that is not blank.
        [[nodiscard]] size_t carriedColumns(Line const& row, LineGoesOn goesOn) noexcept
        {
            auto const columns = unbox<size_t>(row.size());
            return goesOn == LineGoesOn::Yes ? trimWrapPaddingRight(row.storage(), columns)
                                             : trimBlankRightKeepingWideCharacters(row.storage(), columns);
        }

        /// Copies the first @p count cells of @p source to @p target at @p at. Cells @p source does not
        /// store -- all of them, for a row that is blank -- stay as they are in @p target: blank.
        void copyCarriedColumns(LineSoA const& source, LineSoA& target, size_t at, size_t count)
        {
            auto const stored = std::min(count, source.codepoints.size());
            if (stored > 0)
                copyColumns(source, 0, target, at, stored);
        }

        /// Makes @p text, of which @p used cells count, count at least @p columns cells, blank where it
        /// had none.
        void extendWithBlanks(LineSoA& text, size_t& used, size_t columns)
        {
            if (columns <= used)
                return;
            used = columns;
            if (text.codepoints.size() < used)
                resizeLineSoA(text, ColumnCount::cast_from(used));
        }

        /// Whether @p row is blank and nothing ever reached it: no text, no fill, no mark, no provenance.
        /// Such a row can go and come back without anything noticing.
        [[nodiscard]] bool isUntouchedBlank(Line const& row) noexcept
        {
            return row.isBlankWithFillAttrs(GraphicsAttributes {}) && !row.flags().any(HeadOnlyLineFlags)
                   && row.provenance() == LineProvenance {};
        }

        /// Drops the untouched blank rows that end @p lines, keeping at least @p keep rows.
        /// @return How many rows it dropped.
        [[nodiscard]] LineCount dropTrailingUntouchedBlanks(Lines& lines, size_t keep)
        {
            auto dropped = LineCount(0);
            while (lines.size() > keep && isUntouchedBlank(lines.back()))
            {
                lines.storage().pop_back();
                ++dropped;
            }
            return dropped;
        }

        /// Where the cell at @p offset of a row goes when the cells [@p column, @p column + @p width) are
        /// taken out of the row and the text after them closes up: one of those cells to where they began.
        [[nodiscard]] constexpr size_t closedUp(size_t offset, size_t column, size_t width) noexcept
        {
            if (offset < column)
                return offset;
            return offset < column + width ? column : offset - width;
        }

        /// What a narrowing does with a row's cells past the new width.
        enum class NarrowedRow : uint8_t
        {
            Cut = 0,  ///< The row does not reflow: they are cut off.
            Reflowed, ///< They carry on into the rows below, with the rest of the row's logical line.
        };

        /// What a point a reflow follows does when it lies past the end of its line's text.
        enum class PastTheText : uint8_t
        {
            /// It is counted on from the end of the text, which stays as it is: a column a line marks.
            Counted,
            /// The text is made to reach it, the line gaining the row it is on: a cursor.
            Reached,
        };

        /// A column a logical line's head marks on its line, counted as Grid::logicalColumnOf() counts --
        /// the rows above it times the page width, plus its column -- and the flag that says the head
        /// marks it.
        struct HeadOffset
        {
            LineFlag flag;                              ///< Set on a head that marks the column.
            ColumnOffset (Line::*get)() const noexcept; ///< Reads the column off the head.
            void (Line::*set)(ColumnOffset) noexcept;   ///< Writes it.
        };

        /// Every column a head marks on its line. Counted in rows of the page's width, each moves with the
        /// cell it names when the line's rows change: a reflow follows it as it follows the cursor.
        constexpr auto HeadOffsets = std::array {
            HeadOffset { .flag = LineFlag::PromptEnd,
                         .get = &Line::promptEndOffset,
                         .set = &Line::setPromptEndOffset },
            HeadOffset { .flag = LineFlag::CommandEnd,
                         .get = &Line::commandEndOffset,
                         .set = &Line::setCommandEndOffset },
        };

        /// Moves each column @p head marks on its line to where @p move takes it.
        template <typename Move>
        void moveHeadOffsets(Line& head, Move const& move)
        {
            for (auto const& offset: HeadOffsets)
                if (head.isFlagEnabled(offset.flag))
                    std::invoke(offset.set, head, move(std::invoke(offset.get, head)));
        }

        /// The row of [@p top, @p i] that heads the logical line row @p i is in, unless that head is above
        /// @p top.
        [[nodiscard]] std::optional<int> headOf(Lines const& lines, int top, int i) noexcept
        {
            while (i > top && lines[i].wrapped())
                --i;
            return lines[i].wrapped() ? std::nullopt : std::optional { i };
        }

        /// Follows the cursor's cell through a reflow, as foot does with its tracking points: the cursor
        /// keeps its logical line and its offset into that line.
        ///
        /// While the rows are re-chopped, the cursor is an offset into the text being cut -- the logical
        /// line a widening collects, or the overflow a narrowing carries -- until a cut places it on a
        /// row of the rebuilt lines. Where the offset lies past the text, the text is made to reach it,
        /// so the line gains the row the cursor stands on instead of losing it. Any other cell is
        /// followed the same way, but for what lies past the text (PastTheText).
        class ReflowedPoint
        {
          public:
            /// @param row The cursor's row on the page before the reflow.
            /// @param offset The cursor's offset into that row, a pending wrap counted one past its cell.
            /// @param pastTheText What the point does where it lies past the text.
            ReflowedPoint(LineOffset row, size_t offset, PastTheText pastTheText) noexcept:
                _row { row }, _offset { offset }, _pastTheText { pastTheText }
            {
            }

            /// The source row @p i is kept as it is, cut or padded to @p columns cells, at index @p index of
            /// the rebuilt lines. A cursor on it keeps its column, held to the last one.
            void keepRow(int i, size_t index, size_t columns) noexcept
            {
                if (i == *_row)
                    _rebuilt = Rebuilt { .row = index, .column = std::min(_offset, columns - 1) };
            }

            /// A widening is about to append the source row @p i to the logical line it collects.
            /// @param used       How many cells of that logical line are collected so far.
            /// @param rowsBefore The cells the rows already appended had, blanks included.
            void joinRow(int i, Line const& row, size_t used, size_t rowsBefore) noexcept
            {
                if (i != *_row)
                    return;
                // On its text, the cursor goes where its cell goes. Past it, it keeps its offset as the
                // rows held it, rather than the one left once their trailing blanks are dropped: a
                // narrowing that put the cursor on a row of its own comes back to where it was.
                auto const textEnd =
                    trimBlankRightKeepingWideCharacters(row.storage(), unbox<size_t>(row.size()));
                _pending = _offset < textEnd ? used + _offset : rowsBefore + _offset;
            }

            /// Makes @p text, of which @p used cells count, reach the cursor's cell if it is in it.
            void reach(LineSoA& text, size_t& used) const
            {
                if (_pending && _pastTheText == PastTheText::Reached)
                    extendWithBlanks(text, used, *_pending + 1);
            }

            /// The text the cursor is in is cut into rows of @p columns cells, the first at @p firstRow.
            /// @param text The text, of which @p used cells count.
            void cut(size_t firstRow, size_t columns, LineSoA const& text, size_t used) noexcept
            {
                if (!_pending)
                    return;
                auto row = firstRow;
                auto start = size_t { 0 };
                auto end = rowEnd(text, start, used, columns);
                while (*_pending >= end && end < used)
                {
                    start = end;
                    ++row;
                    end = rowEnd(text, start, used, columns);
                }
                _rebuilt = Rebuilt { .row = row, .column = *_pending - start };
                _pending.reset();
            }

            /// A narrowing reaches the source row @p i.
            void enterRow(int i) noexcept
            {
                _inRow = i == *_row ? std::optional<size_t> { _offset } : std::nullopt;
            }

            /// A narrowing prepends the @p overflowUsed cells of overflow it carries to the current row.
            void prependOverflow(size_t overflowUsed) noexcept
            {
                if (_pending)
                    _inRow = std::exchange(_pending, std::nullopt);
                else if (_inRow)
                    *_inRow += overflowUsed;
            }

            /// A narrowing cut the current row to @p columns cells, at index @p index of the rebuilt lines,
            /// and carries what it cut off in @p overflow, of which @p overflowUsed cells count.
            /// @param narrowed Whether the row reflows, or is cut instead.
            /// @param kept How many of the row's cells it kept: @p columns, or fewer where a wide character
            ///             the edge would have cut went with the overflow.
            void cutRow(size_t index,
                        size_t columns,
                        size_t kept,
                        NarrowedRow narrowed,
                        LineSoA& overflow,
                        size_t& overflowUsed)
            {
                if (!_inRow)
                    return;
                if (*_inRow < kept || narrowed == NarrowedRow::Cut)
                    // A row that does not reflow keeps the cursor, on its last column if need be.
                    _rebuilt = Rebuilt { .row = index, .column = std::min(*_inRow, columns - 1) };
                else if (_pastTheText == PastTheText::Counted && overflowUsed == 0)
                    // Past the text, none of which the cut carries on: the point stays counted from this row.
                    _rebuilt = Rebuilt { .row = index, .column = *_inRow };
                else
                {
                    // The cursor's cell went with the overflow -- or, past the text, would have.
                    _pending = *_inRow - kept;
                    reach(overflow, overflowUsed);
                }
                _inRow.reset();
            }

            /// The cells from @p column on, @p width of them, are taken out of the source row @p i before the
            /// reflow, and the text after them closes up. A cursor on them stays where they began.
            void closeGap(int i, size_t column, size_t width) noexcept
            {
                if (i == *_row)
                    _offset = closedUp(_offset, column, width);
            }

            /// A row was inserted into the rebuilt lines at index @p index, moving those from there on down.
            void insertRow(size_t index) noexcept
            {
                if (_rebuilt && _rebuilt->row >= index)
                    ++_rebuilt->row;
            }

            /// A row of the rebuilt lines, counted from the oldest history row, and a column in it.
            struct Rebuilt
            {
                size_t row = 0;
                size_t column = 0;
            };

            /// Where in the rebuilt lines the cursor is, if a row has taken it yet.
            [[nodiscard]] std::optional<Rebuilt> rebuilt() const noexcept { return _rebuilt; }

            /// The row of the rebuilt lines the cursor is on, if a row has taken it yet.
            [[nodiscard]] std::optional<size_t> row() const noexcept
            {
                return _rebuilt ? std::optional<size_t> { _rebuilt->row } : std::nullopt;
            }

            /// The cursor on the page once the rebuilt lines hold @p history rows of history, or
            /// @p fallback if no row held it. A cursor whose text went into the history stays on the
            /// page's top row.
            [[nodiscard]] CellLocation onPage(LineCount history, CellLocation fallback) const noexcept
            {
                if (!_rebuilt)
                    return fallback;
                return CellLocation { .line = std::max(LineOffset(0),
                                                       LineOffset::cast_from(_rebuilt->row)
                                                           - boxed_cast<LineOffset>(history)),
                                      .column = ColumnOffset::cast_from(_rebuilt->column) };
            }

          private:
            LineOffset _row;
            size_t _offset;
            PastTheText _pastTheText;
            std::optional<size_t> _inRow;   ///< The cursor's offset into the row a narrowing is at.
            std::optional<size_t> _pending; ///< The cursor's offset into the text not yet cut.
            std::optional<Rebuilt> _rebuilt;
        };

        /// Every cursor a reflow carries along -- the live one, and the one DECSC saved -- each followed
        /// as a ReflowedPoint, whose operations this repeats for all of them. Beside them, the anchors:
        /// cells followed the same way to learn where they went, such as the head of a block that is
        /// taller than one row. And while the reflow is at a logical line, the columns its head marks
        /// on it.
        class ReflowedCursors
        {
          public:
            /// @param cursors Where the next character goes from each cursor before the reflow.
            explicit ReflowedCursors(std::span<CellLocation const> cursors): _cursorCount { cursors.size() }
            {
                _points.reserve(cursors.size());
                for (auto const& cursor: cursors)
                    _points.emplace_back(cursor.line, unbox<size_t>(cursor.column), PastTheText::Reached);
            }

            /// Follows the cell at @p row and @p column too, as the anchor numbered by how many were
            /// followed before it. Anchors are all followed before any line is.
            void anchor(LineOffset row, size_t column)
            {
                Require(!_line);
                _points.emplace_back(row, column, PastTheText::Counted);
            }

            /// Where in the rebuilt lines the anchor numbered @p index went, if a row has taken it.
            [[nodiscard]] std::optional<ReflowedPoint::Rebuilt> anchorAt(size_t index) const noexcept
            {
                return _points.at(_cursorCount + index).rebuilt();
            }

            /// The reflow reaches the source row @p i, @p row, whose rebuilt rows go to @p rebuilt from
            /// its end on, every row of the lines before it there already. If @p row heads a logical line,
            /// the columns the line followed before marks are written into its head (placeLine()), and
            /// those @p row marks are followed until the next.
            /// @param width How wide the rows are before the reflow, and after it.
            void startLine(Lines& rebuilt, int i, Line const& row, WidthChange width)
            {
                if (row.wrapped())
                    return;
                placeLine(rebuilt, unbox<size_t>(width.to));
                followLine(i, row, rebuilt.size(), unbox<size_t>(width.from));
            }

            /// Writes into the head of the line followed, in @p rebuilt, whose rows are @p columns wide,
            /// where each column it marks went, and stops following them. A column no row took keeps its
            /// value, as do they all if the line left no row.
            void placeLine(Lines& rebuilt, size_t columns)
            {
                if (_line && _line->headRow < rebuilt.size())
                {
                    auto& head = rebuilt.storage()[_line->headRow];
                    for (auto const [index, offset]: core::views::enumerate(HeadOffsets))
                    {
                        auto const& point = _line->points.at(static_cast<size_t>(index));
                        if (auto const at = point ? _points[*point].rebuilt() : std::nullopt)
                        {
                            Require(at->row >= _line->headRow);
                            std::invoke(
                                offset.set,
                                head,
                                ColumnOffset::cast_from(((at->row - _line->headRow) * columns) + at->column));
                        }
                    }
                }
                if (_line)
                    _points.erase(_points.begin() + static_cast<std::ptrdiff_t>(_line->firstPoint),
                                  _points.end());
                _line.reset();
            }

            /// @see ReflowedPoint::closeGap
            void closeGap(int i, size_t column, size_t width) noexcept
            {
                for (auto& point: _points)
                    point.closeGap(i, column, width);
            }

            /// @see ReflowedPoint::insertRow
            void insertRow(size_t index) noexcept
            {
                for (auto& point: _points)
                    point.insertRow(index);
            }

            /// @see ReflowedPoint::keepRow
            void keepRow(int i, size_t index, size_t columns) noexcept
            {
                for (auto& point: _points)
                    point.keepRow(i, index, columns);
            }

            /// @see ReflowedPoint::joinRow
            void joinRow(int i, Line const& row, size_t used, size_t rowsBefore) noexcept
            {
                for (auto& point: _points)
                    point.joinRow(i, row, used, rowsBefore);
            }

            /// @see ReflowedPoint::reach
            void reach(LineSoA& text, size_t& used) const
            {
                for (auto const& point: _points)
                    point.reach(text, used);
            }

            /// @see ReflowedPoint::cut
            void cut(size_t firstRow, size_t columns, LineSoA const& text, size_t used) noexcept
            {
                for (auto& point: _points)
                    point.cut(firstRow, columns, text, used);
            }

            /// @see ReflowedPoint::enterRow
            void enterRow(int i) noexcept
            {
                for (auto& point: _points)
                    point.enterRow(i);
            }

            /// @see ReflowedPoint::prependOverflow
            void prependOverflow(size_t overflowUsed) noexcept
            {
                for (auto& point: _points)
                    point.prependOverflow(overflowUsed);
            }

            /// @see ReflowedPoint::cutRow
            void cutRow(size_t index,
                        size_t columns,
                        size_t kept,
                        NarrowedRow narrowed,
                        LineSoA& overflow,
                        size_t& overflowUsed)
            {
                for (auto& point: _points)
                    point.cutRow(index, columns, kept, narrowed, overflow, overflowUsed);
            }

            /// The lowest row of the rebuilt lines a cursor is on, if a row has taken one yet.
            [[nodiscard]] std::optional<size_t> lowestRow() const noexcept
            {
                auto lowest = std::optional<size_t> {};
                for (auto const& point: _points | std::views::take(_cursorCount))
                    if (auto const row = point.row(); row && (!lowest || *row > *lowest))
                        lowest = row;
                return lowest;
            }

            /// Writes each cursor's place on the page into @p cursors, once the rebuilt lines hold
            /// @p history rows of history. A cursor no row took keeps its line and is held to the
            /// @p columns of the page.
            void place(LineCount history, std::span<CellLocation> cursors, ColumnCount columns) const
            {
                Require(cursors.size() == _cursorCount);
                for (auto const i: std::views::iota(size_t { 0 }, _cursorCount))
                {
                    auto const clamped = CellLocation {
                        .line = cursors[i].line,
                        .column = std::min(cursors[i].column, boxed_cast<ColumnOffset>(columns) - 1),
                    };
                    cursors[i] = _points[i].onPage(history, clamped);
                }
            }

          private:
            /// A logical line whose columns are followed.
            struct FollowedLine
            {
                size_t headRow = 0;    ///< Where in the rebuilt lines its head goes.
                size_t firstPoint = 0; ///< Where its points begin among the points followed.
                /// Which point follows each of HeadOffsets, for a column the head marks.
                std::array<std::optional<size_t>, HeadOffsets.size()> points {};
            };

            /// Follows each column @p head, the source row @p i, marks on its logical line, the head going
            /// to @p headRow of the rebuilt lines. A column naming a row the line does not have is not
            /// reached before the line is placed, and keeps its value.
            /// @param columns How wide the line's rows are before the reflow.
            void followLine(int i, Line const& head, size_t headRow, size_t columns)
            {
                auto line = FollowedLine { .headRow = headRow, .firstPoint = _points.size() };
                for (auto const [index, offset]: core::views::enumerate(HeadOffsets))
                {
                    if (!head.isFlagEnabled(offset.flag))
                        continue;
                    auto const column = unbox<size_t>(std::invoke(offset.get, head));
                    line.points.at(static_cast<size_t>(index)) = _points.size();
                    auto& point =
                        _points.emplace_back(LineOffset::cast_from(i + static_cast<int>(column / columns)),
                                             column % columns,
                                             PastTheText::Counted);
                    point.enterRow(i);
                }
                _line = line;
            }

            size_t _cursorCount;                ///< How many of the points are cursors; the rest are anchors.
            std::vector<ReflowedPoint> _points; ///< The cursors, then the anchors, then the line's columns.
            std::optional<FollowedLine> _line {}; ///< The line whose columns are followed, if one is.
        };

        /// A text-sizing block taller than one row (OSC 66 `s=2` and up) that a reflow carries: its head
        /// moves with the text as a wide character does, while its part on the rows below is lifted out of
        /// them first and put back below the head wherever the head went, as kitty's rewrap copies those
        /// rows along with the head (copy_multiline_extra_lines, resize.c:206).
        struct TallBlock
        {
            LineOffset row;                    ///< The head's row before the reflow.
            size_t column = 0;                 ///< The head's column before the reflow.
            size_t columns = 0;                ///< How many columns the block spans.
            size_t rows = 0;                   ///< How many rows it spans, its head's included.
            std::vector<LineSoA> rowsBelow {}; ///< Its cells on each row below the head, once lifted.
        };

        /// The cell attributes of padding: cells that are no part of the text, which a reflow drops where
        /// they end a row whose line goes on.
        [[nodiscard]] GraphicsAttributes padding() noexcept
        {
            return GraphicsAttributes {}.with(CellFlag::WrapPadding);
        }

        /// Whether the cell at @p column of @p cells continues a block -- a wide character, or one laid out
        /// by OSC 66 -- from its left or from a row above, rather than being a cell of its own or a block's
        /// head.
        [[nodiscard]] bool continuesBlock(LineSoA const& cells, size_t column) noexcept
        {
            return cells.sgr[column].flags.contains(CellFlag::WideCharContinuation);
        }

        /// Every block taller than one row whose head is on one of the rows [@p top, @p bottom).
        [[nodiscard]] std::vector<TallBlock> findTallBlocks(Lines const& lines, int top, int bottom)
        {
            auto blocks = std::vector<TallBlock> {};
            for (auto const i: std::views::iota(top, bottom))
            {
                if (lines[i].isTrivialBuffer()) // which a row holding any part of a block never is
                    continue;
                auto const& cells = lines[i].storage();
                for (auto const column: std::views::iota(size_t { 0 }, cells.codepoints.size()))
                    if (cells.scales[column] > 1 && !continuesBlock(cells, column))
                        blocks.push_back(TallBlock { .row = LineOffset::cast_from(i),
                                                     .column = column,
                                                     .columns = cells.widths[column],
                                                     .rows = cells.scales[column],
                                                     .rowsBelow = {} });
            }
            return blocks;
        }

        /// Takes each block's part on the rows below its head out of those rows, leaving padding, for
        /// placeTallBlocks() to put back below the head. A part that is not there any more -- below the
        /// rows reflowed, or written over -- ends what is lifted of its block.
        void liftTallBlocks(Lines& lines, std::span<TallBlock> blocks, int bottom)
        {
            for (auto& block: blocks)
            {
                for (auto const below: std::views::iota(1, static_cast<int>(block.rows)))
                {
                    auto const i = unbox<int>(block.row) + below;
                    if (i >= bottom || lines[i].isBlank())
                        break;
                    auto const& source = std::as_const(lines[i]).storage();
                    if (block.column + block.columns > source.codepoints.size()
                        || !source.sgr[block.column].flags.contains(CellFlag::MulticellContinuation))
                        break;
                    auto& cells = lines[i].storage();
                    auto& part = block.rowsBelow.emplace_back();
                    initializeLineSoA(part, ColumnCount::cast_from(block.columns));
                    copyColumns(cells, block.column, part, 0, block.columns);
                    clearRange(cells, block.column, block.columns, padding());
                }
            }
        }

        /// The cells [@p column, @p column + @p width) of the row @p i of [@p top, ...), @p columns wide, are
        /// taken out of it, the text after them closing up: each column the head of its logical line marks
        /// on that row moves with the cell it names, as a cursor does (ReflowedPoint::closeGap).
        void closeGapInHeadOffsets(Lines& lines, int top, int i, size_t column, size_t width, size_t columns)
        {
            auto const head = headOf(lines, top, i);
            if (!head)
                return;
            auto const rowStart = static_cast<size_t>(i - *head) * columns;
            moveHeadOffsets(lines[*head], [&](ColumnOffset offset) {
                auto const at = unbox<size_t>(offset);
                if (at < rowStart || at >= rowStart + columns)
                    return offset;
                return ColumnOffset::cast_from(rowStart + closedUp(at - rowStart, column, width));
            });
        }

        /// Takes every block wider than the new width of @p page out of the rows [@p top, @p bottom), the
        /// text after it closing up, as kitty drops a block no row is wide enough for
        /// (multiline_copy_src_to_dest, resize.c:225) -- along with its part below its head, lifted already.
        void dropBlocksWiderThan(WidthChange page,
                                 Lines& lines,
                                 int top,
                                 int bottom,
                                 std::vector<TallBlock>& tallBlocks,
                                 ReflowedCursors& cursors)
        {
            auto const columns = unbox<size_t>(page.to);
            for (auto const i: std::views::iota(top, bottom))
            {
                if (lines[i].isTrivialBuffer())
                    continue;
                auto const width = std::as_const(lines[i]).storage().codepoints.size();
                // From the right, so that closing a gap moves only what has been looked at already.
                for (auto const column: std::views::iota(size_t { 0 }, width) | std::views::reverse)
                {
                    auto const& source = std::as_const(lines[i]).storage();
                    if (source.widths[column] <= columns || continuesBlock(source, column))
                        continue;
                    auto const gap = std::min(size_t { source.widths[column] }, width - column);
                    auto& cells = lines[i].storage();
                    moveColumns(cells, column + gap, column, width - column - gap);
                    clearRange(cells, width - gap, gap, padding());

                    auto const row = LineOffset::cast_from(i);
                    std::erase_if(tallBlocks, [&](TallBlock const& block) {
                        return block.row == row && block.column == column;
                    });
                    for (auto& block: tallBlocks)
                        if (block.row == row && block.column > column)
                            block.column -= gap;
                    cursors.closeGap(i, column, gap);
                    closeGapInHeadOffsets(lines, top, i, column, gap, unbox<size_t>(page.from));
                }
            }
        }

        /// Whether @p row has the cells [@p column, @p column + @p width) free: no text there, and no part of
        /// a block.
        [[nodiscard]] bool hasRoomFor(Line const& row, size_t column, size_t width) noexcept
        {
            auto const& cells = row.storage();
            if (cells.codepoints.empty())
                return true; // a blank row stores no cells
            auto const end = std::min(column + width, cells.codepoints.size());
            return std::ranges::all_of(std::views::iota(column, end), [&cells](size_t i) {
                return cells.codepoints[i] == 0 && !continuesBlock(cells, i);
            });
        }

        /// A row is about to be made at @p target of @p rebuilt, @p columns wide, continuing the line the row
        /// above it is in: each column the head of that line marks on a row from @p target on moves down
        /// with the row.
        void openRowInHeadOffsets(Lines& rebuilt, size_t target, size_t columns)
        {
            auto const head = headOf(rebuilt, 0, static_cast<int>(target) - 1);
            if (!head)
                return;
            auto const moved = (target - static_cast<size_t>(*head)) * columns;
            moveHeadOffsets(rebuilt[*head], [&](ColumnOffset offset) {
                auto const at = unbox<size_t>(offset);
                return at < moved ? offset : ColumnOffset::cast_from(at + columns);
            });
        }

        /// Puts each block's lifted part back on the rows below its head, wherever the reflow moved the head:
        /// on a row with room for it there, else on a row made for it right below, as kitty keeps the rows
        /// below a tall block's head for the block (next_dest_line, resize.c:131). A row made inside a line
        /// that goes on continues that line, padded, and moves the columns its head marks below it; one made
        /// below the rebuilt lines starts a line of its own.
        /// @param rebuilt The rebuilt lines, @p columns wide.
        /// @param defaultFlags What a row starting a line of its own carries.
        /// @return How many rows it made.
        size_t placeTallBlocks(Lines& rebuilt,
                               std::span<TallBlock const> blocks,
                               ReflowedCursors& anchors,
                               ColumnCount columns,
                               LineFlags defaultFlags)
        {
            auto& rows = rebuilt.storage();

            // In the order the heads now stand in, so that a row made below one block moves only the blocks
            // still to come.
            auto order = std::ranges::to<std::vector>(std::views::iota(size_t { 0 }, blocks.size()));
            std::ranges::sort(order, {}, [&anchors](size_t index) {
                auto const head = anchors.anchorAt(index);
                return head ? std::pair { head->row, head->column }
                            : std::pair { std::numeric_limits<size_t>::max(), size_t { 0 } };
            });

            auto made = size_t { 0 };
            for (auto const index: order)
            {
                auto const head = anchors.anchorAt(index);
                if (!head)
                    continue;
                auto const& block = blocks[index];
                auto const provenance = rows[head->row].provenance();
                for (auto const below: std::views::iota(size_t { 0 }, block.rowsBelow.size()))
                {
                    auto const target = head->row + 1 + below;
                    if (target == rows.size())
                    {
                        rows.emplace_back(columns, defaultFlags, GraphicsAttributes {});
                        rows.back().adoptProvenance(provenance);
                        ++made;
                    }
                    else if (!hasRoomFor(rows[target], head->column, block.columns))
                    {
                        auto const goesOn = rows[target].wrapped();
                        if (goesOn)
                            openRowInHeadOffsets(rebuilt, target, unbox<size_t>(columns));
                        auto cells = LineSoA {};
                        initializeLineSoA(cells, columns, goesOn ? padding() : GraphicsAttributes {});
                        auto const flags =
                            goesOn ? rows[target - 1].inheritableFlags() | LineFlag::Wrapped : defaultFlags;
                        auto row = Line(flags, std::move(cells), columns);
                        row.adoptProvenance(provenance);
                        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(target), std::move(row));
                        anchors.insertRow(target);
                        ++made;
                    }
                    auto& cells = rows[target].materializedStorage();
                    copyColumns(block.rowsBelow[below],
                                0,
                                cells,
                                head->column,
                                std::min(block.columns, cells.codepoints.size() - head->column));
                }
            }
            return made;
        }
    } // namespace

    /// Splits a logical line (stored as a LineSoA) into fixed-width Line objects.
    /// @param attributes What the logical line carries; only its head keeps the head-only members.
    /// @returns number of inserted lines.
    static LineCount addNewWrappedLines(Lines& targetLines,
                                        ColumnCount newColumnCount,
                                        LineSoA const& logicalLineBuffer,
                                        size_t usedColumns,
                                        LogicalLineAttributes const& attributes,
                                        bool initialNoWrap)
    {
        auto const& baseFlags = attributes.flags;
        auto const newCols = unbox<size_t>(newColumnCount);
        int i = 0;
        size_t offset = 0;

        // Only the very first chunk of a logical line is that line's head, and only when the caller says
        // the split starts one (a shrink hands us the OVERFLOW of a head it already emitted, so every chunk
        // it asks for is a continuation). The head keeps the logical line's semantic marks and its
        // command-end offset — an offset into the LOGICAL line, which the caller moves with the cell it names
        // once it knows where that went. A continuation keeps only the flags that describe a physical line,
        // and is marked as wrapped.
        auto const emitChunk = [&](LineSoA&& chunk) {
            auto const isHead = i == 0 && initialNoWrap;
            targetLines.emplace_back(isHead ? baseFlags
                                            : (baseFlags.without(HeadOnlyLineFlags) | LineFlag::Wrapped),
                                     std::move(chunk),
                                     newColumnCount);
            if (isHead)
            {
                targetLines.back().setCommandEndOffset(attributes.commandEndOffset);
                targetLines.back().setPromptEndOffset(attributes.promptEndOffset);
            }
            // EVERY chunk, head or not: see LineProvenance.
            targetLines.back().adoptProvenance(attributes.provenance);
            ++i;
        };

        while (offset < usedColumns)
        {
            auto const end = rowEnd(logicalLineBuffer, offset, usedColumns, newCols);
            LineSoA chunk;
            initializeLineSoA(chunk, newColumnCount);
            copyColumns(logicalLineBuffer, offset, chunk, 0, end - offset);
            // A row cut short of the edge left a wide character to the next: the rest of it is padding.
            if (end < usedColumns && end - offset < newCols)
                clearRange(chunk,
                           end - offset,
                           newCols - (end - offset),
                           GraphicsAttributes {}.with(CellFlag::WrapPadding));
            emitChunk(std::move(chunk));
            offset = end;
        }
        return LineCount::cast_from(i);
    }

} // namespace detail
// {{{ Grid impl
Grid::Grid(PageSize pageSize, bool reflowOnResize, HistoryLimits historyLimits):
    _pageSize { pageSize },
    _reflowOnResize { reflowOnResize },
    _historyLimits { historyLimits },
    _lines { detail::createLines(
        pageSize,
        [capacity = historyLimits.capacity]() -> LineCount {
            if (auto const* maxLineCount = std::get_if<LineCount>(&capacity))
                return *maxLineCount;
            else
                return LineCount::cast_from(0);
        }(),
        reflowOnResize,
        GraphicsAttributes {}) },
    _linesUsed { pageSize.lines }
{
    verifyState();
}

void Grid::setHistoryLimits(HistoryLimits historyLimits)
{
    // Every config reload hands the limits in again, almost always unchanged. Rebuilding then would destroy
    // stable row identity for nothing -- and with it every block head and every fold keyed on it.
    if (historyLimits == _historyLimits)
        return;

    verifyState();
    rezeroBuffers();
    _historyLimits = historyLimits;

    // Slots appear and vanish at the boundary between the page and the scrollback, NOT at the
    // ring's end.
    //
    // After rezeroBuffers() the ring reads [page][free][history] with the history newest-last, so
    // the ring's end is where the NEWEST history row lives. Resizing there gets both directions
    // exactly backwards: growing splices blanks between that row and the page, blanking the top of
    // the scrollback and sliding the real rows out of the addressable range, and shrinking drops
    // the newest rows while keeping the oldest. The free region at `_pageSize.lines` is the one
    // place where slots may come and go without moving a row relative to offset 0, and erasing
    // past it consumes the OLDEST history rows first -- precisely the end a shrunken scrollback
    // has to forget.
    auto const newTotalLineCount = _pageSize.lines + this->maxHistoryLineCount();
    auto const oldSlotCount = _lines.size();
    auto const newSlotCount = unbox<size_t>(newTotalLineCount);
    auto& slots = _lines.storage();
    auto const freeRegion = std::next(slots.begin(), static_cast<ptrdiff_t>(unbox(_pageSize.lines)));
    if (newSlotCount > oldSlotCount)
        // The prototype rather than a default-constructed Line: a cols=0 line would violate the
        // "every slot is at least pageSize.columns wide" invariant the other paths rely on.
        slots.insert(freeRegion,
                     newSlotCount - oldSlotCount,
                     Line(_pageSize.columns, defaultLineFlags(), GraphicsAttributes {}));
    else
        slots.erase(freeRegion, std::next(freeRegion, static_cast<ptrdiff_t>(oldSlotCount - newSlotCount)));

    _linesUsed = min(_linesUsed, newTotalLineCount);
    bumpGeneration(RowIdentity::Destroyed);
    verifyState();
}

void Grid::finalizeRevisions() noexcept
{
    // Scan the page, the prefix that scrolled out since the last finalize, and any history row
    // handed out for writing since then — all clamped to the valid history. The first covers rows
    // written and then scrolled away within one batch (stamped at their new negative offset); the
    // second covers rows dirtied in place with nothing scrolling at all (@see _dirtyHistoryFloor).
    auto const scrolledFloor = _stableBase - scrolledOutDepthSince(_stableBaseAtLastFinalize);
    auto const next = _seqno + 1;
    auto stamped = false;
    auto stampedHistoryFloor = NoHistoryFloor;
    for (auto offset = scanTopFor(std::min(scrolledFloor, _dirtyHistoryFloor));
         offset < boxed_cast<LineOffset>(_pageSize.lines);
         ++offset)
    {
        // rowAt, not lineAt: the mutable overload would re-arm the very floor this pass clears.
        if (!rowAt(offset).stampRevision(next))
            continue;
        stamped = true;
        if (offset < LineOffset(0))
            stampedHistoryFloor = std::min(stampedHistoryFloor, stableLineIdOf(offset));
    }
    if (stamped)
        _seqno = next;
    // Remember how deep a history change reached and when, so a consumer that has not caught up to
    // this batch still scans down to it (@see forEachLineChangedSince).
    //
    // Derived from the rows actually STAMPED, deliberately not from _dirtyHistoryFloor: that floor
    // is armed by handing a row out mutably, which a read-only caller sitting in a non-const
    // function does too (Screen::captureBuffer walks the whole scrollback). Keying the sticky
    // watermark on it would let one buffer capture pin every later delta scan at the top of the
    // history for the rest of the generation. The per-row branch below costs one predictable
    // compare and is paid only for rows that were genuinely dirty.
    if (stampedHistoryFloor != NoHistoryFloor)
    {
        _changedHistoryFloor = std::min(_changedHistoryFloor, stampedHistoryFloor);
        _changedHistorySeqno = next;
    }
    _dirtyHistoryFloor = NoHistoryFloor;
    _stableBaseAtLastFinalize = _stableBase;
}

void Grid::clearHistory()
{
    _linesUsed = _pageSize.lines;
    // The floor jumps to the base: every history id is evicted, no resend needed —
    // deliberately NOT a generation bump (page row identity is untouched).
    syncStableFloor();
    verifyState();
}

LineCount Grid::dropRowsAbove(LineOffset head)
{
    Require(addressableTop() <= head && head < boxed_cast<LineOffset>(_pageSize.lines));

    // A head on the page scrolls to the top, which moves the rows above it into the history -- the ED 2 half
    // of an ED 2 + ED 3 pair stopped at the head. Then the history keeps only the rows below the head, by
    // shrinking the used range as clampHistory() does; syncStableFloor() counts every row the floor passes.
    auto const shift = LineCount::cast_from(std::max(unbox(head), 0));
    if (shift > LineCount(0))
        scrollUp(shift);
    _linesUsed = _pageSize.lines + LineCount::cast_from(std::max(-unbox(head), 0));
    syncStableFloor();
    verifyState();
    return shift;
}

bool Grid::anyUsedLineWrapped() const noexcept
{
    for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
        if (_lines[i].wrapped())
            return true;
    return false;
}

bool Grid::nothingToReflowOrCut(ColumnCount newColumnCount) const noexcept
{
    auto const width = unbox<size_t>(newColumnCount);
    for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
    {
        auto const& line = _lines[i];
        if (line.wrapped())
            return false;
        if (trimBlankRightKeepingWideCharacters(line.storage(), unbox<size_t>(line.size())) > width)
            return false;
    }
    return true;
}

void Grid::verifyState() const noexcept
{
#ifdef CONTOUR_VERIFY_STATE
    Require(LineCount::cast_from(_lines.size()) >= totalLineCount());
    Require(LineCount::cast_from(_lines.size()) >= _linesUsed);
    Require(_linesUsed >= _pageSize.lines);
    // The floor names exactly the oldest history row: below it would re-validate evicted stable ids,
    // above it would leave live history rows unaddressable. addressableTop() rests on this.
    Require(_stableFloor == _stableBase - unbox<int64_t>(historyLineCount()));
#endif
}

std::string Grid::renderAllText() const
{
    std::string text;
    text.reserve(unbox<size_t>(historyLineCount() + _pageSize.lines) * unbox<size_t>(_pageSize.columns + 1));

    for (auto y = LineOffset(0); y < LineOffset::cast_from(_lines.size()); ++y)
    {
        text += lineText(y);
        text += '\n';
    }

    return text;
}

std::string Grid::renderMainPageText() const
{
    std::string text;

    for (auto line = LineOffset(0); line < unbox<LineOffset>(_pageSize.lines); ++line)
    {
        text += lineText(line);
        text += '\n';
    }

    return text;
}

std::vector<std::string> Grid::renderRange(LineOffset start,
                                           LineOffset end,
                                           CaptureRendition rendition,
                                           CaptureTrailingSpaces trailing) const
{
    // The same top forEachValidLine walks from, so a capture returns exactly the rows a mirror holds.
    auto const top = addressableTop();
    auto const bottom = unbox<LineOffset>(_pageSize.lines) - LineOffset(1);
    auto const from = std::clamp(start, top, bottom);
    auto const to = std::clamp(end, top, bottom);

    auto lines = std::vector<std::string> {};
    if (from > to)
        return lines;
    lines.reserve(unbox<size_t>(to - from) + 1);
    for (auto y = from; y <= to; ++y)
    {
        auto const& line = lineAt(y);
        auto const columns = trailing == CaptureTrailingSpaces::Keep
                                 ? unbox<ColumnOffset>(_pageSize.columns)
                                 : unbox<ColumnOffset>(line.trimmedColumns());
        lines.push_back(rendition == CaptureRendition::WithSgr ? line.toUtf8WithSgr(ColumnOffset(0), columns)
                                                               : line.toUtf8(ColumnOffset(0), columns));
    }
    return lines;
}

Line& Grid::lineAt(LineOffset line) noexcept
{
    return rowAt(line);
}

Line const& Grid::lineAt(LineOffset line) const noexcept
{
    return const_cast<Grid&>(*this).rowAt(line);
}

CellProxy Grid::at(LineOffset line, ColumnOffset column) noexcept
{
    return useCellAt(line, column);
}

CellProxy Grid::useCellAt(LineOffset line, ColumnOffset column) noexcept
{
    return lineAt(line).useCellAt(column);
}

CellProxy Grid::at(LineOffset line, ColumnOffset column) const noexcept
{
    return const_cast<Grid&>(*this).at(line, column);
}

void Grid::resetPageLines(LineCount count, GraphicsAttributes defaultAttributes) noexcept
{
    for (auto const line: std::views::iota(0, *count))
        lineAt(LineOffset(line)).reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);
}

// }}}
// {{{ Grid impl: Line access
std::string Grid::lineText(LineOffset lineOffset) const
{
    return lineAt(lineOffset).toUtf8();
}

std::string Grid::lineTextTrimmed(LineOffset lineOffset) const
{
    std::string output = lineText(lineOffset);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

std::string Grid::lineText(Line const& line) const
{
    return line.toUtf8();
}

void Grid::setLineText(LineOffset line, std::string_view text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(text))
        useCellAt(line, ColumnOffset::cast_from(i++)).setCharacter(ch);
}

bool Grid::isLineBlank(LineOffset line) const noexcept
{
    return lineAt(line).empty();
}

LineOffset Grid::logicalLineHead(LineOffset line) const noexcept
{
    auto const historyTop = -boxed_cast<LineOffset>(historyLineCount());
    while (line > historyTop && lineAt(line).wrapped())
        --line;
    return line;
}

ColumnOffset Grid::logicalColumnOf(CellLocation position) const noexcept
{
    auto const head = logicalLineHead(position.line);
    auto const wrappedLinesAbove = *position.line - *head;
    return ColumnOffset::cast_from(wrappedLinesAbove * *_pageSize.columns) + position.column;
}

CellLocation Grid::cellAtLogicalColumn(LineOffset head, ColumnOffset column, LineOffset limit) const
{
    auto const width = unbox<int>(_pageSize.columns);
    auto const cell = CellLocation { .line = head + LineOffset::cast_from(unbox<int>(column) / width),
                                     .column = ColumnOffset::cast_from(unbox<int>(column) % width) };
    auto const lineEnd =
        CellLocation { .line = logicalLineEnd(head, limit), .column = ColumnOffset::cast_from(width - 1) };
    return std::min(cell, lineEnd);
}

LineOffset Grid::logicalLineEnd(LineOffset head, LineOffset limit) const
{
    auto const below = std::views::iota(unbox<int>(head) + 1, unbox<int>(limit) + 1);
    auto const next = std::ranges::find_if(
        below, [this](int row) { return !lineAt(LineOffset::cast_from(row)).wrapped(); });
    return next == below.end() ? limit : LineOffset::cast_from(*next - 1);
}

namespace
{
    /// Hands @p visit each row of the logical line [@p first, @p last] with the columns of it that its
    /// logical columns [@p begin, @p end) cover, as (row, from, to) with from < to, top down, for as long
    /// as @p visit returns true.
    template <typename Visit>
    void visitLogicalLineColumns(ColumnCount width,
                                 LineOffset first,
                                 LineOffset last,
                                 ColumnOffset begin,
                                 std::optional<ColumnOffset> end,
                                 Visit visit)
    {
        for (auto const row: std::views::iota(unbox<int>(first), unbox<int>(last) + 1))
        {
            auto const line = LineOffset::cast_from(row);
            auto const lineBegin = ColumnOffset::cast_from((*line - *first) * *width);
            auto const lineEnd = lineBegin + boxed_cast<ColumnOffset>(width);
            if (end.has_value() && lineBegin >= *end)
                return;
            auto const from = std::max(begin, lineBegin) - lineBegin;
            auto const to = (end.has_value() ? std::min(*end, lineEnd) : lineEnd) - lineBegin;
            if (from < to && !visit(line, from, to))
                return;
        }
    }
} // namespace

std::string Grid::logicalLineText(LineOffset first,
                                  LineOffset last,
                                  ColumnOffset begin,
                                  std::optional<ColumnOffset> end,
                                  CaptureRendition rendition,
                                  size_t maxBytes) const
{
    auto text = std::string {};
    visitLogicalLineColumns(
        _pageSize.columns, first, last, begin, end, [&](LineOffset line, ColumnOffset from, ColumnOffset to) {
            auto rowText = rendition == CaptureRendition::WithSgr ? lineAt(line).toUtf8WithSgr(from, to)
                                                                  : lineAt(line).toUtf8(from, to);
            if (text.empty())
                text = std::move(rowText);
            else
                text += rowText;
            return text.size() <= maxBytes;
        });

    text.resize(core::trimRight(text).size());
    return text;
}

bool Grid::isLogicalLineBlank(LineOffset first,
                              LineOffset last,
                              ColumnOffset begin,
                              std::optional<ColumnOffset> end) const
{
    auto blank = true;
    visitLogicalLineColumns(
        _pageSize.columns, first, last, begin, end, [&](LineOffset row, ColumnOffset from, ColumnOffset to) {
            auto const& line = lineAt(row);
            if (line.isBlank())
                return true;
            auto const columns =
                std::views::iota(unbox<int>(from), std::min(unbox<int>(to), unbox<int>(line.size())));
            blank = std::ranges::all_of(columns, [&line](int column) {
                return line.cellEmptyAt(ColumnOffset::cast_from(column)) != 0;
            });
            return blank;
        });
    return blank;
}

/**
 * Computes the relative line number for the bottom-most @p n logical lines.
 */
int Grid::computeLogicalLineNumberFromBottom(LineCount n) const noexcept
{
    int logicalLineCount = 0;
    auto outputRelativePhysicalLine = *_pageSize.lines - 1;

    auto i = _lines.rbegin();
    while (i != _lines.rend())
    {
        if (!i->wrapped())
            logicalLineCount++;
        outputRelativePhysicalLine--;
        ++i;
        if (logicalLineCount == *n)
            break;
    }

    while (i != _lines.rend() && i->wrapped())
    {
        outputRelativePhysicalLine--;
        ++i;
    }

    return outputRelativePhysicalLine;
}
// }}}
// {{{ Grid impl: scrolling
LineCount Grid::scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes) noexcept
{
    verifyState();

    // Before the branching, not after: the branch below that grows the history also contains the
    // at-capacity path in its tail, so one call can both fill the last free slot and start
    // recycling. Trimming first is also what hands that path a correctly positioned free region --
    // at capacity the free slots begin at exactly _pageSize.lines, which is where it appends.
    if (historyLineCount() + linesCountToScrollUp > maxHistoryLineCount())
        clampHistory();

    auto const linesAvailable = LineCount::cast_from(_lines.size() - unbox<size_t>(_linesUsed));
    if (std::holds_alternative<Infinite>(_historyLimits.capacity) && linesAvailable < linesCountToScrollUp)
    {
        auto const linesToAllocate = unbox(linesCountToScrollUp - linesAvailable);

        for ([[maybe_unused]] auto const _: std::views::iota(0, linesToAllocate))
            _lines.emplace_back(_pageSize.columns, defaultLineFlags(), GraphicsAttributes());

        return scrollUp(linesCountToScrollUp, defaultAttributes);
    }
    if (unbox<size_t>(_linesUsed) == _lines.size())
    {
        rotateBuffersLeft(linesCountToScrollUp);
        recycleEvictedLines(linesCountToScrollUp, defaultAttributes);
        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(_linesUsed) < _lines.size());

        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            _linesUsed += linesAppendCount;
            syncStableFloor();
            Require(unbox<size_t>(_linesUsed) <= _lines.size());
            fill_n(next(_lines.begin(), *_pageSize.lines),
                   unbox<size_t>(linesAppendCount),
                   Line(_pageSize.columns, defaultLineFlags(), defaultAttributes));
            rotateBuffersLeft(linesAppendCount);
        }
        if (linesAppendCount < linesCountToScrollUp)
        {
            auto const incrementCount = linesCountToScrollUp - linesAppendCount;
            rotateBuffersLeft(incrementCount);
            recycleEvictedLines(incrementCount, defaultAttributes); // the ring just filled up
        }
        return LineCount::cast_from(linesAppendCount);
    }
}

// inline: both of scrollUp's eviction branches call this once per scroll, and without the hint clang
// keeps it out of line, which costs every scrolled line a call.
inline void Grid::recycleEvictedLines(LineCount count, GraphicsAttributes const& defaultAttributes) noexcept
{
    // Each of these lines IS one evicted from the top of the ring, and it is about to be written: keep
    // its storage rather than release it and allocate it again.
    for (auto const y: std::views::iota(*(_pageSize.lines - count), *_pageSize.lines))
        lineAt(LineOffset(y)).recycle(defaultLineFlags(), defaultAttributes, _pageSize.columns);
}

LineCount Grid::scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    verifyState();
    Require(0 <= *margin.horizontal.from && *margin.horizontal.to < *_pageSize.columns);
    Require(0 <= *margin.vertical.from && *margin.vertical.to < *_pageSize.lines);

    auto const fullHorizontal = margin.horizontal
                                == Margin::Horizontal { .from = ColumnOffset { 0 },
                                                        .to = unbox<ColumnOffset>(_pageSize.columns) - 1 };
    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { .from = LineOffset(0), .to = unbox<LineOffset>(_pageSize.lines) - 1 };

    if (fullHorizontal)
    {
        if (fullVertical)
            return scrollUp(n, defaultAttributes);

        auto const marginHeight = LineCount(margin.vertical.length());
        auto const n2 = std::min(n, marginHeight);
        if (*n2 == 0)
            return LineCount(0);

        if (*margin.vertical.from == 0)
        {
            auto const historyLinesAdded = scrollUp(n2, defaultAttributes);
            auto const firstProtectedLine = margin.vertical.to + 1;
            auto const pageBottomLine = boxed_cast<LineOffset>(_pageSize.lines) - LineOffset(1);

            for (auto targetLine = pageBottomLine; targetLine >= firstProtectedLine; --targetLine)
            {
                auto const sourceLine = targetLine - *n2;
                lineAt(targetLine) = std::move(lineAt(sourceLine));
            }

            // Blank the new region rows at the region bottom.
            for (auto const lineNumber:
                 std::views::iota(*margin.vertical.to - *n2 + 1, *margin.vertical.to + 1))
                lineAt(LineOffset(lineNumber))
                    .reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);

            verifyState();
            return historyLinesAdded;
        }

        if (*n2 && n2 < marginHeight)
        {
            for (auto topLineOffset = *margin.vertical.from; topLineOffset <= *margin.vertical.to - *n2;
                 ++topLineOffset)
                _lines[topLineOffset] = std::move(_lines[topLineOffset + *n2]);
        }

        auto const topEmptyLineNr = *margin.vertical.to - *n2 + 1;
        auto const bottomLineNumber = *margin.vertical.to;
        for (auto lineNumber = topEmptyLineNr; lineNumber <= bottomLineNumber; ++lineNumber)
        {
            _lines[lineNumber].reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);
        }
    }
    else
    {
        auto const marginHeight = margin.vertical.length();
        auto const n2 = std::min(n, marginHeight);
        auto const topTargetLineOffset = margin.vertical.from;
        auto const bottomTargetLineOffset = margin.vertical.to - *n2;
        auto const columnsToMove = unbox<size_t>(margin.horizontal.length());

        for (LineOffset targetLineOffset = topTargetLineOffset; targetLineOffset <= bottomTargetLineOffset;
             ++targetLineOffset)
        {
            auto const sourceLineOffset = targetLineOffset + *n2;
            auto& targetLine = lineAt(targetLineOffset);
            auto const& sourceLine = lineAt(sourceLineOffset);
            auto const fromCol = unbox<size_t>(margin.horizontal.from);
            // copyColumns handles a blank source by clearing the destination range.
            // The destination needs to be materialized for the write — but only when the
            // copy would actually change the destination. Two blank lines with the same
            // fillAttrs collapse to a no-op; differing fillAttrs require materialization
            // so the source's attrs propagate into the target's [fromCol, fromCol+count).
            // Through std::as_const: the mutable storage() overload dirties pessimistically, and
            // this branch reads the target purely to decide that it needs no write at all. Taking
            // the mutable overload here stamps a provably unchanged row into the next delta batch,
            // once per skipped row per scroll.
            if (sourceLine.isBlank() && targetLine.isBlank()
                && sourceLine.storage().fillAttrs == std::as_const(targetLine).storage().fillAttrs)
                continue;
            copyColumns(
                sourceLine.storage(), fromCol, targetLine.materializedStorage(), fromCol, columnsToMove);
        }

        for (LineOffset line = margin.vertical.to - *n2 + 1; line <= margin.vertical.to; ++line)
        {
            auto& targetLine = lineAt(line);
            auto const fromCol = unbox<size_t>(margin.horizontal.from);
            // Clearing a blank line with non-default attrs would change semantics; only skip
            // when the target is already blank with matching fillAttrs.
            if (targetLine.isBlankWithFillAttrs(defaultAttributes))
                continue;
            clearRange(targetLine.materializedStorage(),
                       fromCol,
                       unbox<size_t>(margin.horizontal.length()),
                       defaultAttributes);
        }
    }
    verifyState();
    return LineCount(0);
}

Grid::ScrollDownShape Grid::scrollDownShape(Margin const& margin) const noexcept
{
    auto const fullHorizontal =
        margin.horizontal
        == Margin::Horizontal { .from = ColumnOffset { 0 },
                                .to = unbox<ColumnOffset>(_pageSize.columns) - ColumnOffset(1) };
    if (!fullHorizontal)
        return ScrollDownShape::ShiftRegionCells;

    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { .from = LineOffset(0),
                              .to = unbox<LineOffset>(_pageSize.lines) - LineOffset(1) };
    if (fullVertical && historyLineCount() == LineCount(0))
        return ScrollDownShape::RotateRing;

    return ScrollDownShape::MoveRegionRows;
}

void Grid::scrollDown(LineCount vN, GraphicsAttributes const& defaultAttributes, Margin const& margin)
{
    verifyState();
    Require(vN >= LineCount(0));

    auto const n = std::min(vN, margin.vertical.length());

    switch (scrollDownShape(margin))
    {
        case ScrollDownShape::RotateRing:
            rotateBuffersRight(n);
            resetPageLines(n, defaultAttributes);
            return;
        case ScrollDownShape::MoveRegionRows: {
            // The rows move within the region and nowhere else: with a full-page region, the history
            // above the page is not the region's, and the rows pushed off the region's bottom are gone.
            //
            // One move per row that keeps its content, bottom first so each is read before it is
            // overwritten, and a blank row into each of the n it frees at the top. A rotation would swap
            // every row of the region instead, each swap three moves of a Line and its many buffers,
            // which made a reverse scroll with history several times dearer than the ring rotation it
            // replaced.
            auto const top = *margin.vertical.from;
            auto const bottom = *margin.vertical.to;
            for (auto const i: std::views::iota(top + *n, bottom + 1) | std::views::reverse)
                _lines[i] = std::move(_lines[i - *n]);
            for (auto const i: std::views::iota(top, top + *n))
                _lines[i] = Line(_pageSize.columns, defaultLineFlags(), defaultAttributes);
            return;
        }
        case ScrollDownShape::ShiftRegionCells: break;
    }

    // Shift every line that has somewhere to go: targets [from+n, to], each pulling from n rows
    // above. The lower bound is from+n, not to-n -- with a full-height vertical margin the latter
    // only touches the bottom n+1 (often blank) rows and never moves the content down at all.
    // Iterate downward so each source is read before it is overwritten as a later target.
    for (LineOffset line = margin.vertical.to; line >= margin.vertical.from + *n; --line)
    {
        auto const& srcLine = lineAt(line - *n);
        auto& dstLine = lineAt(line);
        auto const fromCol = unbox<size_t>(margin.horizontal.from);
        // Only skip when both lines are blank AND share fillAttrs; differing
        // attrs must propagate into the copied range (requires materialization).
        // std::as_const for the same reason as in scrollUp: the read that decides "no write
        // needed" must not go through the dirtying overload.
        if (srcLine.isBlank() && dstLine.isBlank()
            && srcLine.storage().fillAttrs == std::as_const(dstLine).storage().fillAttrs)
            continue;
        copyColumns(srcLine.storage(),
                    fromCol,
                    dstLine.materializedStorage(),
                    fromCol,
                    unbox<size_t>(margin.horizontal.length()));
    }

    for (LineOffset line = margin.vertical.from; line < margin.vertical.from + *n; ++line)
    {
        auto& targetLine = lineAt(line);
        auto const fromCol = unbox<size_t>(margin.horizontal.from);
        if (targetLine.isBlankWithFillAttrs(defaultAttributes))
            continue;
        clearRange(targetLine.materializedStorage(),
                   fromCol,
                   unbox<size_t>(margin.horizontal.length()),
                   defaultAttributes);
    }
}

void Grid::unscroll(LineCount n, GraphicsAttributes const& defaultAttributes)
{
    verifyState();
    auto const clampedN = std::min(n, _pageSize.lines);
    auto const pullable = std::min(clampedN, historyLineCount());

    if (*pullable > 0)
    {
        rotateBuffersRight(pullable);
        _linesUsed -= pullable;
        syncStableFloor();
    }

    auto const remaining = clampedN - pullable;
    if (*remaining > 0)
    {
        rotateBuffersRight(remaining);
        resetPageLines(remaining, defaultAttributes);
    }
    verifyState();
}

std::optional<RowMove> Grid::rowMoveOfScrollUp(LineCount n, Margin margin) const noexcept
{
    // The branches of scrollUp(n, attributes, margin), as they treat whole rows.
    auto const pageBottom = boxed_cast<LineOffset>(_pageSize.lines) - 1;
    auto const fullHorizontal = margin.horizontal
                                == Margin::Horizontal { .from = ColumnOffset { 0 },
                                                        .to = unbox<ColumnOffset>(_pageSize.columns) - 1 };
    auto const fullVertical = margin.vertical == Margin::Vertical { .from = LineOffset(0), .to = pageBottom };
    auto const count = unbox<int64_t>(std::min(n, LineCount(margin.vertical.length())));
    if (!fullHorizontal || fullVertical || count == 0)
        return std::nullopt;

    if (margin.vertical.from == LineOffset(0))
    {
        // The region's rows scroll into the history with their ids, as on a whole-page scroll, while the rows
        // below it keep their places as the ids advance past them.
        auto const below = StableIdSpan { .first = stableLineIdOf(margin.vertical.to + 1),
                                          .last = stableLineIdOf(pageBottom) };
        return RowMove { .changed = below, .moved = below, .by = count };
    }

    // Each row of the region takes the content count rows below it, and the region's top rows drop theirs.
    auto const top = stableLineIdOf(margin.vertical.from);
    auto const bottom = stableLineIdOf(margin.vertical.to);
    return RowMove { .changed = { .first = top, .last = bottom },
                     .moved = { .first = top + count, .last = bottom },
                     .by = -count };
}

std::optional<RowMove> Grid::rowMoveOfScrollDown(LineCount n, Margin margin) const noexcept
{
    // What the branch scrollDown(n, attributes, margin) takes does to whole rows.
    auto const count = unbox<int64_t>(std::min(n, LineCount(margin.vertical.length())));
    if (count == 0)
        return std::nullopt;

    auto const top = stableLineIdOf(margin.vertical.from);
    auto const bottom = stableLineIdOf(margin.vertical.to);
    switch (scrollDownShape(margin))
    {
        case ScrollDownShape::ShiftRegionCells: return std::nullopt;
        case ScrollDownShape::RotateRing:
            // The page's rows move down with their ids, and the ones pushed off its bottom are dropped. The
            // new blank top rows take ids below the floor, which no holder has.
            return RowMove { .changed = { .first = top - count, .last = bottom },
                             .moved = { .first = top, .last = bottom - count },
                             .by = 0 };
        case ScrollDownShape::MoveRegionRows:
            // Each row of the region takes the content count rows above it, and the region's bottom rows
            // drop theirs.
            return RowMove { .changed = { .first = top, .last = bottom },
                             .moved = { .first = top, .last = bottom - count },
                             .by = count };
    }
    core::unreachable();
}

std::optional<RowMove> Grid::rowMoveOfUnscroll(LineCount n) const noexcept
{
    // The history rows pulled back onto the page keep their ids and content; the rows pushed off its bottom
    // are dropped.
    auto const count = unbox<int64_t>(std::min(n, _pageSize.lines));
    if (count == 0)
        return std::nullopt;
    auto const bottom = stableLineIdOf(boxed_cast<LineOffset>(_pageSize.lines) - 1);
    return RowMove { .changed = { .first = bottom - count + 1, .last = bottom },
                     .moved = { .first = bottom + 1, .last = bottom },
                     .by = 0 };
}

void Grid::scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    for (LineOffset lineNo = margin.vertical.from; lineNo <= margin.vertical.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto const from = unbox<size_t>(margin.horizontal.from);
        auto const to = unbox<size_t>(margin.horizontal.to) + 1;
        auto const count = to - from;
        if (count > 1)
        {
            // Blank line with matching fillAttrs is invariant under this rotation.
            if (line.isBlankWithFillAttrs(defaultAttributes))
                continue;
            auto& storage = line.materializedStorage();
            moveColumns(storage, from + 1, from, count - 1);
            clearRange(storage, from + count - 1, 1, defaultAttributes);
        }
    }
}

// }}}
// {{{ Grid impl: resize
void Grid::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotateRight(_lines.zeroIndex());
    for (int i = 0; i < unbox(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {}, _pageSize.columns);
    bumpGeneration(RowIdentity::Destroyed);
    // RIS: the session starts over, and its numbering with it. Zeroed after the bump, whose floor sync
    // has just counted the whole scrollback this threw away.
    _evictedRowCount = 0;
    verifyState();
}

CellLocation Grid::growLines(LineCount newHeight, CellLocation cursor)
{
    Require(newHeight > _pageSize.lines);

    CellLocation cursorMove {};
    if (*cursor.line + 1 == *_pageSize.lines)
    {
        auto const totalLinesToExtend = newHeight - _pageSize.lines;
        auto const linesToTakeFromSavedLines = std::min(totalLinesToExtend, historyLineCount());
        Require(totalLinesToExtend >= linesToTakeFromSavedLines);
        Require(*linesToTakeFromSavedLines >= 0);
        rotateBuffersRight(linesToTakeFromSavedLines);
        _pageSize.lines += linesToTakeFromSavedLines;
        cursorMove.line += boxed_cast<LineOffset>(linesToTakeFromSavedLines);
    }

    auto const wrappableFlag = _lines.back().wrappableFlag();
    auto const totalLinesToExtend = newHeight - _pageSize.lines;
    Require(*totalLinesToExtend >= 0);

    auto const newTotalLineCount = maxHistoryLineCount() + newHeight;
    auto const currentTotalLineCount = LineCount::cast_from(_lines.size());
    auto const linesToFill = max(0, *newTotalLineCount - *currentTotalLineCount);

    for ([[maybe_unused]] auto const _: std::views::iota(0, linesToFill))
        _lines.emplace_back(_pageSize.columns, wrappableFlag, GraphicsAttributes {});

    // The lines entering the page come from the ring's unused tail, which a column change leaves at
    // its old width (see growColumns/shrinkColumns) on the promise that it is re-widthed when
    // recycled. This is such a recycling, and the page must not hold a line narrower than itself:
    // the terminal writes to any page column without asking the line how wide it is.
    for (auto const y: std::views::iota(*_pageSize.lines, *newHeight))
        if (auto& line = _lines[y]; line.size() != _pageSize.columns)
            line.resize(_pageSize.columns);

    _pageSize.lines += totalLinesToExtend;
    _linesUsed = min(_linesUsed + totalLinesToExtend, LineCount::cast_from(_lines.size()));
    syncStableFloor(); // the min() clamp can shrink the history at ring capacity

    Ensures(_pageSize.lines == newHeight);
    Ensures(_lines.size() >= unbox<size_t>(maxHistoryLineCount() + _pageSize.lines));
    verifyState();

    return cursorMove;
}

CellLocation Grid::resize(PageSize newSize, CellLocation currentCursorPos, WrapPending wrapPending)
{
    auto cursor = std::array { nextWritePosition(newSize, currentCursorPos, wrapPending) };
    resize(newSize, cursor, std::nullopt);
    return cursor.front();
}

CellLocation Grid::nextWritePosition(PageSize newSize,
                                     CellLocation cursor,
                                     WrapPending wrapPending) const noexcept
{
    if (wrapPending == WrapPending::Yes && newSize.columns != _pageSize.columns)
        ++cursor.column;
    return cursor;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Grid::resize(PageSize newSize, std::span<CellLocation> cursors, std::optional<LineOffset> unreflowedFrom)
{
    ZoneScoped;
    Require(!cursors.empty());
    if (_pageSize == newSize)
        return;

    gridLog()("resize {} -> {} (cursor {})", _pageSize, newSize, cursors.front());

    // {{{ helper methods
    auto const shrinkLines = [this](LineCount newHeight, CellLocation cursor) -> CellLocation {
        Require(newHeight < _pageSize.lines);

        auto const numLinesToShrink = _pageSize.lines - newHeight;
        auto const linesAvailableBelowCursorBeforeShrink =
            _pageSize.lines - boxed_cast<LineCount>(cursor.line + 1);
        auto const cutoffCount = min(numLinesToShrink, linesAvailableBelowCursorBeforeShrink);
        auto const numLinesToPushUp = numLinesToShrink - cutoffCount;
        auto const numLinesToPushUpCapped = min(numLinesToPushUp, maxHistoryLineCount());

        gridLog()(" -> shrink lines: numLinesToShrink {}, linesAvailableBelowCursorBeforeShrink {}, "
                  "cutoff {}, pushUp "
                  "{}/{}",
                  numLinesToShrink,
                  linesAvailableBelowCursorBeforeShrink,
                  cutoffCount,
                  numLinesToPushUp,
                  numLinesToPushUpCapped);

        Ensures(numLinesToShrink == cutoffCount + numLinesToPushUp);

        if (cutoffCount != LineCount(0))
        {
            _pageSize.lines -= cutoffCount;
            _linesUsed -= cutoffCount;
            syncStableFloor();
            Ensures(*cursor.line < *_pageSize.lines);
            verifyState();
        }

        Require(newHeight <= _pageSize.lines);
        if (*numLinesToPushUp)
        {
            gridLog()(" -> numLinesToPushUp {}", numLinesToPushUp);
            Require(*cursor.line + 1 == *_pageSize.lines);
            // The page shrinks before the ring rotates, as in scrollUp: the history grows by the rows
            // pushed up first, so the floor sync inside the rotation sees them as history and keeps
            // them addressable. The other order would walk the floor across them and count them as
            // evicted, though the ring still holds every one.
            _pageSize.lines -= numLinesToPushUp;
            rotateBuffersLeft(numLinesToPushUp);
            clampHistory();
            verifyState();
            return CellLocation { .line = -boxed_cast<LineOffset>(numLinesToPushUp), .column = {} };
        }

        verifyState();
        return CellLocation {};
    };

    // Each cursor is where the next character goes from it: one past its cell when its wrap is pending.
    // That is the place a column change must keep.
    auto reflowedCursors = detail::ReflowedCursors { cursors };

    // The rows a reflowing column change leaves as they are: from the head of the logical line unreflowedFrom
    // names down to the bottom of the page. Without it, none.
    Require(!unreflowedFrom
            || (-boxed_cast<LineOffset>(historyLineCount()) <= *unreflowedFrom
                && *unreflowedFrom < boxed_cast<LineOffset>(_pageSize.lines)));
    auto const keptFrom =
        unreflowedFrom ? logicalLineHead(*unreflowedFrom) : boxed_cast<LineOffset>(_pageSize.lines);

    // Each of them is cut at the new width or padded to it and stays a row of its own, as a row that does not
    // wrap is cut, with any cursor on it. Returns how many rows it appended.
    auto const keepUnreflowedRows = [&](Lines& rebuilt, ColumnCount newColumnCount) {
        for (auto const i: std::views::iota(*keptFrom, *_pageSize.lines))
        {
            reflowedCursors.keepRow(i, rebuilt.size(), unbox<size_t>(newColumnCount));
            _lines[i].resize(newColumnCount);
            rebuilt.emplace_back(std::move(_lines[i]));
        }
        return LineCount::cast_from(*_pageSize.lines - *keptFrom);
    };

    // A block taller than one row (OSC 66) has its part on the rows below its head lifted out of them before
    // the rows are re-chopped -- which moves the head whole, as a wide character -- and put back below the
    // head after, the head followed as an anchor. A block whose head is not re-chopped stays as it is.
    auto const liftedTallBlocks = [&]() {
        auto blocks = detail::findTallBlocks(_lines, -*historyLineCount(), *keptFrom);
        detail::liftTallBlocks(_lines, blocks, *_pageSize.lines);
        return blocks;
    };
    auto const followTallBlocks = [&](std::span<detail::TallBlock const> blocks) {
        for (auto const& block: blocks)
            reflowedCursors.anchor(block.row, block.column);
    };

    // The columns the head at row i marks on its line, whose rows are about to be cut at the new width or
    // padded to it rather than re-chopped: each stays on its row, held to the row's last column where the cut
    // takes its cell, as a cursor on such a row is. Nothing for a row that is no head.
    auto const holdHeadOffsets = [&](int i) {
        if (auto& line = _lines[i]; !line.wrapped())
            detail::moveHeadOffsets(
                line,
                [width = WidthChange { .from = _pageSize.columns, .to = newSize.columns }](
                    ColumnOffset offset) { return unreflowedColumn(offset, width); });
    };

    // A narrowing that re-chops nothing, for nothing is wrapped and nothing but blanks is past the new width,
    // cuts each row where it is. A row a reflow would cut rather than re-chop -- one that does not wrap --
    // has the columns marked on it held as it would; on any other, a column past the new width stays counted
    // from the end of the text. As in growColumns, the blank tail is left alone and re-widthed when recycled.
    auto const cutInPlace = [&](ColumnCount newColumnCount) {
        for (auto const i: std::views::iota(-*historyLineCount(), *_pageSize.lines))
        {
            if (i < *keptFrom && !_lines[i].wrappable())
                holdHeadOffsets(i); // the rows kept are held already
            if (newColumnCount < _lines[i].size())
                _lines[i].resize(newColumnCount);
        }
    };

    auto const growColumns = [&](ColumnCount newColumnCount) {
        if (!_reflowOnResize)
        {
            for (auto& line: _lines)
                if (line.size() < newColumnCount)
                    line.resize(newColumnCount);
            _pageSize.columns = newColumnCount;
            verifyState();
            return;
        }
        else
        {
            auto const extendCount = newColumnCount - _pageSize.columns;
            Require(*extendCount > 0);

            // Reflow exists to rejoin lines that were split by wrapping. When nothing in the used
            // range is wrapped there is nothing to rejoin, and the reflow below would emit exactly
            // the lines already held, only wider -- which is what the non-reflow path above does in
            // place, without rebuilding the ring.
            //
            // Worth the check because rebuilding is not proportional to what the grid holds but to
            // what it *could* hold: the ring is sized to HistoryLimits::capacity, so a column change
            // constructs that many blank Lines and destroys as many old ones. At a 100,000-line
            // scrollback that is 20 MB of memory traffic and ~4.5ms per column change, on an empty
            // grid, and a window drag pays one per column step. The scan is over the used lines
            // only, so it costs nothing when there is nothing to skip.
            if (!anyUsedLineWrapped())
            {
                // Only the used lines need widening. The blank tail keeps its old width and is
                // given the current one when it is recycled -- every Line::reset() call site in
                // this file passes _pageSize.columns for exactly that reason. Walking the tail here
                // would be ~100,000 iterations per column change at a large scrollback, whatever
                // the grid actually holds.
                for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
                    if (_lines[i].size() < newColumnCount)
                        _lines[i].resize(newColumnCount);
                _pageSize.columns = newColumnCount;
                verifyState();
                return;
            }

            auto const tallBlocks = liftedTallBlocks();
            followTallBlocks(tallBlocks);

            Lines grownLines;
            // Reserved up front, as shrinkColumns() already does for its own buffer. The tail of
            // this function pads to page + maxHistoryLineCount() -- the CONFIGURED history limit,
            // not the used one -- so the vector grows to thousands of entries on every column
            // change even when the grid is empty, and without a reserve it reallocates and moves
            // every Line it already holds on the way there. Measured at startup, where nine column
            // changes are negotiated before the window settles.
            grownLines.reserve(unbox<size_t>(_pageSize.lines + maxHistoryLineCount()));
            LineSoA logicalLineBuffer;
            initializeLineSoA(logicalLineBuffer, ColumnCount(0));
            size_t logicalLineUsed = 0;
            size_t logicalLineCells = 0; // what logicalLineUsed would be, were no blanks dropped
            auto logicalLineAttributes = detail::LogicalLineAttributes {};

            auto const appendToLogicalLine = [&logicalLineBuffer,
                                              &logicalLineUsed](Line const& line, detail::LineGoesOn goesOn) {
                auto const used = detail::carriedColumns(line, goesOn);
                if (used > 0)
                {
                    auto const newSize = logicalLineUsed + used;
                    resizeLineSoA(logicalLineBuffer, ColumnCount::cast_from(newSize));
                    detail::copyCarriedColumns(line.storage(), logicalLineBuffer, logicalLineUsed, used);
                    logicalLineUsed = newSize;
                }
            };

            auto const flushLogicalLine = [&]() {
                logicalLineCells = 0;
                reflowedCursors.cut(
                    grownLines.size(), unbox<size_t>(newColumnCount), logicalLineBuffer, logicalLineUsed);
                if (logicalLineUsed > 0)
                {
                    detail::addNewWrappedLines(grownLines,
                                               newColumnCount,
                                               logicalLineBuffer,
                                               logicalLineUsed,
                                               logicalLineAttributes,
                                               true);
                    initializeLineSoA(logicalLineBuffer, ColumnCount(0));
                    logicalLineUsed = 0;
                }
            };

            auto const width = WidthChange { .from = _pageSize.columns, .to = newColumnCount };
            for (int i = -*historyLineCount(); i < *keptFrom; ++i)
            {
                auto& line = _lines[i];
                Require(line.size() >= _pageSize.columns);

                if (!line.wrapped())
                {
                    flushLogicalLine();
                    reflowedCursors.startLine(grownLines, i, line, width);
                    if (line.empty())
                    {
                        reflowedCursors.keepRow(i, grownLines.size(), unbox<size_t>(newColumnCount));
                        line.resize(newColumnCount);
                        grownLines.emplace_back(std::move(line));
                        continue;
                    }
                }

                // Whichever row a logical line starts with gives it its values: its head, which the flush
                // above just made the start, or a continuation whose head is gone.
                if (logicalLineUsed == 0)
                    logicalLineAttributes = detail::logicalLineAttributesFrom(line);

                auto const goesOn = i + 1 < *_pageSize.lines && _lines[i + 1].wrapped()
                                        ? detail::LineGoesOn::Yes
                                        : detail::LineGoesOn::No;
                reflowedCursors.joinRow(i, line, logicalLineUsed, logicalLineCells);
                appendToLogicalLine(line, goesOn);
                reflowedCursors.reach(logicalLineBuffer, logicalLineUsed);
                // Read only for the rows before the cursor's, all of which go on and so are carried whole.
                logicalLineCells += detail::carriedColumns(line, goesOn);
            }

            flushLogicalLine();
            reflowedCursors.placeLine(grownLines, unbox<size_t>(newColumnCount));
            keepUnreflowedRows(grownLines, newColumnCount);
            detail::placeTallBlocks(
                grownLines, tallBlocks, reflowedCursors, newColumnCount, defaultLineFlags());

            if (_pageSize.lines > LineCount::cast_from(grownLines.size()))
            {
                while (LineCount::cast_from(grownLines.size()) < _pageSize.lines)
                    grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

                Ensures(LineCount::cast_from(grownLines.size()) == _pageSize.lines);
            }

            _linesUsed = LineCount::cast_from(grownLines.size());

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            {
                ZoneScopedN("growColumns.padHistory");
                ZoneValue(static_cast<int64_t>(totalLineCount - grownLines.size()));
                while (grownLines.size() < totalLineCount)
                    grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});
            }

            {
                ZoneScopedN("growColumns.commit");
                _lines = std::move(grownLines);
            }
            _pageSize.columns = newColumnCount;

            auto const newHistoryLineCount = _linesUsed - _pageSize.lines;
            rekeyRebuiltRing(newHistoryLineCount);

            verifyState();
            reflowedCursors.place(newHistoryLineCount, cursors, newColumnCount);
        }
    };

    auto const shrinkColumns = [&](ColumnCount newColumnCount) {
        auto const newColumns = unbox<size_t>(newColumnCount);

        if (!_reflowOnResize)
        {
            _pageSize.columns = newColumnCount;
            for (auto& line: _lines)
                if (newColumnCount < line.size())
                    line.resize(newColumnCount);
            verifyState();
            // A cursor that still fits stays where it is; one past the new width moves onto its last column.
            for (auto& cursor: cursors)
                cursor.column = std::min(cursor.column, boxed_cast<ColumnOffset>(newColumnCount) - 1);
            return;
        }
        else
        {
            // The mirror of the guard in growColumns, but stricter on purpose: widening loses
            // nothing, so growing needs only that nothing is wrapped, while narrowing can cut
            // content, so it also needs nothing past the new width (see nothingToReflowOrCut).
            // Startup passes, where the window negotiates its width against a grid that holds
            // nothing, as does any narrowing whose used lines, scrollback included, are unwrapped
            // and fit the new width -- and none of them rebuilds a ring sized to the configured
            // scrollback. The cursor must fit too: one past the new width needs a row added to its
            // line.
            auto const cursorsFit = std::ranges::all_of(cursors, [newColumnCount](CellLocation c) {
                return c.column < boxed_cast<ColumnOffset>(newColumnCount);
            });
            if (cursorsFit && nothingToReflowOrCut(newColumnCount))
            {
                cutInPlace(newColumnCount);
                _pageSize.columns = newColumnCount;
                verifyState();
                return;
            }

            auto const width = WidthChange { .from = _pageSize.columns, .to = newColumnCount };
            auto tallBlocks = liftedTallBlocks();
            detail::dropBlocksWiderThan(
                width, _lines, -*historyLineCount(), *keptFrom, tallBlocks, reflowedCursors);
            followTallBlocks(tallBlocks);

            Lines shrunkLines;
            LineSoA wrappedColumns;
            initializeLineSoA(wrappedColumns, ColumnCount(0));
            size_t wrappedUsed = 0;
            // The row whose overflow is pending in wrappedColumns: its flags decide whether the next row
            // may absorb that overflow, and its carried values go to every chunk the overflow is split
            // into -- a chunk belongs to the row it was cut from. Every row sets it before it is cut, so
            // the initial value is never read. The two offsets stay at zero: a shrink emits only
            // continuations, which carry neither.
            auto previousAttributes = detail::LogicalLineAttributes {};

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            shrunkLines.reserve(totalLineCount);
            Require(totalLineCount == unbox<size_t>(this->totalLineCount()));

            auto numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *keptFrom; ++i)
            {
                auto& line = _lines[i];
                reflowedCursors.enterRow(i);
                // Whether the next row takes this one's overflow, as the merge below decides it for that row.
                auto const goesOn = i + 1 < *_pageSize.lines && _lines[i + 1].wrapped()
                                            && _lines[i + 1].inheritableFlags() == line.inheritableFlags()
                                        ? detail::LineGoesOn::Yes
                                        : detail::LineGoesOn::No;

                if (wrappedUsed > 0)
                {
                    if (line.wrapped() && line.inheritableFlags() == previousAttributes.flags)
                    {
                        // Prepend wrapped columns to this line using SoA copyColumns
                        auto const used = detail::carriedColumns(line, goesOn);
                        auto const totalCols = wrappedUsed + used;
                        LineSoA merged;
                        initializeLineSoA(merged, ColumnCount::cast_from(totalCols));
                        copyColumns(wrappedColumns, 0, merged, 0, wrappedUsed);
                        detail::copyCarriedColumns(line.storage(), merged, wrappedUsed, used);

                        // The one place a Line is rebuilt from its parts. It is a CONTINUATION (the branch
                        // is guarded on it), so it carries no command-end offset to lose — but say so out
                        // loud rather than let the constructor's default quietly stand in for the reason.
                        Require(line.commandEndOffset() == ColumnOffset(0));
                        // Carried across the rebuild explicitly. Unlike the offsets above, the provenance
                        // is NOT default-correct here: a rebuilt continuation keeps its own values, and a
                        // fresh Line would report none.
                        auto const provenance = line.provenance();
                        line = Line(line.flags(), std::move(merged), ColumnCount::cast_from(totalCols));
                        line.adoptProvenance(provenance);
                        reflowedCursors.prependOverflow(wrappedUsed);
                    }
                    else
                    {
                        reflowedCursors.cut(shrunkLines.size(), newColumns, wrappedColumns, wrappedUsed);
                        // Every chunk here is the OVERFLOW of a head that was already emitted, so it is a
                        // continuation: no semantic marks, and therefore no command-end offset either.
                        auto const numLinesInserted = detail::addNewWrappedLines(shrunkLines,
                                                                                 newColumnCount,
                                                                                 wrappedColumns,
                                                                                 wrappedUsed,
                                                                                 previousAttributes,
                                                                                 false);
                        numLinesWritten += numLinesInserted;
                    }
                    wrappedUsed = 0;
                    initializeLineSoA(wrappedColumns, ColumnCount(0));
                }

                // The overflow of the line before has gone into rows of its own by now, if this row starts
                // one.
                reflowedCursors.startLine(shrunkLines, i, line, width);

                // Whatever this row is about to lose is ITS overflow, whether or not it was just rebuilt
                // from its predecessor's: a run of rebuilt continuations must not hand on the values of
                // the row before the run. (A rebuilt row's flags equal the ones it was merged under.)
                previousAttributes = { .flags = line.inheritableFlags(), .provenance = line.provenance() };
                auto const rowWidth = detail::carriedColumns(line, goesOn);
                auto const kept =
                    line.wrappable() ? wideCharacterSafeEnd(line.storage(), 0, newColumns) : newColumns;
                auto overflow = line.reflow(newColumnCount);
                auto const overflowCols = overflow.codepoints.size();
                if (overflowCols > 0)
                {
                    wrappedColumns = std::move(overflow);
                    wrappedUsed = trimBlankRightKeepingWideCharacters(wrappedColumns, overflowCols);
                }
                // Line::reflow drops the blanks that end the row, which may be inside the logical line.
                if (goesOn == detail::LineGoesOn::Yes && line.wrappable() && rowWidth > kept)
                    detail::extendWithBlanks(wrappedColumns, wrappedUsed, rowWidth - kept);

                reflowedCursors.cutRow(shrunkLines.size(),
                                       newColumns,
                                       kept,
                                       line.wrappable() ? detail::NarrowedRow::Reflowed
                                                        : detail::NarrowedRow::Cut,
                                       wrappedColumns,
                                       wrappedUsed);

                shrunkLines.emplace_back(std::move(line));
                numLinesWritten++;
                Ensures(shrunkLines.back().size() >= newColumnCount);
            }
            if (wrappedUsed > 0)
            {
                reflowedCursors.cut(shrunkLines.size(), newColumns, wrappedColumns, wrappedUsed);
                numLinesWritten += detail::addNewWrappedLines(
                    shrunkLines, newColumnCount, wrappedColumns, wrappedUsed, previousAttributes, false);
            }
            reflowedCursors.placeLine(shrunkLines, newColumns);
            numLinesWritten += keepUnreflowedRows(shrunkLines, newColumnCount);
            numLinesWritten += LineCount::cast_from(detail::placeTallBlocks(
                shrunkLines, tallBlocks, reflowedCursors, newColumnCount, defaultLineFlags()));
            Require(unbox<size_t>(numLinesWritten) == shrunkLines.size());
            Require(numLinesWritten >= _pageSize.lines);

            // The blank rows below the cursor make room for the rows the narrowing added, before anything
            // is pushed into the history -- kitty, foot and WezTerm drop them likewise -- so a prompt on an
            // empty page stays in view. The page is refilled with blank rows below.
            numLinesWritten -= detail::dropTrailingUntouchedBlanks(
                shrunkLines,
                std::max(unbox<size_t>(_pageSize.lines),
                         reflowedCursors.lowestRow().value_or(shrunkLines.size()) + 1));

            while (shrunkLines.size() < totalLineCount)
                shrunkLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

            _linesUsed = LineCount::cast_from(numLinesWritten);
            _lines = std::move(shrunkLines);
            _pageSize.columns = newColumnCount;

            // The rotation goes through the ring primitive rather than being applied to the local
            // vector, because that primitive is where the stable-id accounting lives. Rotating
            // `shrunkLines` instead left `_stableBase` exactly where it was while the history GREW
            // underneath it, so every history row the reflow had just created sat below
            // `_stableFloor` and forEachValidLine() began its walk above them: a client attaching
            // after a narrowing resize got a grid with no scrollback at all, though the daemon still
            // held the rows. growColumns goes through the same primitive.
            rekeyRebuiltRing(historyLineCount());

            verifyState();
            reflowedCursors.place(historyLineCount(), cursors, newColumnCount);
        }
    };
    // }}}

    // Read before either resize below moves it: only a COLUMN change reflows, and reflow is the half
    // of this that rebuilds the ring and with it every row's identity.
    auto const columnsChanged = newSize.columns != _pageSize.columns;

    // Every row of a grid that does not reflow is cut or padded rather than re-chopped, as are the rows a
    // reflowing one keeps.
    if (columnsChanged)
        for (auto const i:
             std::views::iota(_reflowOnResize ? *keptFrom : -*historyLineCount(), *_pageSize.lines))
            holdHeadOffsets(i);

    using crispy::Comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
    {
        case Comparison::Greater: {
            ZoneScopedN("Grid::growColumns");
            growColumns(newSize.columns);
            break;
        }
        case Comparison::Less: {
            ZoneScopedN("Grid::shrinkColumns");
            shrinkColumns(newSize.columns);
            break;
        }
        case Comparison::Equal: break;
    }

    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
    {
        case Comparison::Greater: {
            ZoneScopedN("Grid::growLines");
            auto const moved = growLines(newSize.lines, cursors.front());
            for (auto& cursor: cursors)
                cursor += moved;
            break;
        }
        case Comparison::Less: {
            ZoneScopedN("Grid::shrinkLines");
            auto const moved = shrinkLines(newSize.lines, cursors.front());
            for (auto& cursor: cursors)
                cursor += moved;
            break;
        }
        case Comparison::Equal: break;
    }

    Ensures(_pageSize == newSize);
    // Any page-size change makes a mirror resync, so the generation bumps either way. Stable ids are
    // the narrower question: growLines/shrinkLines rotate through the ring primitives that carry the
    // id accounting, so a pure LINE-count change leaves every row naming what it named -- which is
    // what lets a collapsed fold survive a status line appearing or a window growing taller. A column
    // change is the one that rebuilds the ring.
    bumpGeneration(columnsChanged ? RowIdentity::Destroyed : RowIdentity::Preserved);
    verifyState();
}

void Grid::clampHistory()
{
    // No headroom means no trade to make: the scrollback evicts line-wise, exactly as it always has.
    // An unbounded scrollback evicts nothing at all, and without that second test the ring's current
    // allocation would read as the capacity and the trim would start cutting the very history
    // `infinite` promises to keep. (Not implied by the first: a finite guarantee under an infinite
    // capacity does have headroom.)
    if (!_historyLimits.hasHeadroom() || std::holds_alternative<Infinite>(_historyLimits.capacity))
        return;

    // Also covers the zero-history grid -- every page but the primary one -- which is what makes the
    // monotone-floor reasoning below unconditional rather than true-by-audit: rotateBuffersRight has
    // a zero-history branch that LOWERS _stableFloor, and it is the only path that ever does.
    auto const historyLineCount = this->historyLineCount();
    auto const guaranteed = guaranteedHistoryLineCount();
    if (historyLineCount <= guaranteed)
        return;

    // Rows above `bound` may go; a block start AT bound leaves exactly the guarantee. Clamped to -1
    // because offset 0 is the page, not the scrollback.
    auto const bound = std::min(LineOffset::cast_from(-unbox<int>(guaranteed)), LineOffset(-1));

    // The scan stops at addressableTop(), the top every walk over the rows this grid has starts from:
    // a row above it is no row at all, whatever LineFlag::Marked its slot last held, and reading one as
    // a block start would evict to a prompt that is not there. @see addressableTop.
    auto const top = addressableTop();
    if (bound < top)
        return;

    auto const boundId = stableLineIdOf(bound);

    // Examine only the rows that have BECOME eligible since the last call, remembering the newest
    // boundary rather than searching for it again.
    //
    // Rescanning the window each time is what makes the no-boundary case quadratic, and that case is
    // not rare -- it is every shell without OSC 133 integration, and every command whose output is
    // longer than the whole headroom. Once the buffer sits at capacity EVERY scroll asks again, and
    // only a SUCCESSFUL trim buys the headroom back, so an unsuccessful one would re-walk the same
    // thousands of rows per line forever. Measured at 12x the scroll cost before this.
    //
    // A row becomes eligible exactly when `bound` reaches its id, so watching the rows that cross
    // `bound` observes every one of them, once. The first call has nothing remembered and sweeps the
    // window whole; after that the work is proportional to the lines scrolled, not to the depth.
    auto& scan = _evictionScan.has_value() ? *_evictionScan : _evictionScan.emplace(boundId, std::nullopt);
    auto const scanFrom = std::max(top, LineOffset::cast_from(scan.examinedThrough + 1 - _stableBase));
    for (auto y = scanFrom; y <= bound; ++y)
        if (lineAt(y).marked())
            scan.newestBlockStart = stableLineIdOf(y);
    scan.examinedThrough = std::max(scan.examinedThrough, boundId);

    if (!scan.newestBlockStart)
        return;

    // An unscroll walks _stableBase -- and with it `bound` -- BACKWARDS, so what was eligible a
    // moment ago may not be now. Trimming to it would retain less than the guarantee. Skipping is
    // enough: the rows are still there and become eligible again as the base advances.
    if (*scan.newestBlockStart > boundId)
        return;

    // Evicted by the line-wise path since it was recorded, so it names no row any more.
    auto const blockStart = lineOffsetOf(*scan.newestBlockStart);
    if (!blockStart)
    {
        scan.newestBlockStart.reset();
        return;
    }

    // Measured from the oldest history row, which is where addressableTop() starts. Zero when the last
    // trim already reached this boundary.
    auto const dropCount = LineCount::cast_from(unbox<int>(*blockStart) + unbox<int>(historyLineCount));
    if (!dropCount)
        return;

    // The dropped rows are deliberately NOT reset here, though they still hold their cells and their
    // Marked flag. Both are unreachable the moment _linesUsed shrinks: syncStableFloor() puts the
    // floor exactly at the new oldest row, addressableTop() is the floor, and every walk over "the rows this
    // grid has" starts there -- so no scan, no render and no snapshot can see them. A slot coming back into
    // use is re-initialised on the way: scrollUp's below-capacity branch fill_n's a fresh Line into it, and
    // its at-capacity branch reset()s it.
    //
    // Resetting anyway would be pure cost. It does not lower the high-water mark, because at capacity
    // every one of the ring's rows is materialised regardless; it only frees a row's cells slightly
    // earlier than its reuse would. Measured, it was the single most expensive thing this function
    // did -- roughly doubling the per-line scroll cost of a marked buffer.
    //
    // The whole eviction, in two lines: the ring keeps its slots and its rotation, the rows simply
    // stop being counted, and the freed slots rejoin the free region scrollUp appends into. No
    // generation bump -- every surviving row keeps the id it had, and the rising floor is the
    // signal consumers already watch for an eviction (@see FoldState::prune).
    _linesUsed -= dropCount;
    syncStableFloor();
    verifyState();
}

// }}}
// {{{ dumpGrid impl
std::ostream& dumpGrid(std::ostream& os, Grid const& grid)
{
    os << std::format(
        "main page lines: scrollback cur {} max {}, main page lines {}, used lines {}, zero index {}\n",
        grid.historyLineCount(),
        grid.maxHistoryLineCount(),
        grid.pageSize().lines,
        grid.linesUsed(),
        grid.zeroIndex());

    for (int const lineOffset:
         std::views::iota(-unbox(grid.historyLineCount()), unbox(grid.pageSize().lines)))
    {
        Line const& lineAttribs = grid.lineAt(LineOffset(lineOffset));

        os << std::format("[{:>2}] \"{}\" | {}\n",
                          lineOffset,
                          grid.lineText(LineOffset::cast_from(lineOffset)),
                          lineAttribs.flags());
    }

    return os;
}

std::string dumpGrid(Grid const& grid)
{
    std::stringstream sstr;
    dumpGrid(sstr, grid);
    return sstr.str();
}
// }}}

namespace
{
    /// The grid as a word walk sees it: a word ends at an empty cell or at one of @c _delimiters.
    class GridWordCells
    {
      public:
        GridWordCells(Grid const& grid, u32string_view delimiters):
            _grid { &grid }, _delimiters { delimiters }
        {
        }

        [[nodiscard]] PageSize pageSize() const noexcept { return _grid->pageSize(); }
        [[nodiscard]] bool continuesRowAbove(LineOffset line) const noexcept
        {
            return _grid->continuesRowAbove(line);
        }
        [[nodiscard]] bool isWordDelimiter(CellLocation position) const noexcept
        {
            return _grid->cellEmptyOrContainsOneOf(position, _delimiters);
        }

      private:
        Grid const* _grid;
        u32string_view _delimiters;
    };
} // namespace

CellLocationRange Grid::wordRangeUnderCursor(CellLocation position,
                                             u32string_view wordDelimiters) const noexcept
{
    auto const cells = GridWordCells(*this, wordDelimiters);
    return CellLocationRange { .first = wordBegin(cells, position), .second = wordEnd(cells, position) };
}

bool Grid::cellEmptyOrContainsOneOf(CellLocation position, u32string_view delimiters) const noexcept
{
    position.column = min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    auto const flags = at(position.line, position.column).flags();

    // Padding is no text and ends no word: the word goes on where its wide character went, on the row
    // that continues this one.
    if (flags.contains(CellFlag::WrapPadding))
        return !isLineWrapped(position.line + 1);

    // A wide character's second half is part of that character, and delimits only where it does.
    if (flags.contains(CellFlag::WideCharContinuation) && !flags.contains(CellFlag::MulticellContinuation))
        while (position.column > ColumnOffset(0)
               && at(position.line, position.column).isFlagEnabled(CellFlag::WideCharContinuation))
            --position.column;

    auto cell = at(position.line, position.column);
    return CellUtil::empty(cell) || delimiters.contains(cell.codepoint(0));
}

u32string Grid::extractText(CellLocationRange range) const noexcept
{
    if (range.first.line > range.second.line)
        std::swap(range.first, range.second);

    auto const rightMargin = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    auto output = u32string {};
    for (auto const lineNumber: std::views::iota(unbox(range.first.line), unbox(range.second.line) + 1))
    {
        auto const line = LineOffset(lineNumber);
        // A row that continues the one above after a soft wrap continues its text; any other begins a line.
        if (line != range.first.line && !isLineWrapped(line))
            output += U'\n';

        auto const from = line == range.first.line ? range.first.column : ColumnOffset(0);
        auto const to = line == range.second.line ? min(range.second.column, rightMargin) : rightMargin;
        for (auto const column: std::views::iota(unbox(from), max(unbox(from), unbox(to) + 1)))
        {
            auto cell = at(line, ColumnOffset(column));
            if (cell.flags().any(NoTextCellFlags))
                continue; // no text of its own: as a selection copies it
            if (cell.codepointCount() != 0)
                output += cell.codepoints();
            else
                output += U' ';
        }
    }

    return output;
}

} // end namespace vtbackend
