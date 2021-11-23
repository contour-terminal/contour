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
#include <terminal/Cell.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <algorithm>
#include <iostream>

#include <fmt/format.h>

using std::max;
using std::min;
using std::vector;

namespace terminal
{

auto inline GridLog = logstore::Category("vt.grid", "Grid related",
                                         logstore::Category::State::Disabled,
                                         logstore::Category::Visibility::Hidden);

namespace detail { // {{{
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
        while (n && cells[n - 1].empty())
            --n;
        return cells.subspan(0, n);
    }

    template <typename Cell>
    Lines<Cell> createLines(PageSize _pageSize,
                            LineCount _maxHistoryLineCount,
                            bool _reflowOnResize,
                            GraphicsAttributes _initialSGR)
    {
        auto const defaultLineFlags = _reflowOnResize
                                    ? LineFlags::Wrappable
                                    : LineFlags::None;
        auto const totalLineCount = unbox<size_t>(_pageSize.lines + _maxHistoryLineCount);
        size_t const pitch = unbox<size_t>(_pageSize.columns);

        Lines<Cell> lines;
        lines.reserve(totalLineCount);

        for (auto const _: ranges::views::iota(0u, totalLineCount))
            lines.emplace_back(_pageSize.columns, defaultLineFlags, Cell{});

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
        typename Line<Cell>::Buffer&& _logicalLineBuffer, // TODO: don't move, do (c)ref instead
        LineFlags _baseFlags,
        bool _initialNoWrap // TODO: pass `LineFlags _defaultLineFlags` instead?
    )
    {
        using LineBuffer = typename Line<Cell>::Buffer;
        auto const wrappedFlag = _initialNoWrap ? LineFlags::None : LineFlags::Wrapped;

        // TODO: avoid unnecessary copies via erase() by incrementally updating (from, to)
        int i = 0;

        while (_logicalLineBuffer.size() >= *_newColumnCount)
        {
            auto from = _logicalLineBuffer.begin();
            auto to = from + _newColumnCount.as<std::ptrdiff_t>();
            auto const wrappedFlag = i == 0 && _initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            _targetLines.emplace_back(LineBuffer(from, to), _baseFlags | wrappedFlag);
            _logicalLineBuffer.erase(from, to);
            ++i;
        }

        if (_logicalLineBuffer.size() > 0)
        {
            auto const wrappedFlag = i == 0 && _initialNoWrap ? LineFlags::None : LineFlags::Wrapped;
            ++i;
            _targetLines.emplace_back(_newColumnCount, move(_logicalLineBuffer), _baseFlags | wrappedFlag);
        }
        return LineCount::cast_from(i);
    }

} // }}}
// {{{ Grid impl
template <typename Cell>
Grid<Cell>::Grid(PageSize _pageSize, bool _reflowOnResize, LineCount _maxHistoryLineCount) :
    pageSize_{ _pageSize },
    reflowOnResize_{ _reflowOnResize },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    lines_{detail::createLines<Cell>(_pageSize, _maxHistoryLineCount, _reflowOnResize, GraphicsAttributes{})},
    linesUsed_{_pageSize.lines}
{
    verifyState();
}

template <typename Cell>
void Grid<Cell>::setMaxHistoryLineCount(LineCount _maxHistoryLineCount)
{
#if 0
    verifyState();
    if (_maxHistoryLineCount < maxHistoryLineCount_)
    {
        maxHistoryLineCount_ = _maxHistoryLineCount;
        verifyState();
        return;
    }

    auto t = detail::createLines<Cell>(pageSize_, _maxHistoryLineCount, reflowOnResize_, GraphicsAttributes{});
    t.rotate_left(lines_.zero_index());
    for (int i = -static_cast<int>(lines_.zero_index()); i < *pageSize_.lines; ++i)
        t[i] = std::move(lines_[i]);

    // recreate lines
    // - copy existing ones
    // - add empty to fill

    verifyState();
#else
    verifyState();
    rezeroBuffers();
    maxHistoryLineCount_ = _maxHistoryLineCount;
    lines_.reserve(unbox<size_t>(_maxHistoryLineCount));
    verifyState();
#endif
}

template <typename Cell>
void Grid<Cell>::clearHistory()
{
    linesUsed_ = pageSize_.lines;
    verifyState();
}

template <typename Cell>
void Grid<Cell>::verifyState()
{
    Expects(LineCount::cast_from(lines_.size()) >= totalLineCount());
    Expects(totalLineCount() >= linesUsed_);
    Expects(LineCount::cast_from(lines_.size()) >= linesUsed_);
    Expects(linesUsed_ >= pageSize_.lines);
}

template <typename Cell>
std::string Grid<Cell>::renderAllText() const
{
    std::string text;
    text.reserve(unbox<size_t>(historyLineCount() + pageSize_.lines)
               * unbox<size_t>(pageSize_.columns + 1));

    for (auto y = LineOffset(0); y < LineOffset::cast_from(lines_.size()); ++y)
    {
        text += lineText(y);
        text += '\n';
    }

    return text;
}

template <typename Cell>
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
Line<Cell>& Grid<Cell>::lineAt(LineOffset _line) noexcept
{
    Expects(*_line < *pageSize_.lines);
    return lines_[unbox<long>(_line)];
}

template <typename Cell>
Line<Cell> const& Grid<Cell>::lineAt(LineOffset _line) const noexcept
{
    return const_cast<Grid&>(*this).lineAt(_line);
}

template <typename Cell>
Cell& Grid<Cell>::at(LineOffset _line, ColumnOffset _column) noexcept
{
    return useCellAt(_line, _column);
}

template <typename Cell>
Cell& Grid<Cell>::useCellAt(LineOffset _line, ColumnOffset _column) noexcept
{
    return lineAt(_line).useCellAt(_column);
}

template <typename Cell>
Cell const& Grid<Cell>::at(LineOffset _line, ColumnOffset _column) const noexcept
{
    return const_cast<Grid&>(*this).at(_line, _column);
}

template <typename Cell>
gsl::span<Line<Cell>> Grid<Cell>::pageAtScrollOffset(ScrollOffset _scrollOffset)
{
    Expects(unbox<LineCount>(_scrollOffset) <= historyLineCount());

    int const offset = -*_scrollOffset;
    Line<Cell>* startLine = &lines_[offset];
    auto const count = unbox<size_t>(pageSize_.lines);

    return gsl::span<Line<Cell>>{startLine, count};
}

template <typename Cell>
gsl::span<Line<Cell> const> Grid<Cell>::pageAtScrollOffset(ScrollOffset _scrollOffset) const
{
    Expects(unbox<LineCount>(_scrollOffset) <= historyLineCount());

    int const offset = -*_scrollOffset;
    Line<Cell> const* startLine = &lines_[offset];
    auto const count = unbox<size_t>(pageSize_.lines);

    return gsl::span<Line<Cell> const>{startLine, count};
}

template <typename Cell>
gsl::span<Line<Cell> const> Grid<Cell>::mainPage() const
{
    return pageAtScrollOffset({});
}

template <typename Cell>
gsl::span<Line<Cell>> Grid<Cell>::mainPage()
{
    return pageAtScrollOffset({});
}
// }}}
// {{{ Grid impl: Line access
template <typename Cell>
gsl::span<Cell const> Grid<Cell>::lineBufferRightTrimmed(LineOffset _line) const noexcept
{
    return detail::trimRight(lineBuffer(_line));
}

template <typename Cell>
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
std::string Grid<Cell>::lineTextTrimmed(LineOffset _line) const
{
    std::string output = lineText(_line);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

template <typename Cell>
std::string Grid<Cell>::lineText(Line<Cell> const& _line) const
{
    std::stringstream sstr;
    for (Cell const& cell: lineBuffer(_line))
    {
        if (cell.codepointCount() == 0)
            sstr << ' ';
        else
            sstr << cell.toUtf8();
    }
    return sstr.str();
}

template <typename Cell>
void Grid<Cell>::setLineText(LineOffset _line, std::string_view _text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(_text))
        useCellAt(_line, ColumnOffset::cast_from(i++)).setCharacter(ch);
}

template <typename Cell>
bool Grid<Cell>::isLineBlank(LineOffset _line) const noexcept
{
    auto const is_blank = [](auto const& _cell) noexcept
    {
        return _cell.empty();
    };

    auto const line = lineBuffer(_line);
    return std::all_of(line.begin(), line.end(), is_blank);
}

/**
 * Computes the relative line number for the bottom-most @p _n logical lines.
 */
template <typename Cell>
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
        //printf("further upwards: l %d, p %d\n", logicalLineCount, outputRelativePhysicalLine);
        outputRelativePhysicalLine--;
        ++i;
    }

    return outputRelativePhysicalLine;
}
// }}}
// {{{ Grid impl: scrolling
template <typename Cell>
LineCount Grid<Cell>::scrollUp(LineCount _n, GraphicsAttributes _defaultAttributes) noexcept
{
    verifyState();
    if (linesUsed_ == totalLineCount()) // with all grid lines in-use
    {
        // TODO: ensure explicit test for this case
        rotateBuffersLeft(_n);

        // Initialize (/reset) new lines.
        for (auto y = boxed_cast<LineOffset>(pageSize_.lines - _n); y < boxed_cast<LineOffset>(pageSize_.lines); ++y)
            lineAt(y).reset(defaultLineFlags(), _defaultAttributes);

        return _n;
    }
    else
    {
        // TODO: ensure explicit test for this case
        auto const linesAvailable = lines_.size() - unbox<size_t>(linesUsed_);
        auto const n = std::min(unbox<size_t>(_n), linesAvailable);
        if (n != 0)
        {
            linesUsed_.value += n;
            fill_n(
                next(lines_.begin(), *pageSize_.lines),
                n,
                Line{pageSize_.columns, defaultLineFlags(), Cell{_defaultAttributes}}
            );
            rotateBuffersLeft(LineCount::cast_from(n));
        }
        if (n < unbox<size_t>(_n))
        {
            auto const incrementCount = unbox<size_t>(_n) - n;
            linesUsed_ += LineCount::cast_from(incrementCount);

            // Initialize (/reset) new lines.
            for (auto y = boxed_cast<LineOffset>(pageSize_.lines - _n); y < boxed_cast<LineOffset>(pageSize_.lines); ++y)
                lineAt(y).reset(defaultLineFlags(), _defaultAttributes);
        }
        return LineCount::cast_from(n);
    }
}

template <typename Cell>
LineCount Grid<Cell>::scrollUp(LineCount _n, GraphicsAttributes _defaultAttributes, Margin _margin) noexcept
{
    verifyState();
    Expects(0 <= *_margin.horizontal.from && *_margin.horizontal.to < *pageSize_.columns);
    Expects(0 <= *_margin.vertical.from && *_margin.vertical.to < *pageSize_.lines);

    // these two booleans could be cached and updated whenever _margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal = _margin.horizontal == Margin::Horizontal{ColumnOffset{0}, unbox<ColumnOffset>(pageSize_.columns) - 1};
    auto const fullVertical = _margin.vertical == Margin::Vertical{LineOffset(0), unbox<LineOffset>(pageSize_.lines) - 1};

    if (fullHorizontal)
    {
        if (fullVertical) // full-screen scroll-up
            return scrollUp(_n, _defaultAttributes);

        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = LineCount(_margin.vertical.length());
        auto const n = std::min(_n, marginHeight);
        if (n < marginHeight)
        {
            // rotate line attribs
            auto u = next(begin(lines_), *_margin.vertical.from);
            auto v = next(begin(lines_), *_margin.vertical.from + *n);
            auto w = next(begin(lines_), *_margin.vertical.to + 1);
            rotate(u, v, w);
        }

        std::for_each(
            next(begin(lines_), *_margin.vertical.to - *n + 1),
            next(begin(lines_), *_margin.vertical.to + 1),
            [&](Line<Cell>& line) {
                line.reset(defaultLineFlags(), _defaultAttributes);
            }
        );
    }
    else
    {
        // a full "inside" scroll-up
        auto const marginHeight = _margin.vertical.length();
        auto const n = std::min(_n, marginHeight);

        if (n <= marginHeight)
        {
            for (LineOffset line = _margin.vertical.from; line <= _margin.vertical.from + *n; ++line)
            {
                auto t = &useCellAt(line, _margin.horizontal.from);
                auto s = &at(line + *n, _margin.horizontal.from);
                std::copy_n(s, unbox<size_t>(_margin.horizontal.length()), t);
            }
            for (LineOffset line = _margin.vertical.from + *n + 1; line <= _margin.vertical.to; ++line)
            {
                auto a = &useCellAt(line, _margin.horizontal.from);
                auto b = &at(line, _margin.horizontal.to + 1);
                while (a != b)
                {
                    a->reset(_defaultAttributes);
                    a++;
                }
            }
        }
    }
    verifyState();
    return LineCount(0); // No full-margin lines scrolled up.
}

template <typename Cell>
void Grid<Cell>::scrollDown(LineCount v_n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin)
{
    verifyState();
    Expects(v_n >= LineCount(0));

    // these two booleans could be cached and updated whenever _margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal = _margin.horizontal == Margin::Horizontal{ColumnOffset{0}, unbox<ColumnOffset>(pageSize_.columns) - ColumnOffset(1)};
    auto const fullVertical = _margin.vertical == Margin::Vertical{LineOffset(0), unbox<LineOffset>(pageSize_.lines) - LineOffset(1)};

    auto const n = std::min(v_n, _margin.vertical.length());

    if (fullHorizontal && fullVertical)
    {
        // full screen scrolling

        // on the full screen (all lines)
        // move all lines up by N lines
        // bottom N lines are wiped out

        auto const marginHeight = LineCount(_margin.vertical.length());
        std::rotate(
            std::begin(mainPage()),
            std::next(begin(mainPage()), *marginHeight - *n),
            std::end(mainPage())
        );
        for (Line<Cell>& line: mainPage().subspan(0, *n))
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
            for (LineOffset line = _margin.vertical.to;
                    line >= _margin.vertical.to - *n; --line)
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
                    *a = Cell{_defaultAttributes};
                    a++;
                }
            }
        }
    }
}
// }}}
// {{{ Grid impl: resize
template <typename Cell>
void Grid<Cell>::reset()
{
    linesUsed_ = pageSize_.lines;
    lines_.rotate_right(lines_.zero_index());
    for (int i = 0; i < unbox<int>(pageSize_.lines); ++i)
        lines_[i].reset(defaultLineFlags(), GraphicsAttributes{});
    verifyState();
}

template <typename Cell>
Coordinate Grid<Cell>::resize(PageSize _newSize, Coordinate _currentCursorPos, bool _wrapPending)
{
    if (pageSize_ == _newSize)
        return _currentCursorPos;

    LOGSTORE(GridLog)("resize {} -> {} (cursor {})", pageSize_, _newSize, _currentCursorPos);

    // Growing in line count with scrollback lines present will move
    // the scrollback lines into the visible area.
    //
    // Shrinking in line count with the cursor at the bottom margin will move
    // the top lines into the scrollback area.

    using LineBuffer = typename Line<Cell>::Buffer;

    // {{{ helper methods
    auto const growLines = [this](LineCount _newHeight, Coordinate _cursor) -> Coordinate
    {
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until pageSize_.lines == _newHeight.

        Expects(_newHeight > pageSize_.lines);
        //lines_.reserve(unbox<size_t>(maxHistoryLineCount_ + _newHeight));

        // Pull down from history if cursor is at bottom and if scrollback available.
        Coordinate cursorMove{};
        if (*_cursor.line + 1 == *pageSize_.lines)
        {
            auto const extendCount0 = _newHeight - pageSize_.lines;
            auto const rowsToTakeFromSavedLines = std::min(extendCount0, historyLineCount());
            Expects(extendCount0 >= rowsToTakeFromSavedLines);
            Expects(*rowsToTakeFromSavedLines >= 0);
            rotateBuffersRight(rowsToTakeFromSavedLines);
            pageSize_.lines += rowsToTakeFromSavedLines;
            cursorMove.line += boxed_cast<LineOffset>(rowsToTakeFromSavedLines);
        }

        auto const wrappableFlag = lines_.back().wrappableFlag();
        auto const extendCount = _newHeight - pageSize_.lines;
        auto const rowsToTakeFromSavedLines = std::min(extendCount, historyLineCount());
        Expects(*extendCount >= 0);
        // ? Expects(rowsToTakeFromSavedLines == LineCount(0));

        auto const linesFill =
            max(0, unbox<int>(maxHistoryLineCount_ + _newHeight) - static_cast<int>(lines_.size()));
        for (auto const _: ranges::views::iota(0, linesFill))
            lines_.emplace_back(pageSize_.columns, wrappableFlag, Cell{});
        pageSize_.lines += extendCount;
        linesUsed_ += extendCount;

        Ensures(pageSize_.lines == _newHeight);
        Ensures(lines_.size() >= unbox<size_t>(maxHistoryLineCount_ + pageSize_.lines));
        verifyState();

        return cursorMove;
    };

    auto const shrinkLines = [this](LineCount _newHeight, Coordinate _cursor) -> Coordinate
    {
        // Shrink existing line count to _newSize.lines
        // by splicing the number of lines to be shrinked by into savedLines bottom.

        Expects(_newHeight < pageSize_.lines);

        // FIXME: in alt screen, when shrinking more then available below screen cursor -> assertion failure

        auto const numLinesToShrink = pageSize_.lines - _newHeight;
        auto const linesAvailableBelowCursorBeforeShrink = pageSize_.lines - boxed_cast<LineCount>(_cursor.line + 1);
        auto const cutoffCount = min(numLinesToShrink, linesAvailableBelowCursorBeforeShrink);
        auto const numLinesToPushUp = numLinesToShrink - cutoffCount;
        auto const numLinesToPushUpCapped = min(numLinesToPushUp, maxHistoryLineCount_);

        LOGSTORE(GridLog)(" -> shrink lines: numLinesToShrink {}, linesAvailableBelowCursorBeforeShrink {}, cutoff {}, pushUp {}/{}",
                          numLinesToShrink, linesAvailableBelowCursorBeforeShrink, cutoffCount,
                          numLinesToPushUp, numLinesToPushUpCapped);

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
        Expects(_newHeight <= pageSize_.lines);
        if (*numLinesToPushUp)
        {
            LOGSTORE(GridLog)(" -> numLinesToPushUp {}", numLinesToPushUp);
            Expects(*_cursor.line + 1 == *pageSize_.lines);
            rotateBuffersLeft(numLinesToPushUp);
            pageSize_.lines -= numLinesToPushUp;
            linesUsed_ -= numLinesToPushUp;
            clampHistory();
            verifyState();
            return Coordinate{-boxed_cast<LineOffset>(numLinesToPushUp), {}};
        }

        verifyState();
        return Coordinate{};
    };

    auto const growColumns = [this, _wrapPending](ColumnCount _newColumnCount) -> Coordinate
    {
        using LineBuffer = typename Line<Cell>::Buffer;

        if (!reflowOnResize_)
        {
            for (auto& line: lines_)
                if (line.size() < _newColumnCount)
                    line.resize(_newColumnCount);
            pageSize_.columns = _newColumnCount;
            verifyState();
            return Coordinate{LineOffset(0), ColumnOffset(_wrapPending ? 1 : 0)};
        }
        else
        {
            // Grow columns by inverse shrink,
            // i.e. the lines are traversed in reverse order.

            auto const extendCount = _newColumnCount - pageSize_.columns;
            Expects(*extendCount > 0);

            Lines<Cell> grownLines;
            LineBuffer logicalLineBuffer; // Temporary state, representing wrapped columns from the line "below".
            LineFlags logicalLineFlags = LineFlags::None;

            auto const appendToLogicalLine =
                [&logicalLineBuffer](gsl::span<Cell const> cells)
                {
                    for (auto const& cell: cells)
                        logicalLineBuffer.push_back(cell);
                };

            auto const flushLogicalLine =
                [_newColumnCount, &grownLines, &logicalLineBuffer, &logicalLineFlags]()
                {
                    if (!logicalLineBuffer.empty())
                    {
                        detail::addNewWrappedLines(grownLines, _newColumnCount, move(logicalLineBuffer), logicalLineFlags, true);
                        logicalLineBuffer.clear();
                    }
                };

            [[maybe_unused]] auto const logLogicalLine =
                [&logicalLineBuffer]([[maybe_unused]] LineFlags lineFlags, [[maybe_unused]] std::string_view msg)
                {
                    LOGSTORE(GridLog)("{} |> \"{}\"",
                        msg,
                        Line<Cell>(LineBuffer(logicalLineBuffer), lineFlags).toUtf8()
                    );
                };

            for (int i = -*historyLineCount(); i < *pageSize_.lines; ++i)
            {
                auto& line = lines_[i];
                // logLogicalLine(line.flags(), fmt::format("Line[{:>2}]: next line: \"{}\"", i, line.toUtf8()));
                Expects(line.size() >= pageSize_.columns);

                if (line.wrapped())
                {
                    // logLogicalLine(line.flags(), fmt::format(" - appending: \"{}\"", line.toUtf8Trimmed()));
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

            //auto diff = int(lines_.size()) - unbox<int>(pageSize_.lines);
            auto cy = LineCount(0);
            if (pageSize_.lines > LineCount::cast_from(grownLines.size()))
            {
                // The lines we've been reflowing do not fill the pageSize,
                // so fill the gap until we have a full page.
                cy = pageSize_.lines - LineCount::cast_from(grownLines.size());
                while (LineCount::cast_from(grownLines.size()) < pageSize_.lines)
                    grownLines.emplace_back(_newColumnCount, defaultLineFlags());

                Ensures(LineCount::cast_from(grownLines.size()) == pageSize_.lines);
            }

            linesUsed_ = LineCount::cast_from(grownLines.size());

            // Fill scrollback lines.
            auto const totalLineCount = unbox<size_t>(pageSize_.lines + maxHistoryLineCount_);
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(_newColumnCount, defaultLineFlags());

            lines_ = move(grownLines);
            pageSize_.columns = _newColumnCount;

            auto const newHistoryLineCount = linesUsed_ - pageSize_.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return Coordinate{-boxed_cast<LineOffset>(cy), ColumnOffset(_wrapPending ? 1 : 0)};
        }
    };

    auto const shrinkColumns = [this](ColumnCount _newColumnCount, LineCount _newLineCount, Coordinate _cursor) -> Coordinate
    {
        using LineBuffer = typename Line<Cell>::Buffer;

        if (!reflowOnResize_)
        {
            pageSize_.columns = _newColumnCount;
            crispy::for_each(lines_, [=](Line<Cell>& line) {
                if (line.size() < _newColumnCount)
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

            auto const totalLineCount = unbox<size_t>(pageSize_.lines + maxHistoryLineCount_);
            shrinkedLines.reserve(totalLineCount);
            Expects(totalLineCount == unbox<size_t>(this->totalLineCount()));

            LineCount numLinesWritten = LineCount(0);
            for (auto i = -*historyLineCount(); i < *pageSize_.lines; ++i)
            {
                auto& line = lines_[i];

                // do we have previous columns carried?
                if (!wrappedColumns.empty())
                {
                    if (line.wrapped() && line.inheritableFlags() == previousFlags)
                    {
                        // Prepend previously wrapped columns into current line.
                        auto& editable = line.editable();
                        editable.insert(editable.begin(), wrappedColumns.begin(), wrappedColumns.end());
                    }
                    else
                    {
                        // Insert NEW line(s) between previous and this line with previously wrapped columns.
                        auto const numLinesInserted = detail::addNewWrappedLines(shrinkedLines, _newColumnCount, move(wrappedColumns), previousFlags, false);
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
            numLinesWritten += detail::addNewWrappedLines(shrinkedLines, _newColumnCount, move(wrappedColumns), previousFlags, false);
            Expects(unbox<size_t>(numLinesWritten) == shrinkedLines.size());
            Expects(numLinesWritten >= pageSize_.lines);

            while (shrinkedLines.size() < totalLineCount)
                shrinkedLines.emplace_back(_newColumnCount, LineFlags::None); // defaultLineFlags());

            shrinkedLines.rotate_left(unbox<int>(numLinesWritten - pageSize_.lines)); // maybe to be done outisde?
            linesUsed_ = LineCount::cast_from(numLinesWritten);

            // if (LineCount::cast_from(shrinkedLines.size()) > pageSize_.lines)
            // {
            //     auto const overflow = LineCount::cast_from(shrinkedLines.size()) -
            //     pageSize_.lines;
            //     linesUsed_ -= overflow;
            // }

            lines_ = move(shrinkedLines);
            pageSize_.columns = _newColumnCount;

            verifyState();
            return _cursor; // TODO
        }
    };
    // }}}

    Coordinate cursor = _currentCursorPos;

    // grow/shrink columns
    using crispy::Comparison;
    switch (crispy::strongCompare(_newSize.columns, pageSize_.columns))
    {
        case Comparison::Greater:
            cursor += growColumns(_newSize.columns);
            break;
        case Comparison::Less:
            cursor = shrinkColumns(_newSize.columns, _newSize.lines, cursor);
            break;
        case Comparison::Equal:
            break;
    }

    // grow/shrink lines
    switch (crispy::strongCompare(_newSize.lines, pageSize_.lines))
    {
        case Comparison::Greater:
            cursor += growLines(_newSize.lines, cursor);
            break;
        case Comparison::Less:
            cursor += shrinkLines(_newSize.lines, cursor);
            break;
        case Comparison::Equal:
            break;
    }

    Ensures(pageSize_ == _newSize);
    verifyState();

    return cursor;
}

template <typename Cell>
void Grid<Cell>::clampHistory()
{
    // TODO: needed?
}

template <typename Cell>
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
        generate_n(
            back_inserter(lines_),
            *n,
            [&]() { return Line<Cell>(pageSize_.columns, wrappableFlag, Cell{_attr}); }
        );
        clampHistory();
    }
}
// }}}
// {{{ dumpGrid impl
template <typename Cell>
std::ostream& dumpGrid(std::ostream& os, Grid<Cell> const& grid)
{
    os << fmt::format("main page lines: scrollback cur {} max {}, main page lines {}, used lines {}, zero index {}\n",
                      grid.historyLineCount(),
                      grid.maxHistoryLineCount(),
                      grid.pageSize().lines,
                      grid.linesUsed(),
                      grid.zero_index());

    for (auto const line: ranges::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines)))
    {
        terminal::Line<Cell> const& lineAttribs = grid.lineAt(LineOffset(line));

        os << fmt::format("[{:>2}] \"{}\" | {}\n",
                          line, grid.lineText(LineOffset::cast_from(line)),
                          lineAttribs.flags());
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

template class Grid<Cell>;
template std::string dumpGrid<Cell>(Grid<Cell> const& grid);

} // end namespace terminal
