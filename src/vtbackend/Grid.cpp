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
#include <variant>

using ranges::views::iota;
using std::max;
using std::min;
using std::u32string;
using std::u32string_view;
using std::vector;

namespace terminal
{

auto const inline GridLog = logstore::Category(
    "vt.grid", "Grid related", logstore::Category::State::Disabled, logstore::Category::Visibility::Hidden);

// {{{ detail
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
    Lines<Cell> createLines(crispy::BufferObject<Cell>& cellStorage,
                            PageSize pageSize,
                            MaxHistoryLineCount maxHistoryLineCount,
                            bool reflowOnResize,
                            GraphicsAttributes initialSGR)
    {
        auto const historyLineCount = std::holds_alternative<LineCount>(maxHistoryLineCount)
            ? std::get<LineCount>(maxHistoryLineCount)
            : LineCount(0); // TODO(pr) <-- Fill reasonable initial value here.

        auto const totalLineCount = unbox<size_t>(pageSize.lines + historyLineCount);

        auto const defaultLineFlags = reflowOnResize ? LineFlags::Wrappable : LineFlags::None;

        Lines<Cell> lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: ranges::views::iota(0u, totalLineCount))
        {
            auto span = cellStorage.advance(unbox<size_t>(pageSize.columns));
            lines.emplace_back(pageSize.columns, defaultLineFlags, initialSGR, span);
        }

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
// }}}

// {{{ Grid impl
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Cell> Grid<Cell>::allocateCellSpace(ColumnCount count)
{
    // TODO(pr) properly handle inifite scrollback (probably, using BufferObjectPool).
    Require(cellStorage_->bytesAvailable() >= unbox<size_t>(count));
    return cellStorage_->advance(unbox<size_t>(count));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
size_t Grid<Cell>::calculateCellStorageElementCount() const noexcept
{
    auto const historyLineCount = std::holds_alternative<LineCount>(historyLimit_)
        ? std::get<LineCount>(historyLimit_)
        : LineCount(0); // TODO(pr) <-- Fill reasonable initial value here.

    return unbox<size_t>(pageSize_.lines + historyLineCount)
        * unbox<size_t>(pageSize_.columns)
        ;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Grid<Cell>::Grid(PageSize _pageSize, bool _reflowOnResize, MaxHistoryLineCount _maxHistoryLineCount):
    pageSize_ { _pageSize },
    margin_ { Margin::Vertical { {}, pageSize_.lines.as<LineOffset>() - LineOffset(1) },
              Margin::Horizontal { {}, pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1) } },
    reflowOnResize_ { _reflowOnResize },
    historyLimit_ { _maxHistoryLineCount },
    cellStoragePool_(calculateCellStorageElementCount()),
    cellStorage_(cellStoragePool_.allocateBufferObject()),
    lines_ { detail::createLines<Cell>(
        *cellStorage_,
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
                lines_.emplace_back(pageSize_.columns,
                                    defaultLineFlags(),
                                    GraphicsAttributes {},
                                    allocateCellSpace(pageSize_.columns));;
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
            for (int i = 0; i < unbox<int>(linesAppendCount); ++i)
                lines_[unbox<int>(pageSize_.lines) + i].reset(defaultLineFlags(), _defaultAttributes);
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
            lines_[lineNumber].reset(defaultLineFlags(), _defaultAttributes);
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
CellLocation Grid<Cell>::resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending)
{
    crispy::ignore_unused(newSize, currentCursorPos, wrapPending);
    crispy::todo();

    auto const newHistoryLineCount = std::holds_alternative<LineCount>(historyLimit_)
        ? std::get<LineCount>(historyLimit_)
        : historyLineCount();

    auto const totalLineCount = unbox<size_t>(pageSize_.lines + newHistoryLineCount);
    Lines<Cell> newLines;
    newLines.reserve(totalLineCount);

    // For scrollback lines, we make use of the fact, that they will never change, for as long as they keep
    // being in scrollback.
    //
    // Only main page lines need to be rewritten and refilled.

    // Case 1: we shrink the columns
    // -> trailing columns will yield new lines with wrapped flag set.
    //
    // Case 2: we grow the column count

    for (int const lineOffset: iota(-unbox<int>(historyLineCount()), unbox<int>(pageSize().lines)))
    {
        Line<Cell> const& oldLine = lineAt(LineOffset::cast_from(lineOffset));
        (void) oldLine;
        newLines.emplace_back(/* TODO(pr) ... */);
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
