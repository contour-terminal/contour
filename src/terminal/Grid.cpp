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
#include <terminal/Grid.h>
#include <terminal/primitives.h>

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
    Lines<Cell> createLines(PageSize _pageSize,
                            LineCount _maxHistoryLineCount,
                            bool _reflowOnResize,
                            GraphicsAttributes _initialSGR)
    {
        auto const defaultLineFlags = _reflowOnResize ? LineFlags::Wrappable : LineFlags::None;
        auto const totalLineCount = unbox<size_t>(_pageSize.lines + _maxHistoryLineCount);

        Lines<Cell> lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: ranges::views::iota(0u, totalLineCount))
            lines.emplace_back(defaultLineFlags, TrivialLineBuffer { _pageSize.columns, _initialSGR });

        return lines;
    }

    /**
     * Appends logical line by splitting into fixed-width lines.
     *
     * @param _targetLines
     * @param _newColumnCount
     * @param _logicalLineBuffer
     * @param _baseFlags
     * @param _initialNoWrap
     *
     * @returns number of inserted lines
     */
    template <typename Cell>
    LineCount addNewWrappedLines(
        Lines<Cell>& _targetLines,
        ColumnCount _newColumnCount,
        typename Line<Cell>::InflatedBuffer&& _logicalLineBuffer, // TODO: don't move, do (c)ref instead
        LineFlags _baseFlags,
        bool _initialNoWrap // TODO: pass `LineFlags _defaultLineFlags` instead?
    )
    {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        // TODO: avoid unnecessary copies via erase() by incrementally updating (from, to)
        int i = 0;

        while (_logicalLineBuffer.size() >= unbox<size_t>(_newColumnCount))
        {
            auto from = _logicalLineBuffer.begin();
            auto to = from + _newColumnCount.as<std::ptrdiff_t>();
            auto const wrappedFlag = i == 0 && _initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            _targetLines.emplace_back(_baseFlags | wrappedFlag, LineBuffer(from, to));
            _logicalLineBuffer.erase(from, to);
            ++i;
        }

        if (_logicalLineBuffer.size() > 0)
        {
            auto const wrappedFlag = i == 0 && _initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            ++i;
            _logicalLineBuffer.resize(unbox<size_t>(_newColumnCount));
            _targetLines.emplace_back(_baseFlags | wrappedFlag, std::move(_logicalLineBuffer));
        }
        return LineCount::cast_from(i);
    }

} // namespace detail
// {{{ Grid impl
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Grid<Cell>::Grid(PageSize _pageSize, bool _reflowOnResize, MaxHistoryLineCount _maxHistoryLineCount):
    pageSize_ { _pageSize },
    reflowOnResize_ { _reflowOnResize },
    historyLimit_ { _maxHistoryLineCount },
    lines_ { detail::createLines<Cell>(
        _pageSize,
        [_maxHistoryLineCount]() -> LineCount {
            if (auto const* maxLineCount = std::get_if<LineCount>(&_maxHistoryLineCount))
                return *maxLineCount;
            else
                return LineCount::cast_from(0);
        }(),
        _reflowOnResize,
        GraphicsAttributes {}) },
    linesUsed_ { _pageSize.lines }
{
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::setMaxHistoryLineCount(MaxHistoryLineCount _maxHistoryLineCount)
{
    verifyState();
    rezeroBuffers();
    historyLimit_ = _maxHistoryLineCount;
    lines_.resize(unbox<size_t>(pageSize_.lines + maxHistoryLineCount()));
    linesUsed_ = min(linesUsed_, pageSize_.lines + maxHistoryLineCount());
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::clearHistory()
{
    linesUsed_ = pageSize_.lines;
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::verifyState() const
{
#if !defined(NDEBUG)
    // maxHistoryLineCount_ + pageSize_.lines
    Require(LineCount::cast_from(lines_.size()) >= totalLineCount());
    Require(LineCount::cast_from(lines_.size()) >= linesUsed_);
    Require(linesUsed_ >= pageSize_.lines);
#endif
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::renderAllText() const
{
    std::string text;
    text.reserve(unbox<size_t>(historyLineCount() + pageSize_.lines) * unbox<size_t>(pageSize_.columns + 1));

    for (auto y = LineOffset(0); y < LineOffset::cast_from(lines_.size()); ++y)
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

    for (auto line = LineOffset(0); line < unbox<LineOffset>(pageSize_.lines); ++line)
    {
        text += lineText(line);
        text += '\n';
    }

    return text;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Line<Cell>& Grid<Cell>::lineAt(LineOffset _line) noexcept
{
    // Require(*_line < *pageSize_.lines);
    return lines_[unbox<long>(_line)];
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Line<Cell> const& Grid<Cell>::lineAt(LineOffset _line) const noexcept
{
    // Require(*_line < *pageSize_.lines);
    return const_cast<Grid&>(*this).lineAt(_line);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& Grid<Cell>::at(LineOffset _line, ColumnOffset _column) noexcept
{
    return useCellAt(_line, _column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& Grid<Cell>::useCellAt(LineOffset _line, ColumnOffset _column) noexcept
{
    return lineAt(_line).useCellAt(_column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell const& Grid<Cell>::at(LineOffset _line, ColumnOffset _column) const noexcept
{
    return const_cast<Grid&>(*this).at(_line, _column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell>> Grid<Cell>::pageAtScrollOffset(ScrollOffset _scrollOffset)
{
    Require(unbox<LineCount>(_scrollOffset) <= historyLineCount());

    int const offset = -*_scrollOffset;
    Line<Cell>* startLine = &lines_[offset];
    auto const count = unbox<size_t>(pageSize_.lines);

    return gsl::span<Line<Cell>> { startLine, count };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Line<Cell> const> Grid<Cell>::pageAtScrollOffset(ScrollOffset _scrollOffset) const
{
    Require(unbox<LineCount>(_scrollOffset) <= historyLineCount());

    int const offset = -*_scrollOffset;
    Line<Cell> const* startLine = &lines_[offset];
    auto const count = unbox<size_t>(pageSize_.lines);

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
gsl::span<Cell const> Grid<Cell>::lineBufferRightTrimmed(LineOffset _line) const noexcept
{
    return detail::trimRight(lineBuffer(_line));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineText(LineOffset _line) const
{
    std::string line;
    line.reserve(unbox<size_t>(pageSize_.columns));

    for (Cell const& cell: lineBuffer(_line))
        if (cell.codepointCount())
            line += cell.toUtf8();
        else
            line += ' '; // fill character

    return line;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineTextTrimmed(LineOffset _line) const
{
    std::string output = lineText(_line);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Grid<Cell>::lineText(Line<Cell> const& _line) const
{
    std::stringstream sstr;
    for (Cell const& cell: _line.inflatedBuffer())
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
void Grid<Cell>::setLineText(LineOffset _line, std::string_view _text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(_text))
        useCellAt(_line, ColumnOffset::cast_from(i++)).setCharacter(ch);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Grid<Cell>::isLineBlank(LineOffset _line) const noexcept
{
    auto const is_blank = [](auto const& _cell) noexcept {
        return CellUtil::empty(_cell);
    };

    auto const line = lineBuffer(_line);
    return std::all_of(line.begin(), line.end(), is_blank);
}

/**
 * Computes the relative line number for the bottom-most @p _n logical lines.
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
int Grid<Cell>::computeLogicalLineNumberFromBottom(LineCount _n) const noexcept
{
    int logicalLineCount = 0;
    auto outputRelativePhysicalLine = *pageSize_.lines - 1;

    auto i = lines_.rbegin();
    while (i != lines_.rend())
    {
        if (!i->wrapped())
            logicalLineCount++;
        outputRelativePhysicalLine--;
        ++i;
        if (logicalLineCount == *_n)
            break;
    }

    // XXX If the top-most logical line is reached, we still need to traverse upwards until the
    // beginning of the top-most logical line (the one that does not have the wrapped-flag set).
    while (i != lines_.rend() && i->wrapped())
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
LineCount Grid<Cell>::scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes _defaultAttributes) noexcept
{
    verifyState();
    if (unbox<size_t>(linesUsed_) == lines_.size()) // with all grid lines in-use
    {
        if (std::get_if<Infinite>(&historyLimit_))
        {
            for ([[maybe_unused]] auto const _: ranges::views::iota(0, unbox<int>(linesCountToScrollUp)))
                lines_.emplace_back(defaultLineFlags(),
                                    TrivialLineBuffer { pageSize_.columns, GraphicsAttributes {} });
            return scrollUp(linesCountToScrollUp, _defaultAttributes);
        }
        // TODO: ensure explicit test for this case
        rotateBuffersLeft(linesCountToScrollUp);

        // Initialize (/reset) new lines.
        for (auto y = boxed_cast<LineOffset>(pageSize_.lines - linesCountToScrollUp);
             y < boxed_cast<LineOffset>(pageSize_.lines);
             ++y)
            lineAt(y).reset(defaultLineFlags(), _defaultAttributes);

        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(linesUsed_) < lines_.size());

        // Number of lines in the ring buffer that are not yet
        // used by the grid system.
        auto const linesAvailable = LineCount::cast_from(lines_.size() - unbox<size_t>(linesUsed_));

        // Number of lines in the ring buffer that we can allocate at the head.
        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            linesUsed_ += linesAppendCount;
            Require(unbox<size_t>(linesUsed_) <= lines_.size());
            fill_n(next(lines_.begin(), *pageSize_.lines),
                   unbox<size_t>(linesAppendCount),
                   Line<Cell> { defaultLineFlags(),
                                TrivialLineBuffer { pageSize_.columns, _defaultAttributes } });
            rotateBuffersLeft(linesAppendCount);
        }
        if (linesAppendCount < linesCountToScrollUp)
        {
            auto const incrementCount = linesCountToScrollUp - linesAppendCount;
            rotateBuffersLeft(incrementCount);

            // Initialize (/reset) new lines.
            for (auto y = boxed_cast<LineOffset>(pageSize_.lines - linesCountToScrollUp);
                 y < boxed_cast<LineOffset>(pageSize_.lines);
                 ++y)
                lineAt(y).reset(defaultLineFlags(), _defaultAttributes);
        }
        return LineCount::cast_from(linesAppendCount);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
LineCount Grid<Cell>::scrollUp(LineCount _n, GraphicsAttributes _defaultAttributes, Margin _margin) noexcept
{
    verifyState();
    Require(0 <= *_margin.horizontal.from && *_margin.horizontal.to < *pageSize_.columns);
    Require(0 <= *_margin.vertical.from && *_margin.vertical.to < *pageSize_.lines);

    // these two booleans could be cached and updated whenever _margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        _margin.horizontal
        == Margin::Horizontal { ColumnOffset { 0 }, unbox<ColumnOffset>(pageSize_.columns) - 1 };
    auto const fullVertical =
        _margin.vertical == Margin::Vertical { LineOffset(0), unbox<LineOffset>(pageSize_.lines) - 1 };

    if (fullHorizontal)
    {
        if (fullVertical) // full-screen scroll-up
            return scrollUp(_n, _defaultAttributes);

        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = LineCount(_margin.vertical.length());
        auto const n = std::min(_n, marginHeight);
        if (*n && n < marginHeight)
        {
            // rotate line attribs
            for (auto topLineOffset = *_margin.vertical.from; topLineOffset <= *_margin.vertical.to - *n;
                 ++topLineOffset)
                lines_[topLineOffset] = std::move(lines_[topLineOffset + *n]);
        }

        auto const topEmptyLineNr = *_margin.vertical.to - *n + 1;
        auto const bottomLineNumber = *_margin.vertical.to;
        for (auto lineNumber = topEmptyLineNr; lineNumber <= bottomLineNumber; ++lineNumber)
        {
            lines_[lineNumber].reset(defaultLineFlags(), _defaultAttributes, pageSize_.columns);
        }
    }
    else
    {
        // a full "inside" scroll-up
        auto const marginHeight = _margin.vertical.length();
        auto const n = std::min(_n, marginHeight);
        // auto const bottomLineOffsetToCopy = min(_margin.vertical.from + *n, _margin.vertical.to - 1);
        auto const topTargetLineOffset = _margin.vertical.from;
        auto const bottomTargetLineOffset = _margin.vertical.to - *n;
        auto const columnsToMove = unbox<size_t>(_margin.horizontal.length());

        for (LineOffset targetLineOffset = topTargetLineOffset; targetLineOffset <= bottomTargetLineOffset;
             ++targetLineOffset)
        {
            auto const sourceLineOffset = targetLineOffset + *n;
            auto t = &useCellAt(targetLineOffset, _margin.horizontal.from);
            auto s = &at(sourceLineOffset, _margin.horizontal.from);
            std::copy_n(s, columnsToMove, t);
        }

        for (LineOffset line = _margin.vertical.to - *n + 1; line <= _margin.vertical.to; ++line)
        {
            auto a = &useCellAt(line, _margin.horizontal.from);
            auto b = a + unbox<int>(_margin.horizontal.length());
            while (a != b)
            {
                a->reset(_defaultAttributes);
                a++;
            }
        }
    }
    verifyState();
    return LineCount(0); // No full-margin lines scrolled up.
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::scrollDown(LineCount v_n,
                            GraphicsAttributes const& _defaultAttributes,
                            Margin const& _margin)
{
    verifyState();
    Require(v_n >= LineCount(0));

    // these two booleans could be cached and updated whenever _margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        _margin.horizontal
        == Margin::Horizontal { ColumnOffset { 0 },
                                unbox<ColumnOffset>(pageSize_.columns) - ColumnOffset(1) };
    auto const fullVertical =
        _margin.vertical
        == Margin::Vertical { LineOffset(0), unbox<LineOffset>(pageSize_.lines) - LineOffset(1) };

    auto const n = std::min(v_n, _margin.vertical.length());

    if (fullHorizontal && fullVertical)
    {
        // full screen scrolling

        // on the full screen (all lines)
        // move all lines up by N lines
        // bottom N lines are wiped out

        rotateBuffersRight(n);

        for (Line<Cell>& line: mainPage().subspan(0, unbox<size_t>(n)))
            line.reset(defaultLineFlags(), _defaultAttributes);
        return;
    }

    if (fullHorizontal) // => but ont fully vertical
    {
        // scroll down only inside vertical margin with full horizontal extend
        auto a = std::next(begin(lines_), *_margin.vertical.from);
        auto b = std::next(begin(lines_), *_margin.vertical.to + 1 - *n);
        auto c = std::next(begin(lines_), *_margin.vertical.to + 1);
        std::rotate(a, b, c);
        for (auto const i: ranges::views::iota(*_margin.vertical.from, *_margin.vertical.from + *n))
            lines_[i].reset(defaultLineFlags(), _defaultAttributes);
    }
    else
    {
        // a full "inside" scroll-down
        if (n <= _margin.vertical.length())
        {
            for (LineOffset line = _margin.vertical.to; line >= _margin.vertical.to - *n; --line)
            {
                auto s = &at(line - *n, _margin.horizontal.from);
                auto t = &at(line, _margin.horizontal.from);
                std::copy_n(s, unbox<size_t>(_margin.horizontal.length()), t);
            }

            for (LineOffset line = _margin.vertical.from; line < _margin.vertical.from + *n; ++line)
            {
                auto a = &at(line, _margin.horizontal.from);
                auto b = &at(line, _margin.horizontal.to + 1);
                while (a != b)
                {
                    *a = Cell { _defaultAttributes };
                    a++;
                }
            }
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::scrollLeft(GraphicsAttributes _defaultAttributes, Margin _margin) noexcept
{
    for (LineOffset lineNo = _margin.vertical.from; lineNo <= _margin.vertical.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto column0 = line.inflatedBuffer().begin() + *_margin.horizontal.from;
        auto column1 = line.inflatedBuffer().begin() + *_margin.horizontal.from + 1;
        auto column2 = line.inflatedBuffer().begin() + *_margin.horizontal.to + 1;
        std::rotate(column0, column1, column2);

        auto const emptyCell = Cell { _defaultAttributes };
        auto const emptyCellsBegin = line.inflatedBuffer().begin() + *_margin.horizontal.to;
        std::fill_n(emptyCellsBegin, 1, emptyCell);
    }
}

// }}}
// {{{ Grid impl: resize
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Grid<Cell>::reset()
{
    linesUsed_ = pageSize_.lines;
    lines_.rotate_right(lines_.zero_index());
    for (int i = 0; i < unbox<int>(pageSize_.lines); ++i)
        lines_[i].reset(defaultLineFlags(), GraphicsAttributes {});
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
CellLocation Grid<Cell>::growLines(LineCount _newHeight, CellLocation _cursor)
{
    // Grow line count by splicing available lines from history back into buffer, if available,
    // or create new ones until pageSize_.lines == _newHeight.

    Require(_newHeight > pageSize_.lines);
    // lines_.reserve(unbox<size_t>(maxHistoryLineCount_ + _newHeight));

    // Pull down from history if cursor is at bottom and if scrollback available.
    CellLocation cursorMove {};
    if (*_cursor.line + 1 == *pageSize_.lines)
    {
        auto const totalLinesToExtend = _newHeight - pageSize_.lines;
        auto const linesToTakeFromSavedLines = std::min(totalLinesToExtend, historyLineCount());
        Require(totalLinesToExtend >= linesToTakeFromSavedLines);
        Require(*linesToTakeFromSavedLines >= 0);
        rotateBuffersRight(linesToTakeFromSavedLines);
        pageSize_.lines += linesToTakeFromSavedLines;
        cursorMove.line += boxed_cast<LineOffset>(linesToTakeFromSavedLines);
    }

    auto const wrappableFlag = lines_.back().wrappableFlag();
    auto const totalLinesToExtend = _newHeight - pageSize_.lines;
    Require(*totalLinesToExtend >= 0);
    // ? Require(linesToTakeFromSavedLines == LineCount(0));

    auto const newTotalLineCount = maxHistoryLineCount() + _newHeight;
    auto const currentTotalLineCount = LineCount::cast_from(lines_.size());
    auto const linesToFill = max(0, *newTotalLineCount - *currentTotalLineCount);

    for ([[maybe_unused]] auto const _: ranges::views::iota(0, linesToFill))
        lines_.emplace_back(wrappableFlag, TrivialLineBuffer { pageSize_.columns, GraphicsAttributes {} });

    pageSize_.lines += totalLinesToExtend;
    linesUsed_ = min(linesUsed_ + totalLinesToExtend, LineCount::cast_from(lines_.size()));

    Ensures(pageSize_.lines == _newHeight);
    Ensures(lines_.size() >= unbox<size_t>(maxHistoryLineCount() + pageSize_.lines));
    verifyState();

    return cursorMove;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
CellLocation Grid<Cell>::resize(PageSize _newSize, CellLocation _currentCursorPos, bool _wrapPending)
{
    if (pageSize_ == _newSize)
        return _currentCursorPos;

    GridLog()("resize {} -> {} (cursor {})", pageSize_, _newSize, _currentCursorPos);

    // Growing in line count with scrollback lines present will move
    // the scrollback lines into the visible area.
    //
    // Shrinking in line count with the cursor at the bottom margin will move
    // the top lines into the scrollback area.

    // {{{ helper methods
    auto const shrinkLines = [this](LineCount _newHeight, CellLocation _cursor) -> CellLocation {
        // Shrink existing line count to _newSize.lines
        // by splicing the number of lines to be shrinked by into savedLines bottom.

        Require(_newHeight < pageSize_.lines);

        // FIXME: in alt screen, when shrinking more then available below screen cursor -> assertion failure

        auto const numLinesToShrink = pageSize_.lines - _newHeight;
        auto const linesAvailableBelowCursorBeforeShrink =
            pageSize_.lines - boxed_cast<LineCount>(_cursor.line + 1);
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
            pageSize_.lines -= cutoffCount;
            linesUsed_ -= cutoffCount;
            Ensures(*_cursor.line < *pageSize_.lines);
            verifyState();
        }

        // 2.) If _newHeight is still below page line count, then shrink by rotating up.
        Require(_newHeight <= pageSize_.lines);
        if (*numLinesToPushUp)
        {
            GridLog()(" -> numLinesToPushUp {}", numLinesToPushUp);
            Require(*_cursor.line + 1 == *pageSize_.lines);
            rotateBuffersLeft(numLinesToPushUp);
            pageSize_.lines -= numLinesToPushUp;
            clampHistory();
            verifyState();
            return CellLocation { -boxed_cast<LineOffset>(numLinesToPushUp), {} };
        }

        verifyState();
        return CellLocation {};
    };

    auto const growColumns = [this, _wrapPending](ColumnCount _newColumnCount) -> CellLocation {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        if (!reflowOnResize_)
        {
            for (auto& line: lines_)
                if (line.size() < _newColumnCount)
                    line.resize(_newColumnCount);
            pageSize_.columns = _newColumnCount;
            verifyState();
            return CellLocation { LineOffset(0), ColumnOffset(_wrapPending ? 1 : 0) };
        }
        else
        {
            // Grow columns by inverse shrink,
            // i.e. the lines are traversed in reverse order.

            auto const extendCount = _newColumnCount - pageSize_.columns;
            Require(*extendCount > 0);

            Lines<Cell> grownLines;
            LineBuffer
                logicalLineBuffer; // Temporary state, representing wrapped columns from the line "below".
            LineFlags logicalLineFlags = LineFlags::None;

            auto const appendToLogicalLine = [&logicalLineBuffer](gsl::span<Cell const> cells) {
                for (auto const& cell: cells)
                    logicalLineBuffer.push_back(cell);
            };

            auto const flushLogicalLine = [_newColumnCount,
                                           &grownLines,
                                           &logicalLineBuffer,
                                           &logicalLineFlags]() {
                if (!logicalLineBuffer.empty())
                {
                    detail::addNewWrappedLines(
                        grownLines, _newColumnCount, std::move(logicalLineBuffer), logicalLineFlags, true);
                    logicalLineBuffer.clear();
                }
            };

            [[maybe_unused]] auto const logLogicalLine =
                [&logicalLineBuffer]([[maybe_unused]] LineFlags lineFlags,
                                     [[maybe_unused]] std::string_view msg) {
                    GridLog()("{} |> \"{}\"", msg, Line<Cell>(lineFlags, logicalLineBuffer).toUtf8());
                };

            for (int i = -*historyLineCount(); i < *pageSize_.lines; ++i)
            {
                auto& line = lines_[i];
                // logLogicalLine(line.flags(), fmt::format("Line[{:>2}]: next line: \"{}\"", i,
                // line.toUtf8()));
                Require(line.size() >= pageSize_.columns);

                if (line.wrapped())
                {
                    // logLogicalLine(line.flags(), fmt::format(" - appending: \"{}\"",
                    // line.toUtf8Trimmed()));
                    appendToLogicalLine(line.trim_blank_right());
                }
                else // line is not wrapped
                {
                    flushLogicalLine();
                    // logLogicalLine(line.flags(), " - start new logical line");
                    appendToLogicalLine(line.cells());
                    logicalLineFlags = line.flags() & ~LineFlags::Wrapped;
                }
            }

            flushLogicalLine(); // Flush last (bottom) line, if anything pending.

            // auto diff = int(lines_.size()) - unbox<int>(pageSize_.lines);
            auto cy = LineCount(0);
            if (pageSize_.lines > LineCount::cast_from(grownLines.size()))
            {
                // The lines we've been reflowing do not fill the pageSize,
                // so fill the gap until we have a full page.
                cy = pageSize_.lines - LineCount::cast_from(grownLines.size());
                while (LineCount::cast_from(grownLines.size()) < pageSize_.lines)
                    grownLines.emplace_back(
                        defaultLineFlags(),
                        TrivialLineBuffer { _newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

                Ensures(LineCount::cast_from(grownLines.size()) == pageSize_.lines);
            }

            linesUsed_ = LineCount::cast_from(grownLines.size());

            // Fill scrollback lines.
            auto const totalLineCount = unbox<size_t>(pageSize_.lines + maxHistoryLineCount());
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(
                    defaultLineFlags(),
                    TrivialLineBuffer { _newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

            lines_ = std::move(grownLines);
            pageSize_.columns = _newColumnCount;

            auto const newHistoryLineCount = linesUsed_ - pageSize_.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return CellLocation { -boxed_cast<LineOffset>(cy), ColumnOffset(_wrapPending ? 1 : 0) };
        }
    };

    auto const shrinkColumns = [this](ColumnCount _newColumnCount,
                                      LineCount /*_newLineCount*/,
                                      CellLocation _cursor) -> CellLocation {
        using LineBuffer = typename Line<Cell>::InflatedBuffer;

        if (!reflowOnResize_)
        {
            pageSize_.columns = _newColumnCount;
            crispy::for_each(lines_, [=](Line<Cell>& line) {
                if (_newColumnCount < line.size())
                    line.resize(_newColumnCount);
            });
            verifyState();
            return _cursor + std::min(_cursor.column, boxed_cast<ColumnOffset>(_newColumnCount));
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
            LineFlags previousFlags = lines_.front().inheritableFlags();

            auto const totalLineCount = unbox<size_t>(pageSize_.lines + maxHistoryLineCount());
            shrinkedLines.reserve(totalLineCount);
            Require(totalLineCount == unbox<size_t>(this->totalLineCount()));

            auto numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *pageSize_.lines; ++i)
            {
                auto& line = lines_[i];

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
                            shrinkedLines, _newColumnCount, std::move(wrappedColumns), previousFlags, false);
                        numLinesWritten += numLinesInserted;
                        previousFlags = line.inheritableFlags();
                    }
                }
                else
                {
                    previousFlags = line.inheritableFlags();
                }

                wrappedColumns = line.reflow(_newColumnCount);

                shrinkedLines.emplace_back(std::move(line));
                numLinesWritten++;
                Ensures(shrinkedLines.back().size() >= _newColumnCount);
            }
            numLinesWritten += detail::addNewWrappedLines(
                shrinkedLines, _newColumnCount, std::move(wrappedColumns), previousFlags, false);
            Require(unbox<size_t>(numLinesWritten) == shrinkedLines.size());
            Require(numLinesWritten >= pageSize_.lines);

            while (shrinkedLines.size() < totalLineCount)
                shrinkedLines.emplace_back(
                    LineFlags::None,
                    TrivialLineBuffer { _newColumnCount, GraphicsAttributes {}, GraphicsAttributes {} });

            shrinkedLines.rotate_left(
                unbox<size_t>(numLinesWritten - pageSize_.lines)); // maybe to be done outisde?
            linesUsed_ = LineCount::cast_from(numLinesWritten);

            // if (LineCount::cast_from(shrinkedLines.size()) > pageSize_.lines)
            // {
            //     auto const overflow = LineCount::cast_from(shrinkedLines.size()) -
            //     pageSize_.lines;
            //     linesUsed_ -= overflow;
            // }

            lines_ = std::move(shrinkedLines);
            pageSize_.columns = _newColumnCount;

            verifyState();
            return _cursor; // TODO
        }
    };
    // }}}

    CellLocation cursor = _currentCursorPos;

    // grow/shrink columns
    using crispy::Comparison;
    switch (crispy::strongCompare(_newSize.columns, pageSize_.columns))
    {
        case Comparison::Greater: cursor += growColumns(_newSize.columns); break;
        case Comparison::Less: cursor = shrinkColumns(_newSize.columns, _newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    // grow/shrink lines
    switch (crispy::strongCompare(_newSize.lines, pageSize_.lines))
    {
        case Comparison::Greater: cursor += growLines(_newSize.lines, cursor); break;
        case Comparison::Less: cursor += shrinkLines(_newSize.lines, cursor); break;
        case Comparison::Equal: break;
    }

    Ensures(pageSize_ == _newSize);
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
void Grid<Cell>::appendNewLines(LineCount _count, GraphicsAttributes _attr)
{
    auto const wrappableFlag = lines_.back().wrappableFlag();

    if (historyLineCount() == maxHistoryLineCount())
    {
        // We've reached to history line count limit already.
        // Rotate lines that would fall off down to the bottom again in a clean state.
        // We do save quite some overhead due to avoiding unnecessary memory allocations.
        for (int i = 0; i < unbox<int>(_count); ++i)
        {
            auto line = std::move(lines_.front());
            lines_.pop_front();
            line.reset(defaultLineFlags(), _attr);
            lines_.emplace_back(std::move(line));
        }
        return;
    }

    if (auto const n = std::min(_count, pageSize_.lines); *n > 0)
    {
        generate_n(back_inserter(lines_), *n, [&]() {
            return Line<Cell>(wrappableFlag, TrivialLineBuffer { pageSize_.columns, _attr, _attr });
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

#include <terminal/cell/CompactCell.h>
template class terminal::Grid<terminal::CompactCell>;
template std::string terminal::dumpGrid<terminal::CompactCell>(terminal::Grid<terminal::CompactCell> const&);

#include <terminal/cell/SimpleCell.h>
template class terminal::Grid<terminal::SimpleCell>;
template std::string terminal::dumpGrid<terminal::SimpleCell>(terminal::Grid<terminal::SimpleCell> const&);
