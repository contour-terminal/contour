// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Grid.h>
#include <vtbackend/primitives.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <algorithm>
#include <format>
#include <iostream>
#include <ranges>

using std::max;
using std::min;
using std::u32string;
using std::u32string_view;
using std::vector;

namespace vtbackend
{

auto const inline gridLog = logstore::category(
    "vt.grid", "Grid related", logstore::category::state::Disabled, logstore::category::visibility::Hidden);

namespace detail
{
    Lines createLines(PageSize pageSize,
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

    /// Splits a logical line (stored as a LineSoA) into fixed-width Line objects.
    /// @returns number of inserted lines.
    LineCount addNewWrappedLines(Lines& targetLines,
                                 ColumnCount newColumnCount,
                                 LineSoA&& logicalLineBuffer,
                                 size_t usedColumns,
                                 LineFlags baseFlags,
                                 bool initialNoWrap)
    {
        auto const newCols = unbox<size_t>(newColumnCount);
        int i = 0;
        size_t offset = 0;

        while (offset + newCols <= usedColumns)
        {
            auto const wrappedFlag = i == 0 && initialNoWrap ? LineFlag::None : LineFlag::Wrapped;
            LineSoA chunk;
            initializeLineSoA(chunk, newColumnCount);
            copyColumns(logicalLineBuffer, offset, chunk, 0, newCols);
            targetLines.emplace_back(baseFlags | wrappedFlag, std::move(chunk), newColumnCount);
            offset += newCols;
            ++i;
        }

        if (offset < usedColumns)
        {
            auto const wrappedFlag = i == 0 && initialNoWrap ? LineFlag::None : LineFlag::Wrapped;
            auto const remaining = usedColumns - offset;
            LineSoA chunk;
            initializeLineSoA(chunk, newColumnCount);
            copyColumns(logicalLineBuffer, offset, chunk, 0, remaining);
            targetLines.emplace_back(baseFlags | wrappedFlag, std::move(chunk), newColumnCount);
            ++i;
        }
        return LineCount::cast_from(i);
    }

} // namespace detail
// {{{ Grid impl
Grid::Grid(PageSize pageSize, bool reflowOnResize, MaxHistoryLineCount maxHistoryLineCount):
    _pageSize { pageSize },
    _reflowOnResize { reflowOnResize },
    _historyLimit { maxHistoryLineCount },
    _lines { detail::createLines(
        pageSize,
        [maxHistoryLineCount]() -> LineCount {
            if (auto const* maxLineCount = std::get_if<LineCount>(&maxHistoryLineCount))
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

void Grid::setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount)
{
    verifyState();
    rezeroBuffers();
    _historyLimit = maxHistoryLineCount;
    _lines.resize(unbox<size_t>(_pageSize.lines + this->maxHistoryLineCount()));
    _linesUsed = min(_linesUsed, _pageSize.lines + this->maxHistoryLineCount());
    verifyState();
}

void Grid::clearHistory()
{
    _linesUsed = _pageSize.lines;
    verifyState();
}

void Grid::verifyState() const noexcept
{
#if !defined(NDEBUG)
    Require(LineCount::cast_from(_lines.size()) >= totalLineCount());
    Require(LineCount::cast_from(_lines.size()) >= _linesUsed);
    Require(_linesUsed >= _pageSize.lines);
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

Line& Grid::lineAt(LineOffset line) noexcept
{
    return _lines[unbox<long>(line)];
}

Line const& Grid::lineAt(LineOffset line) const noexcept
{
    return const_cast<Grid&>(*this).lineAt(line);
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

gsl::span<Line> Grid::pageAtScrollOffset(ScrollOffset scrollOffset)
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    Line* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<Line> { startLine, count };
}

gsl::span<Line const> Grid::pageAtScrollOffset(ScrollOffset scrollOffset) const
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    Line const* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<Line const> { startLine, count };
}

gsl::span<Line const> Grid::mainPage() const
{
    return pageAtScrollOffset({});
}

gsl::span<Line> Grid::mainPage()
{
    return pageAtScrollOffset({});
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
    auto const linesAvailable = LineCount::cast_from(_lines.size() - unbox<size_t>(_linesUsed));
    if (std::holds_alternative<Infinite>(_historyLimit) && linesAvailable < linesCountToScrollUp)
    {
        auto const linesToAllocate = unbox(linesCountToScrollUp - linesAvailable);

        for ([[maybe_unused]] auto const _: std::views::iota(0, linesToAllocate))
            _lines.emplace_back(_pageSize.columns, defaultLineFlags(), GraphicsAttributes());

        return scrollUp(linesCountToScrollUp, defaultAttributes);
    }
    if (unbox<size_t>(_linesUsed) == _lines.size())
    {
        rotateBuffersLeft(linesCountToScrollUp);

        for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
             y < boxed_cast<LineOffset>(_pageSize.lines);
             ++y)
            lineAt(y).reset(defaultLineFlags(), defaultAttributes);

        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(_linesUsed) < _lines.size());

        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            _linesUsed += linesAppendCount;
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

            for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
                 y < boxed_cast<LineOffset>(_pageSize.lines);
                 ++y)
                lineAt(y).reset(defaultLineFlags(), defaultAttributes);
        }
        return LineCount::cast_from(linesAppendCount);
    }
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
            copyColumns(sourceLine.storage(), fromCol, targetLine.storage(), fromCol, columnsToMove);
        }

        for (LineOffset line = margin.vertical.to - *n2 + 1; line <= margin.vertical.to; ++line)
        {
            auto& targetLine = lineAt(line);
            auto const fromCol = unbox<size_t>(margin.horizontal.from);
            clearRange(
                targetLine.storage(), fromCol, unbox<size_t>(margin.horizontal.length()), defaultAttributes);
        }
    }
    verifyState();
    return LineCount(0);
}

void Grid::scrollDown(LineCount vN, GraphicsAttributes const& defaultAttributes, Margin const& margin)
{
    verifyState();
    Require(vN >= LineCount(0));

    auto const fullHorizontal =
        margin.horizontal
        == Margin::Horizontal { .from = ColumnOffset { 0 },
                                .to = unbox<ColumnOffset>(_pageSize.columns) - ColumnOffset(1) };
    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { .from = LineOffset(0),
                              .to = unbox<LineOffset>(_pageSize.lines) - LineOffset(1) };

    auto const n = std::min(vN, margin.vertical.length());

    if (fullHorizontal && fullVertical)
    {
        rotateBuffersRight(n);

        for (Line& line: mainPage().subspan(0, unbox<size_t>(n)))
            line.reset(defaultLineFlags(), defaultAttributes);
        return;
    }

    if (fullHorizontal)
    {
        auto a = std::next(begin(_lines), *margin.vertical.from);
        auto b = std::next(begin(_lines), *margin.vertical.to + 1 - *n);
        auto c = std::next(begin(_lines), *margin.vertical.to + 1);
        std::rotate(a, b, c);
        for (auto const i: std::views::iota(*margin.vertical.from, *margin.vertical.from + *n))
            _lines[i].reset(defaultLineFlags(), defaultAttributes);
    }
    else
    {
        if (n <= margin.vertical.length())
        {
            for (LineOffset line = margin.vertical.to; line >= margin.vertical.to - *n; --line)
            {
                auto const& srcLine = lineAt(line - *n);
                auto& dstLine = lineAt(line);
                auto const fromCol = unbox<size_t>(margin.horizontal.from);
                copyColumns(srcLine.storage(),
                            fromCol,
                            dstLine.storage(),
                            fromCol,
                            unbox<size_t>(margin.horizontal.length()));
            }

            for (LineOffset line = margin.vertical.from; line < margin.vertical.from + *n; ++line)
            {
                auto& targetLine = lineAt(line);
                auto const fromCol = unbox<size_t>(margin.horizontal.from);
                clearRange(targetLine.storage(),
                           fromCol,
                           unbox<size_t>(margin.horizontal.length()),
                           defaultAttributes);
            }
        }
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
    }

    auto const remaining = clampedN - pullable;
    if (*remaining > 0)
    {
        rotateBuffersRight(remaining);
        for (auto& line: mainPage().subspan(0, unbox<size_t>(remaining)))
            line.reset(defaultLineFlags(), defaultAttributes);
    }
    verifyState();
}

void Grid::scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    for (LineOffset lineNo = margin.vertical.from; lineNo <= margin.vertical.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto& storage = line.storage();
        auto const from = unbox<size_t>(margin.horizontal.from);
        auto const to = unbox<size_t>(margin.horizontal.to) + 1;
        auto const count = to - from;
        if (count > 1)
        {
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
    _lines.rotate_right(_lines.zero_index());
    for (int i = 0; i < unbox(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {});
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

    _pageSize.lines += totalLinesToExtend;
    _linesUsed = min(_linesUsed + totalLinesToExtend, LineCount::cast_from(_lines.size()));

    Ensures(_pageSize.lines == newHeight);
    Ensures(_lines.size() >= unbox<size_t>(maxHistoryLineCount() + _pageSize.lines));
    verifyState();

    return cursorMove;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
CellLocation Grid::resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending)
{
    if (_pageSize == newSize)
        return currentCursorPos;

    gridLog()("resize {} -> {} (cursor {})", _pageSize, newSize, currentCursorPos);

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
            Ensures(*cursor.line < *_pageSize.lines);
            verifyState();
        }

        Require(newHeight <= _pageSize.lines);
        if (*numLinesToPushUp)
        {
            gridLog()(" -> numLinesToPushUp {}", numLinesToPushUp);
            Require(*cursor.line + 1 == *_pageSize.lines);
            rotateBuffersLeft(numLinesToPushUp);
            _pageSize.lines -= numLinesToPushUp;
            clampHistory();
            verifyState();
            return CellLocation { .line = -boxed_cast<LineOffset>(numLinesToPushUp), .column = {} };
        }

        verifyState();
        return CellLocation {};
    };

    auto const growColumns = [this, wrapPending](ColumnCount newColumnCount) -> CellLocation {
        if (!_reflowOnResize)
        {
            for (auto& line: _lines)
                if (line.size() < newColumnCount)
                    line.resize(newColumnCount);
            _pageSize.columns = newColumnCount;
            verifyState();
            return CellLocation { .line = LineOffset(0), .column = ColumnOffset(wrapPending ? 1 : 0) };
        }
        else
        {
            auto const extendCount = newColumnCount - _pageSize.columns;
            Require(*extendCount > 0);

            Lines grownLines;
            LineSoA logicalLineBuffer;
            initializeLineSoA(logicalLineBuffer, ColumnCount(0));
            size_t logicalLineUsed = 0;
            LineFlags logicalLineFlags = LineFlag::None;

            auto const appendToLogicalLine = [&logicalLineBuffer, &logicalLineUsed](Line const& line) {
                auto const cols = unbox<size_t>(line.size());
                auto const used = trimBlankRight(line.storage(), cols);
                if (used > 0)
                {
                    auto const newSize = logicalLineUsed + used;
                    resizeLineSoA(logicalLineBuffer, ColumnCount::cast_from(newSize));
                    copyColumns(line.storage(), 0, logicalLineBuffer, logicalLineUsed, used);
                    logicalLineUsed = newSize;
                }
            };

            auto const flushLogicalLine =
                [newColumnCount, &grownLines, &logicalLineBuffer, &logicalLineUsed, &logicalLineFlags]() {
                    if (logicalLineUsed > 0)
                    {
                        detail::addNewWrappedLines(grownLines,
                                                   newColumnCount,
                                                   std::move(logicalLineBuffer),
                                                   logicalLineUsed,
                                                   logicalLineFlags,
                                                   true);
                        initializeLineSoA(logicalLineBuffer, ColumnCount(0));
                        logicalLineUsed = 0;
                    }
                };

            for (int i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];
                Require(line.size() >= _pageSize.columns);

                if (line.wrapped())
                {
                    appendToLogicalLine(line);
                }
                else
                {
                    flushLogicalLine();
                    if (line.empty())
                    {
                        line.resize(newColumnCount);
                        grownLines.emplace_back(std::move(line));
                    }
                    else
                    {
                        appendToLogicalLine(line);
                        logicalLineFlags = line.flags().without(LineFlag::Wrapped);
                    }
                }
            }

            flushLogicalLine();

            auto cy = LineCount(0);
            if (_pageSize.lines > LineCount::cast_from(grownLines.size()))
            {
                cy = _pageSize.lines - LineCount::cast_from(grownLines.size());
                while (LineCount::cast_from(grownLines.size()) < _pageSize.lines)
                    grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

                Ensures(LineCount::cast_from(grownLines.size()) == _pageSize.lines);
            }

            _linesUsed = LineCount::cast_from(grownLines.size());

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(newColumnCount, defaultLineFlags(), GraphicsAttributes {});

            _lines = std::move(grownLines);
            _pageSize.columns = newColumnCount;

            auto const newHistoryLineCount = _linesUsed - _pageSize.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return CellLocation { .line = -boxed_cast<LineOffset>(cy),
                                  .column = ColumnOffset(wrapPending ? 1 : 0) };
        }
    };

    auto const shrinkColumns =
        [this](ColumnCount newColumnCount, LineCount /*newLineCount*/, CellLocation cursor) -> CellLocation {
        if (!_reflowOnResize)
        {
            _pageSize.columns = newColumnCount;
            crispy::for_each(_lines, [=](Line& line) {
                if (newColumnCount < line.size())
                    line.resize(newColumnCount);
            });
            verifyState();
            return cursor + std::min(cursor.column, boxed_cast<ColumnOffset>(newColumnCount));
        }
        else
        {
            Lines shrinkedLines;
            LineSoA wrappedColumns;
            initializeLineSoA(wrappedColumns, ColumnCount(0));
            size_t wrappedUsed = 0;
            LineFlags previousFlags = _lines.front().inheritableFlags();

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            shrinkedLines.reserve(totalLineCount);
            Require(totalLineCount == unbox<size_t>(this->totalLineCount()));

            auto numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];

                if (wrappedUsed > 0)
                {
                    if (line.wrapped() && line.inheritableFlags() == previousFlags)
                    {
                        // Prepend wrapped columns to this line using SoA copyColumns
                        auto const cols = unbox<size_t>(line.size());
                        auto const used = trimBlankRight(line.storage(), cols);
                        auto const totalCols = wrappedUsed + used;
                        LineSoA merged;
                        initializeLineSoA(merged, ColumnCount::cast_from(totalCols));
                        copyColumns(wrappedColumns, 0, merged, 0, wrappedUsed);
                        if (used > 0)
                            copyColumns(line.storage(), 0, merged, wrappedUsed, used);
                        line = Line(line.flags(), std::move(merged), ColumnCount::cast_from(totalCols));
                    }
                    else
                    {
                        auto const numLinesInserted = detail::addNewWrappedLines(shrinkedLines,
                                                                                 newColumnCount,
                                                                                 std::move(wrappedColumns),
                                                                                 wrappedUsed,
                                                                                 previousFlags,
                                                                                 false);
                        numLinesWritten += numLinesInserted;
                        previousFlags = line.inheritableFlags();
                    }
                    wrappedUsed = 0;
                    initializeLineSoA(wrappedColumns, ColumnCount(0));
                }
                else
                {
                    line.setWrappable(true);
                    previousFlags = line.inheritableFlags();
                }

                auto overflow = line.reflow(newColumnCount);
                auto const overflowCols = overflow.codepoints.size();
                if (overflowCols > 0)
                {
                    wrappedColumns = std::move(overflow);
                    wrappedUsed = trimBlankRight(wrappedColumns, overflowCols);
                }

                shrinkedLines.emplace_back(std::move(line));
                numLinesWritten++;
                Ensures(shrinkedLines.back().size() >= newColumnCount);
            }
            if (wrappedUsed > 0)
            {
                numLinesWritten += detail::addNewWrappedLines(shrinkedLines,
                                                              newColumnCount,
                                                              std::move(wrappedColumns),
                                                              wrappedUsed,
                                                              previousFlags,
                                                              false);
            }
            Require(unbox<size_t>(numLinesWritten) == shrinkedLines.size());
            Require(numLinesWritten >= _pageSize.lines);

            while (shrinkedLines.size() < totalLineCount)
                shrinkedLines.emplace_back(newColumnCount, LineFlag::None, GraphicsAttributes {});

            shrinkedLines.rotate_left(unbox<size_t>(numLinesWritten - _pageSize.lines));
            _linesUsed = LineCount::cast_from(numLinesWritten);

            _lines = std::move(shrinkedLines);
            _pageSize.columns = newColumnCount;

            verifyState();
            return cursor; // TODO
        }
    };
    // }}}

    CellLocation cursor = currentCursorPos;

    using crispy::comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
    {
        case comparison::Greater: cursor += growColumns(newSize.columns); break;
        case comparison::Less: cursor = shrinkColumns(newSize.columns, newSize.lines, cursor); break;
        case comparison::Equal: break;
    }

    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
    {
        case comparison::Greater: cursor += growLines(newSize.lines, cursor); break;
        case comparison::Less: cursor += shrinkLines(newSize.lines, cursor); break;
        case comparison::Equal: break;
    }

    Ensures(_pageSize == newSize);
    verifyState();

    return cursor;
}

void Grid::clampHistory()
{
    // TODO: needed?
}

void Grid::appendNewLines(LineCount count, GraphicsAttributes attr)
{
    auto const wrappableFlag = _lines.back().wrappableFlag();

    if (historyLineCount() == maxHistoryLineCount())
    {
        for (int i = 0; i < unbox(count); ++i)
        {
            auto line = std::move(_lines.front());
            _lines.pop_front();
            line.reset(defaultLineFlags(), attr);
            _lines.emplace_back(std::move(line));
        }
        return;
    }

    if (auto const n = std::min(count, _pageSize.lines); *n > 0)
    {
        generate_n(back_inserter(_lines), *n, [&]() { return Line(_pageSize.columns, wrappableFlag, attr); });
        clampHistory();
    }
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
        grid.zero_index());

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

CellLocationRange Grid::wordRangeUnderCursor(CellLocation position,
                                             u32string_view wordDelimiters) const noexcept
{
    auto const left = [this, wordDelimiters, position]() {
        auto last = position;
        auto current = last;

        for (;;)
        {
            auto const wrapIntoPreviousLine =
                *current.column == 0 && *current.line > 0 && isLineWrapped(current.line);
            if (*current.column > 0)
                current.column--;
            else if (*current.line > 0 || wrapIntoPreviousLine)
            {
                current.line--;
                current.column = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
            }
            else
                break;

            if (cellEmptyOrContainsOneOf(current, wordDelimiters))
                break;
            last = current;
        }
        return last;
    }();

    auto const right = [this, wordDelimiters, position]() {
        auto last = position;
        auto current = last;

        for (;;)
        {
            if (*current.column == *pageSize().columns - 1 && *current.line + 1 < *pageSize().lines
                && isLineWrapped(current.line))
            {
                current.line++;
                current.column = ColumnOffset(0);
                current =
                    stretchedColumn(CellLocation { .line = current.line, .column = current.column + 1 });
            }

            if (*current.column + 1 < *pageSize().columns)
            {
                current =
                    stretchedColumn(CellLocation { .line = current.line, .column = current.column + 1 });
            }
            else if (*current.line + 1 < *pageSize().lines)
            {
                current.line++;
                current.column = ColumnOffset(0);
            }
            else
                break;

            if (cellEmptyOrContainsOneOf(current, wordDelimiters))
                break;
            last = current;
        }
        return last;
    }();

    return CellLocationRange { .first = left, .second = right };
}

bool Grid::cellEmptyOrContainsOneOf(CellLocation position, u32string_view delimiters) const noexcept
{
    position.column = min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    auto cell = at(position.line, position.column);
    return CellUtil::empty(cell) || delimiters.find(cell.codepoint(0)) != std::u32string_view::npos;
}

u32string Grid::extractText(CellLocationRange range) const noexcept
{
    if (range.first.line > range.second.line)
        std::swap(range.first, range.second);

    auto output = u32string {};

    assert(range.first.line == range.second.line);

    auto const rightMargin = boxed_cast<ColumnOffset>(pageSize().columns) - 1;
    auto const numLines = LineCount::cast_from(range.second.line - range.first.line + 1);
    auto ranges = vector<ColumnRange> {};

    switch (numLines.value)
    {
        case 1:
            ranges.emplace_back(ColumnRange { .line = range.first.line,
                                              .fromColumn = range.first.column,
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
        case 2:
            ranges.emplace_back(ColumnRange {
                .line = range.first.line, .fromColumn = range.first.column, .toColumn = rightMargin });
            ranges.emplace_back(ColumnRange { .line = range.first.line,
                                              .fromColumn = ColumnOffset(0),
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
        default:
            ranges.emplace_back(ColumnRange {
                .line = range.first.line, .fromColumn = range.first.column, .toColumn = rightMargin });
            for (auto j = range.first.line + 1; j < range.second.line; ++j)
                ranges.emplace_back(
                    ColumnRange { .line = j, .fromColumn = ColumnOffset(0), .toColumn = rightMargin });
            ranges.emplace_back(ColumnRange { .line = range.second.line,
                                              .fromColumn = ColumnOffset(0),
                                              .toColumn = min(range.second.column, rightMargin) });
            break;
    }

    for (auto const& range: ranges)
    {
        if (!output.empty())
            output += '\n';
        for (auto column = range.fromColumn; column <= range.toColumn; ++column)
        {
            auto cell = at(range.line, column);
            if (cell.codepointCount() != 0)
                output += cell.codepoints();
            else
                output += L' ';
        }
    }

    return output;
}

} // end namespace vtbackend
