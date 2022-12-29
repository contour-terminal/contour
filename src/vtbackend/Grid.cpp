/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtbackend/Grid.h>
#include <vtbackend/primitives.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <fmt/format.h>

#include <algorithm>
#include <iostream>

using std::max;
using std::min;
using std::u32string;
using std::u32string_view;
using std::vector;

namespace terminal
{

auto const inline GridLog = logstore::Category(
    "vt.grid", "Grid related", logstore::Category::State::Disabled, logstore::Category::Visibility::Hidden);

namespace detail
{
    template <typename... Args>
    void logf([[maybe_unused]] Args&&... args)
    {
#if 1 // !defined(NDEBUG)
        fmt::print(std::forward<Args>(args)...);
        fmt::print("\n");
#endif
    }

    template <typename Cell>
    gsl::span<Cell> trimRight(gsl::span<Cell> cells)
    {
        size_t n = cells.size();
        while (n && CellUtil::empty(cells[n - 1]))
            --n;
        return cells.subspan(0, n);
    }

    template <typename Cell>
    Lines<Cell> createLines(PageSize pageSize,
                            LineCount maxHistoryLineCount,
                            bool reflowOnResize,
                            GraphicsAttributes initialSGR)
    {
        auto const defaultLineFlags = reflowOnResize ? LineFlags::Wrappable : LineFlags::None;
        auto const totalLineCount = unbox<size_t>(pageSize.lines + maxHistoryLineCount);

        Lines<Cell> lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: ranges::views::iota(0u, totalLineCount))
            lines.emplace_back(defaultLineFlags, TrivialLineBuffer { pageSize.columns, initialSGR });

        return lines;
    }

    /**
     * Appends logical line by splitting into fixed-width lines.
     *
     * @param targetLines
     * @param newColumnCount
     * @param logicalLineBuffer
     * @param baseFlags
     * @param initialNoWrap
     *
     * @returns number of inserted lines
     */
    template <typename Cell>
    LineCount addNewWrappedLines(
        Lines<Cell>& targetLines,
        ColumnCount newColumnCount,
        typename Line<Cell>::InflatedBuffer&& logicalLineBuffer, // TODO: don't move, do (c)ref instead
        LineFlags baseFlags,
        bool initialNoWrap // TODO: pass `LineFlags defaultLineFlags` instead?
    )
    {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        // TODO: avoid unnecessary copies via erase() by incrementally updating (from, to)
        int i = 0;

        while (logicalLineBuffer.size() >= unbox<size_t>(newColumnCount))
        {
            auto from = logicalLineBuffer.begin();
            auto to = from + newColumnCount.as<std::ptrdiff_t>();
            auto const wrappedFlag = i == 0 && initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            targetLines.emplace_back(baseFlags | wrappedFlag, LineBuffer(from, to));
            logicalLineBuffer.erase(from, to);
            ++i;
        }

        if (logicalLineBuffer.size() > 0)
        {
            auto const wrappedFlag = i == 0 && initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            ++i;
            logicalLineBuffer.resize(unbox<size_t>(newColumnCount));
            targetLines.emplace_back(baseFlags | wrappedFlag, std::move(logicalLineBuffer));
        }
        return LineCount::cast_from(i);
    }

} // namespace detail
// {{{ Grid impl
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Grid<Cell>::Grid(PageSize pageSize, bool reflowOnResize, MaxHistoryLineCount maxHistoryLineCount):
    _pageSize { pageSize },
    _margin { Margin::Vertical { {}, _pageSize.lines.as<LineOffset>() - LineOffset(1) },
              Margin::Horizontal { {}, _pageSize.columns.as<ColumnOffset>() - ColumnOffset(1) } },
    _reflowOnResize { reflowOnResize },
    _historyLimit { maxHistoryLineCount },
    _lines { detail::createLines<Cell>(
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

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount)
{
    verifyState();
    rezeroBuffers();
    _historyLimit = maxHistoryLineCount;
    _lines.resize(unbox<size_t>(_pageSize.lines + this->maxHistoryLineCount()));
    _linesUsed = min(_linesUsed, _pageSize.lines + this->maxHistoryLineCount());
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::clearHistory()
{
    _linesUsed = _pageSize.lines;
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::verifyState() const noexcept
{
#if !defined(NDEBUG)
    // _maxHistoryLineCount + _pageSize.lines
    Require(LineCount::cast_from(_lines.size()) >= totalLineCount());
    Require(LineCount::cast_from(_lines.size()) >= _linesUsed);
    Require(_linesUsed >= _pageSize.lines);
#endif
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::renderAllText() const
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

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::renderMainPageText() const
{
    std::string text;

    for (auto line = LineOffset(0); line < unbox<LineOffset>(_pageSize.lines); ++line)
    {
        text += lineText(line);
        text += '\n';
    }

    return text;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Line<Cell>& Grid<Cell>::lineAt(LineOffset line) noexcept
{
    // Require(*line < *_pageSize.lines);
    return _lines[unbox<long>(line)];
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Line<Cell> const& Grid<Cell>::lineAt(LineOffset line) const noexcept
{
    // Require(*line < *_pageSize.lines);
    return const_cast<Grid&>(*this).lineAt(line);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& Grid<Cell>::at(LineOffset line, ColumnOffset column) noexcept
{
    return useCellAt(line, column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& Grid<Cell>::useCellAt(LineOffset line, ColumnOffset column) noexcept
{
    return lineAt(line).useCellAt(column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell const& Grid<Cell>::at(LineOffset line, ColumnOffset column) const noexcept
{
    return const_cast<Grid&>(*this).at(line, column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell>> Grid<Cell>::pageAtScrollOffset(ScrollOffset scrollOffset)
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    Line<Cell>* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<Line<Cell>> { startLine, count };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell> const> Grid<Cell>::pageAtScrollOffset(ScrollOffset scrollOffset) const
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    Line<Cell> const* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<Line<Cell> const> { startLine, count };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell> const> Grid<Cell>::mainPage() const
{
    return pageAtScrollOffset({});
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell>> Grid<Cell>::mainPage()
{
    return pageAtScrollOffset({});
}
// }}}
// {{{ Grid impl: Line access
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Cell const> Grid<Cell>::lineBufferRightTrimmed(LineOffset line) const noexcept
{
    return detail::trimRight(lineBuffer(line));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineText(LineOffset lineOffset) const
{
    std::string line;
    line.reserve(unbox<size_t>(_pageSize.columns));

    for (Cell const& cell: lineBuffer(lineOffset))
        if (cell.codepointCount())
            line += cell.toUtf8();
        else
            line += ' '; // fill character

    return line;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineTextTrimmed(LineOffset lineOffset) const
{
    std::string output = lineText(lineOffset);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineText(Line<Cell> const& line) const
{
    std::stringstream sstr;
    for (Cell const& cell: line.inflatedBuffer())
    {
        if (cell.codepointCount() == 0)
            sstr << ' ';
        else
            sstr << cell.toUtf8();
    }
    return sstr.str();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::setLineText(LineOffset line, std::string_view text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(text))
        useCellAt(line, ColumnOffset::cast_from(i++)).setCharacter(ch);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Grid<Cell>::isLineBlank(LineOffset line) const noexcept
{
    auto const is_blank = [](auto const& cell) noexcept {
        return CellUtil::empty(cell);
    };

    auto const theLineBuffer = lineBuffer(line);
    return std::all_of(theLineBuffer.begin(), theLineBuffer.end(), is_blank);
}

/**
 * Computes the relative line number for the bottom-most @p n logical lines.
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
int Grid<Cell>::computeLogicalLineNumberFromBottom(LineCount n) const noexcept
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

    // XXX If the top-most logical line is reached, we still need to traverse upwards until the
    // beginning of the top-most logical line (the one that does not have the wrapped-flag set).
    while (i != _lines.rend() && i->wrapped())
    {
        // printf("further upwards: l %d, p %d\n", logicalLineCount, outputRelativePhysicalLine);
        outputRelativePhysicalLine--;
        ++i;
    }

    return outputRelativePhysicalLine;
}
// }}}
// {{{ Grid impl: scrolling
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
LineCount Grid<Cell>::scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes) noexcept
{
    verifyState();
    if (unbox<size_t>(_linesUsed) == _lines.size()) // with all grid lines in-use
    {
        if (std::get_if<Infinite>(&_historyLimit))
        {
            for ([[maybe_unused]] auto const _: ranges::views::iota(0, unbox<int>(linesCountToScrollUp)))
                _lines.emplace_back(defaultLineFlags(),
                                    TrivialLineBuffer { _pageSize.columns, GraphicsAttributes {} });
            return scrollUp(linesCountToScrollUp, defaultAttributes);
        }
        // TODO: ensure explicit test for this case
        rotateBuffersLeft(linesCountToScrollUp);

        // Initialize (/reset) new lines.
        for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
             y < boxed_cast<LineOffset>(_pageSize.lines);
             ++y)
            lineAt(y).reset(defaultLineFlags(), defaultAttributes);

        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(_linesUsed) < _lines.size());

        // Number of lines in the ring buffer that are not yet
        // used by the grid system.
        auto const linesAvailable = LineCount::cast_from(_lines.size() - unbox<size_t>(_linesUsed));

        // Number of lines in the ring buffer that we can allocate at the head.
        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            _linesUsed += linesAppendCount;
            Require(unbox<size_t>(_linesUsed) <= _lines.size());
            fill_n(next(_lines.begin(), *_pageSize.lines),
                   unbox<size_t>(linesAppendCount),
                   Line<Cell> { defaultLineFlags(),
                                TrivialLineBuffer { _pageSize.columns, defaultAttributes } });
            rotateBuffersLeft(linesAppendCount);
        }
        if (linesAppendCount < linesCountToScrollUp)
        {
            auto const incrementCount = linesCountToScrollUp - linesAppendCount;
            rotateBuffersLeft(incrementCount);

            // Initialize (/reset) new lines.
            for (auto y = boxed_cast<LineOffset>(_pageSize.lines - linesCountToScrollUp);
                 y < boxed_cast<LineOffset>(_pageSize.lines);
                 ++y)
                lineAt(y).reset(defaultLineFlags(), defaultAttributes);
        }
        return LineCount::cast_from(linesAppendCount);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
LineCount Grid<Cell>::scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    verifyState();
    Require(0 <= *margin.horizontal.from && *margin.horizontal.to < *_pageSize.columns);
    Require(0 <= *margin.vertical.from && *margin.vertical.to < *_pageSize.lines);

    // these two booleans could be cached and updated whenever margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        margin.horizontal
        == Margin::Horizontal { ColumnOffset { 0 }, unbox<ColumnOffset>(_pageSize.columns) - 1 };
    auto const fullVertical =
        margin.vertical == Margin::Vertical { LineOffset(0), unbox<LineOffset>(_pageSize.lines) - 1 };

    if (fullHorizontal)
    {
        if (fullVertical) // full-screen scroll-up
            return scrollUp(n, defaultAttributes);

        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = LineCount(margin.vertical.length());
        auto const n2 = std::min(n, marginHeight);
        if (*n2 && n2 < marginHeight)
        {
            // rotate line attribs
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
        // a full "inside" scroll-up
        auto const marginHeight = margin.vertical.length();
        auto const n2 = std::min(n, marginHeight);
        // auto const bottomLineOffsetToCopy = min(margin.vertical.from + *n2, margin.vertical.to - 1);
        auto const topTargetLineOffset = margin.vertical.from;
        auto const bottomTargetLineOffset = margin.vertical.to - *n2;
        auto const columnsToMove = unbox<size_t>(margin.horizontal.length());

        for (LineOffset targetLineOffset = topTargetLineOffset; targetLineOffset <= bottomTargetLineOffset;
             ++targetLineOffset)
        {
            auto const sourceLineOffset = targetLineOffset + *n2;
            auto t = &useCellAt(targetLineOffset, margin.horizontal.from);
            auto s = &at(sourceLineOffset, margin.horizontal.from);
            std::copy_n(s, columnsToMove, t);
        }

        for (LineOffset line = margin.vertical.to - *n2 + 1; line <= margin.vertical.to; ++line)
        {
            auto a = &useCellAt(line, margin.horizontal.from);
            auto b = a + unbox<int>(margin.horizontal.length());
            while (a != b)
            {
                a->reset(defaultAttributes);
                a++;
            }
        }
    }
    verifyState();
    return LineCount(0); // No full-margin lines scrolled up.
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::scrollDown(LineCount vN, GraphicsAttributes const& defaultAttributes, Margin const& margin)
{
    verifyState();
    Require(vN >= LineCount(0));

    // these two booleans could be cached and updated whenever margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        margin.horizontal
        == Margin::Horizontal { ColumnOffset { 0 },
                                unbox<ColumnOffset>(_pageSize.columns) - ColumnOffset(1) };
    auto const fullVertical =
        margin.vertical
        == Margin::Vertical { LineOffset(0), unbox<LineOffset>(_pageSize.lines) - LineOffset(1) };

    auto const n = std::min(vN, margin.vertical.length());

    if (fullHorizontal && fullVertical)
    {
        // full screen scrolling

        // on the full screen (all lines)
        // move all lines up by N lines
        // bottom N lines are wiped out

        rotateBuffersRight(n);

        for (Line<Cell>& line: mainPage().subspan(0, unbox<size_t>(n)))
            line.reset(defaultLineFlags(), defaultAttributes);
        return;
    }

    if (fullHorizontal) // => but ont fully vertical
    {
        // scroll down only inside vertical margin with full horizontal extend
        auto a = std::next(begin(_lines), *margin.vertical.from);
        auto b = std::next(begin(_lines), *margin.vertical.to + 1 - *n);
        auto c = std::next(begin(_lines), *margin.vertical.to + 1);
        std::rotate(a, b, c);
        for (auto const i: ranges::views::iota(*margin.vertical.from, *margin.vertical.from + *n))
            _lines[i].reset(defaultLineFlags(), defaultAttributes);
    }
    else
    {
        // a full "inside" scroll-down
        if (n <= margin.vertical.length())
        {
            for (LineOffset line = margin.vertical.to; line >= margin.vertical.to - *n; --line)
            {
                auto s = &at(line - *n, margin.horizontal.from);
                auto t = &at(line, margin.horizontal.from);
                std::copy_n(s, unbox<size_t>(margin.horizontal.length()), t);
            }

            for (LineOffset line = margin.vertical.from; line < margin.vertical.from + *n; ++line)
            {
                auto a = &at(line, margin.horizontal.from);
                auto b = &at(line, margin.horizontal.to + 1);
                while (a != b)
                {
                    *a = Cell { defaultAttributes };
                    a++;
                }
            }
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept
{
    for (LineOffset lineNo = margin.vertical.from; lineNo <= margin.vertical.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto column0 = line.inflatedBuffer().begin() + *margin.horizontal.from;
        auto column1 = line.inflatedBuffer().begin() + *margin.horizontal.from + 1;
        auto column2 = line.inflatedBuffer().begin() + *margin.horizontal.to + 1;
        std::rotate(column0, column1, column2);

        auto const emptyCell = Cell { defaultAttributes };
        auto const emptyCellsBegin = line.inflatedBuffer().begin() + *margin.horizontal.to;
        std::fill_n(emptyCellsBegin, 1, emptyCell);
    }
}

// }}}
// {{{ Grid impl: resize
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotate_right(_lines.zero_index());
    for (int i = 0; i < unbox<int>(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {});
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
CellLocation Grid<Cell>::growLines(LineCount newHeight, CellLocation cursor)
{
    // Grow line count by splicing available lines from history back into buffer, if available,
    // or create new ones until _pageSize.lines == newHeight.

    Require(newHeight > _pageSize.lines);
    // _lines.reserve(unbox<size_t>(_maxHistoryLineCount + newHeight));

    // Pull down from history if cursor is at bottom and if scrollback available.
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
    // ? Require(linesToTakeFromSavedLines == LineCount(0));

    auto const newTotalLineCount = maxHistoryLineCount() + newHeight;
    auto const currentTotalLineCount = LineCount::cast_from(_lines.size());
    auto const linesToFill = max(0, *newTotalLineCount - *currentTotalLineCount);

    for ([[maybe_unused]] auto const _: ranges::views::iota(0, linesToFill))
        _lines.emplace_back(wrappableFlag, TrivialLineBuffer { _pageSize.columns, GraphicsAttributes {} });

    _pageSize.lines += totalLinesToExtend;
    _linesUsed = min(_linesUsed + totalLinesToExtend, LineCount::cast_from(_lines.size()));

    Ensures(_pageSize.lines == newHeight);
    Ensures(_lines.size() >= unbox<size_t>(maxHistoryLineCount() + _pageSize.lines));
    verifyState();

    return cursorMove;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
CellLocation Grid<Cell>::resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending)
{
    if (_pageSize == newSize)
        return currentCursorPos;

    GridLog()("resize {} -> {} (cursor {})", _pageSize, newSize, currentCursorPos);

    // Growing in line count with scrollback lines present will move
    // the scrollback lines into the visible area.
    //
    // Shrinking in line count with the cursor at the bottom margin will move
    // the top lines into the scrollback area.

    // {{{ helper methods
    auto const shrinkLines = [this](LineCount newHeight, CellLocation cursor) -> CellLocation {
        // Shrink existing line count to newSize.lines
        // by splicing the number of lines to be shrinked by into savedLines bottom.

        Require(newHeight < _pageSize.lines);

        // FIXME: in alt screen, when shrinking more then available below screen cursor -> assertion failure

        auto const numLinesToShrink = _pageSize.lines - newHeight;
        auto const linesAvailableBelowCursorBeforeShrink =
            _pageSize.lines - boxed_cast<LineCount>(cursor.line + 1);
        auto const cutoffCount = min(numLinesToShrink, linesAvailableBelowCursorBeforeShrink);
        auto const numLinesToPushUp = numLinesToShrink - cutoffCount;
        auto const numLinesToPushUpCapped = min(numLinesToPushUp, maxHistoryLineCount());

        GridLog()(" -> shrink lines: numLinesToShrink {}, linesAvailableBelowCursorBeforeShrink {}, "
                  "cutoff {}, pushUp "
                  "{}/{}",
                  numLinesToShrink,
                  linesAvailableBelowCursorBeforeShrink,
                  cutoffCount,
                  numLinesToPushUp,
                  numLinesToPushUpCapped);

        Ensures(numLinesToShrink == cutoffCount + numLinesToPushUp);

        // 1.) Shrink up to the number of lines below the cursor.
        if (cutoffCount != LineCount(0))
        {
            _pageSize.lines -= cutoffCount;
            _linesUsed -= cutoffCount;
            Ensures(*cursor.line < *_pageSize.lines);
            verifyState();
        }

        // 2.) If newHeight is still below page line count, then shrink by rotating up.
        Require(newHeight <= _pageSize.lines);
        if (*numLinesToPushUp)
        {
            GridLog()(" -> numLinesToPushUp {}", numLinesToPushUp);
            Require(*cursor.line + 1 == *_pageSize.lines);
            rotateBuffersLeft(numLinesToPushUp);
            _pageSize.lines -= numLinesToPushUp;
            clampHistory();
            verifyState();
            return CellLocation { -boxed_cast<LineOffset>(numLinesToPushUp), {} };
        }

        verifyState();
        return CellLocation {};
    };

    auto const growColumns = [this, wrapPending](ColumnCount newColumnCount) -> CellLocation {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        if (!_reflowOnResize)
        {
            for (auto& line: _lines)
                if (line.size() < newColumnCount)
                    line.resize(newColumnCount);
            _pageSize.columns = newColumnCount;
            verifyState();
            return CellLocation { LineOffset(0), ColumnOffset(wrapPending ? 1 : 0) };
        }
        else
        {
            // Grow columns by inverse shrink,
            // i.e. the lines are traversed in reverse order.

            auto const extendCount = newColumnCount - _pageSize.columns;
            Require(*extendCount > 0);

            Lines<Cell> grownLines;
            LineBuffer
                logicalLineBuffer; // Temporary state, representing wrapped columns from the line "below".
            LineFlags logicalLineFlags = LineFlags::None;

            auto const appendToLogicalLine = [&logicalLineBuffer](gsl::span<Cell const> cells) {
                for (auto const& cell: cells)
                    logicalLineBuffer.push_back(cell);
            };

            auto const flushLogicalLine =
                [newColumnCount, &grownLines, &logicalLineBuffer, &logicalLineFlags]() {
                    if (!logicalLineBuffer.empty())
                    {
                        detail::addNewWrappedLines(
                            grownLines, newColumnCount, std::move(logicalLineBuffer), logicalLineFlags, true);
                        logicalLineBuffer.clear();
                    }
                };

            [[maybe_unused]] auto const logLogicalLine =
                [&logicalLineBuffer]([[maybe_unused]] LineFlags lineFlags,
                                     [[maybe_unused]] std::string_view msg) {
                    GridLog()("{} |> \"{}\"", msg, Line<Cell>(lineFlags, logicalLineBuffer).toUtf8());
                };

            for (int i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];
                // logLogicalLine(line.flags(), fmt::format("Line[{:>2}]: next line: \"{}\"", i,
                // line.toUtf8()));
                Require(line.size() >= _pageSize.columns);

                if (line.wrapped())
                {
                    // logLogicalLine(line.flags(), fmt::format(" - appending: \"{}\"",
                    // line.toUtf8Trimmed()));
                    appendToLogicalLine(line.trim_blank_right());
                }
                else // line is not wrapped
                {
                    flushLogicalLine();
                    if (line.isTrivialBuffer())
                    {
                        auto& buffer = line.trivialBuffer();
                        buffer.displayWidth = newColumnCount;
                        grownLines.emplace_back(line);
                    }
                    else
                    {
                        // logLogicalLine(line.flags(), " - start new logical line");
                        appendToLogicalLine(line.cells());
                        logicalLineFlags = line.flags() & ~LineFlags::Wrapped;
                    }
                }
            }

            flushLogicalLine(); // Flush last (bottom) line, if anything pending.

            // auto diff = int(_lines.size()) - unbox<int>(_pageSize.lines);
            auto cy = LineCount(0);
            if (_pageSize.lines > LineCount::cast_from(grownLines.size()))
            {
                // The lines we've been reflowing do not fill the pageSize,
                // so fill the gap until we have a full page.
                cy = _pageSize.lines - LineCount::cast_from(grownLines.size());
                while (LineCount::cast_from(grownLines.size()) < _pageSize.lines)
                    grownLines.emplace_back(
                        defaultLineFlags(),
                        TrivialLineBuffer { newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

                Ensures(LineCount::cast_from(grownLines.size()) == _pageSize.lines);
            }

            _linesUsed = LineCount::cast_from(grownLines.size());

            // Fill scrollback lines.
            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(
                    defaultLineFlags(),
                    TrivialLineBuffer { newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

            _lines = std::move(grownLines);
            _pageSize.columns = newColumnCount;

            auto const newHistoryLineCount = _linesUsed - _pageSize.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return CellLocation { -boxed_cast<LineOffset>(cy), ColumnOffset(wrapPending ? 1 : 0) };
        }
    };

    auto const shrinkColumns =
        [this](ColumnCount newColumnCount, LineCount /*newLineCount*/, CellLocation cursor) -> CellLocation {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        if (!_reflowOnResize)
        {
            _pageSize.columns = newColumnCount;
            crispy::for_each(_lines, [=](Line<Cell>& line) {
                if (newColumnCount < line.size())
                    line.resize(newColumnCount);
            });
            verifyState();
            return cursor + std::min(cursor.column, boxed_cast<ColumnOffset>(newColumnCount));
        }
        else
        {
            // {{{ Shrinking progress
            // -----------------------------------------------------------------------
            //  (one-by-one)        | (from-5-to-2)
            // -----------------------------------------------------------------------
            // "ABCDE"              | "ABCDE"
            // "abcde"              | "xy   "
            // ->                   | "abcde"
            // "ABCD"               | ->
            // "E   "   Wrapped     | "AB"                  push "AB", wrap "CDE"
            // "abcd"               | "CD"      Wrapped     push "CD", wrap "E"
            // "e   "   Wrapped     | "E"       Wrapped     push "E",  inc line
            // ->                   | "xy"      no-wrapped  push "xy", inc line
            // "ABC"                | "ab"      no-wrapped  push "ab", wrap "cde"
            // "DE "    Wrapped     | "cd"      Wrapped     push "cd", wrap "e"
            // "abc"                | "e "      Wrapped     push "e",  inc line
            // "de "    Wrapped
            // ->
            // "AB"
            // "DE"     Wrapped
            // "E "     Wrapped
            // "ab"
            // "cd"     Wrapped
            // "e "     Wrapped
            // }}}

            Lines<Cell> shrinkedLines;
            LineBuffer wrappedColumns;
            LineFlags previousFlags = _lines.front().inheritableFlags();

            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            shrinkedLines.reserve(totalLineCount);
            Require(totalLineCount == unbox<size_t>(this->totalLineCount()));

            auto numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *_pageSize.lines; ++i)
            {
                auto& line = _lines[i];

                // do we have previous columns carried?
                if (!wrappedColumns.empty())
                {
                    if (line.wrapped() && line.inheritableFlags() == previousFlags)
                    {
                        // Prepend previously wrapped columns into current line.
                        auto& editable = line.inflatedBuffer();
                        editable.insert(editable.begin(), wrappedColumns.begin(), wrappedColumns.end());
                    }
                    else
                    {
                        // Insert NEW line(s) between previous and this line with previously wrapped columns.
                        auto const numLinesInserted = detail::addNewWrappedLines(
                            shrinkedLines, newColumnCount, std::move(wrappedColumns), previousFlags, false);
                        numLinesWritten += numLinesInserted;
                        previousFlags = line.inheritableFlags();
                    }
                }
                else
                {
                    previousFlags = line.inheritableFlags();
                }

                wrappedColumns = line.reflow(newColumnCount);

                shrinkedLines.emplace_back(std::move(line));
                numLinesWritten++;
                Ensures(shrinkedLines.back().size() >= newColumnCount);
            }
            numLinesWritten += detail::addNewWrappedLines(
                shrinkedLines, newColumnCount, std::move(wrappedColumns), previousFlags, false);
            Require(unbox<size_t>(numLinesWritten) == shrinkedLines.size());
            Require(numLinesWritten >= _pageSize.lines);

            while (shrinkedLines.size() < totalLineCount)
                shrinkedLines.emplace_back(
                    LineFlags::None,
                    TrivialLineBuffer { newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

            shrinkedLines.rotate_left(
                unbox<size_t>(numLinesWritten - _pageSize.lines)); // maybe to be done outisde?
            _linesUsed = LineCount::cast_from(numLinesWritten);

            // if (LineCount::cast_from(shrinkedLines.size()) > _pageSize.lines)
            // {
            //     auto const overflow = LineCount::cast_from(shrinkedLines.size()) -
            //     _pageSize.lines;
            //     _linesUsed -= overflow;
            // }

            _lines = std::move(shrinkedLines);
            _pageSize.columns = newColumnCount;

            verifyState();
            return cursor; // TODO
        }
    };
    // }}}

    CellLocation cursor = currentCursorPos;

    // grow/shrink columns
    using crispy::Comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
    {
        case Comparison::Greater: cursor += growColumns(newSize.columns); break;
        case Comparison::Less: cursor = shrinkColumns(newSize.columns, newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    // grow/shrink lines
    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
    {
        case Comparison::Greater: cursor += growLines(newSize.lines, cursor); break;
        case Comparison::Less: cursor += shrinkLines(newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    Ensures(_pageSize == newSize);
    verifyState();

    return cursor;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::clampHistory()
{
    // TODO: needed?
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::appendNewLines(LineCount count, GraphicsAttributes attr)
{
    auto const wrappableFlag = _lines.back().wrappableFlag();

    if (historyLineCount() == maxHistoryLineCount())
    {
        // We've reached to history line count limit already.
        // Rotate lines that would fall off down to the bottom again in a clean state.
        // We do save quite some overhead due to avoiding unnecessary memory allocations.
        for (int i = 0; i < unbox<int>(count); ++i)
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
        generate_n(back_inserter(_lines), *n, [&]() {
            return Line<Cell>(wrappableFlag, TrivialLineBuffer { _pageSize.columns, attr, attr });
        });
        clampHistory();
    }
}
// }}}
// {{{ dumpGrid impl
template <typename Cell>
std::ostream& dumpGrid(std::ostream& os, Grid<Cell> const& grid)
{
    os << fmt::format(
        "main page lines: scrollback cur {} max {}, main page lines {}, used lines {}, zero index {}\n",
        grid.historyLineCount(),
        grid.maxHistoryLineCount(),
        grid.pageSize().lines,
        grid.linesUsed(),
        grid.zero_index());

    for (int const lineOffset:
         ranges::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines)))
    {
        terminal::Line<Cell> const& lineAttribs = grid.lineAt(LineOffset(lineOffset));

        os << fmt::format("[{:>2}] \"{}\" | {}\n",
                          lineOffset,
                          grid.lineText(LineOffset::cast_from(lineOffset)),
                          (unsigned) lineAttribs.flags());
    }

    return os;
}

template <typename Cell>
std::string dumpGrid(Grid<Cell> const& grid)
{
    std::stringstream sstr;
    dumpGrid(sstr, grid);
    return sstr.str();
}
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
CellLocationRange Grid<Cell>::wordRangeUnderCursor(CellLocation position,
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

        // get right word margin
        for (;;)
        {
            if (*current.column == *pageSize().columns - 1 && *current.line + 1 < *pageSize().lines
                && isLineWrapped(current.line))
            {
                current.line++;
                current.column = ColumnOffset(0);
                current = stretchedColumn(CellLocation { current.line, current.column + 1 });
            }

            if (*current.column + 1 < *pageSize().columns)
            {
                current = stretchedColumn(CellLocation { current.line, current.column + 1 });
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

    return { left, right };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Grid<Cell>::cellEmptyOrContainsOneOf(CellLocation position, u32string_view delimiters) const noexcept
{
    // Word selection may be off by one
    position.column = min(position.column, boxed_cast<ColumnOffset>(pageSize().columns - 1));

    auto const& cell = at(position.line, position.column);
    return CellUtil::empty(cell) || delimiters.find(cell.codepoint(0)) != delimiters.npos;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
u32string Grid<Cell>::extractText(CellLocationRange range) const noexcept
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
            ranges.emplace_back(
                ColumnRange { range.first.line, range.first.column, min(range.second.column, rightMargin) });
            break;
        case 2:
            ranges.emplace_back(ColumnRange { range.first.line, range.first.column, rightMargin });
            ranges.emplace_back(
                ColumnRange { range.first.line, ColumnOffset(0), min(range.second.column, rightMargin) });
            break;
        default:
            ranges.emplace_back(ColumnRange { range.first.line, range.first.column, rightMargin });
            for (auto j = range.first.line + 1; j < range.second.line; ++j)
                ranges.emplace_back(ColumnRange { j, ColumnOffset(0), rightMargin });
            ranges.emplace_back(
                ColumnRange { range.second.line, ColumnOffset(0), min(range.second.column, rightMargin) });
            break;
    }

    for (auto const& range: ranges)
    {
        if (!output.empty())
            output += '\n';
        for (auto column = range.fromColumn; column <= range.toColumn; ++column)
        {
            auto const& cell = at(range.line, column);
            if (cell.codepointCount() != 0)
                output += cell.codepoints();
            else
                output += L' ';
        }
    }

    return output;
}

} // end namespace terminal

#include <vtbackend/cell/CompactCell.h>
template class terminal::Grid<terminal::CompactCell>;
template std::string terminal::dumpGrid<terminal::CompactCell>(terminal::Grid<terminal::CompactCell> const&);

#include <vtbackend/cell/SimpleCell.h>
template class terminal::Grid<terminal::SimpleCell>;
template std::string terminal::dumpGrid<terminal::SimpleCell>(terminal::Grid<terminal::SimpleCell> const&);
