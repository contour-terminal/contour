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

#include "vtbackend/Line.h"
#include "vtbackend/cell/SimpleCell.h"

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
    lines<Cell> createLines(PageSize pageSize,
                            LineCount maxHistoryLineCount,
                            bool reflowOnResize,
                            graphics_attributes initialSGR)
    {
        auto const defaultLineFlags = reflowOnResize ? line_flags::Wrappable : line_flags::None;
        auto const totalLineCount = unbox<size_t>(pageSize.lines + maxHistoryLineCount);

        lines<Cell> lines;
        lines.reserve(totalLineCount);

        for ([[maybe_unused]] auto const _: ranges::views::iota(0u, totalLineCount))
            lines.emplace_back(defaultLineFlags, trivial_line_buffer { pageSize.columns, initialSGR });

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
        lines<Cell>& targetLines,
        ColumnCount newColumnCount,
        typename line<Cell>::inflated_buffer&& logicalLineBuffer, // TODO: don't move, do (c)ref instead
        line_flags baseFlags,
        bool initialNoWrap // TODO: pass `LineFlags defaultLineFlags` instead?
    )
    {
        using line_buffer = typename line<Cell>::inflated_buffer;

        // TODO: avoid unnecessary copies via erase() by incrementally updating (from, to)
        int i = 0;

        while (logicalLineBuffer.size() >= unbox<size_t>(newColumnCount))
        {
            auto from = logicalLineBuffer.begin();
            auto to = from + newColumnCount.as<std::ptrdiff_t>();
            auto const wrappedFlag = i == 0 && initialNoWrap ? line_flags::None : line_flags::Wrapped;
            targetLines.emplace_back(baseFlags | wrappedFlag, line_buffer(from, to));
            logicalLineBuffer.erase(from, to);
            ++i;
        }

        if (logicalLineBuffer.size() > 0)
        {
            auto const wrappedFlag = i == 0 && initialNoWrap ? line_flags::None : line_flags::Wrapped;
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
grid<Cell>::grid(PageSize pageSize, bool reflowOnResize, max_history_line_count maxHistoryLineCount):
    _pageSize { pageSize },
    _margin { margin::vertical { {}, _pageSize.lines.as<line_offset>() - line_offset(1) },
              margin::horizontal { {}, _pageSize.columns.as<column_offset>() - column_offset(1) } },
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
        graphics_attributes {}) },
    _linesUsed { pageSize.lines }
{
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void grid<Cell>::setMaxHistoryLineCount(max_history_line_count maxHistoryLineCount)
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
void grid<Cell>::clearHistory()
{
    _linesUsed = _pageSize.lines;
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void grid<Cell>::verifyState() const noexcept
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
std::string grid<Cell>::renderAllText() const
{
    std::string text;
    text.reserve(unbox<size_t>(historyLineCount() + _pageSize.lines) * unbox<size_t>(_pageSize.columns + 1));

    for (auto y = line_offset(0); y < line_offset::cast_from(_lines.size()); ++y)
    {
        text += lineText(y);
        text += '\n';
    }

    return text;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string grid<Cell>::renderMainPageText() const
{
    std::string text;

    for (auto line = line_offset(0); line < unbox<line_offset>(_pageSize.lines); ++line)
    {
        text += lineText(line);
        text += '\n';
    }

    return text;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
line<Cell>& grid<Cell>::lineAt(line_offset line) noexcept
{
    // Require(*line < *_pageSize.lines);
    return _lines[unbox<long>(line)];
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
line<Cell> const& grid<Cell>::lineAt(line_offset line) const noexcept
{
    // Require(*line < *_pageSize.lines);
    return const_cast<grid&>(*this).lineAt(line);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& grid<Cell>::at(line_offset line, column_offset column) noexcept
{
    return useCellAt(line, column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& grid<Cell>::useCellAt(line_offset line, column_offset column) noexcept
{
    return lineAt(line).useCellAt(column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell const& grid<Cell>::at(line_offset line, column_offset column) const noexcept
{
    return const_cast<grid&>(*this).at(line, column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<line<Cell>> grid<Cell>::pageAtScrollOffset(scroll_offset scrollOffset)
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    line<Cell>* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<line<Cell>> { startLine, count };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<line<Cell> const> grid<Cell>::pageAtScrollOffset(scroll_offset scrollOffset) const
{
    Require(unbox<LineCount>(scrollOffset) <= historyLineCount());

    int const offset = -*scrollOffset;
    line<Cell> const* startLine = &_lines[offset];
    auto const count = unbox<size_t>(_pageSize.lines);

    return gsl::span<line<Cell> const> { startLine, count };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<line<Cell> const> grid<Cell>::mainPage() const
{
    return pageAtScrollOffset({});
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<line<Cell>> grid<Cell>::mainPage()
{
    return pageAtScrollOffset({});
}
// }}}
// {{{ Grid impl: Line access
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Cell const> grid<Cell>::lineBufferRightTrimmed(line_offset line) const noexcept
{
    return detail::trimRight(lineBuffer(line));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string grid<Cell>::lineText(line_offset lineOffset) const
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
std::string grid<Cell>::lineTextTrimmed(line_offset lineOffset) const
{
    std::string output = lineText(lineOffset);
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string grid<Cell>::lineText(line<Cell> const& line) const
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
void grid<Cell>::setLineText(line_offset line, std::string_view text)
{
    size_t i = 0;
    for (auto const ch: unicode::convert_to<char32_t>(text))
        useCellAt(line, column_offset::cast_from(i++)).setCharacter(ch);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool grid<Cell>::isLineBlank(line_offset line) const noexcept
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
int grid<Cell>::computeLogicalLineNumberFromBottom(LineCount n) const noexcept
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
LineCount grid<Cell>::scrollUp(LineCount linesCountToScrollUp, graphics_attributes defaultAttributes) noexcept
{
    verifyState();
    // Number of lines in the ring buffer that are not yet
    // used by the grid system.
    auto const linesAvailable = LineCount::cast_from(_lines.size() - unbox<size_t>(_linesUsed));
    if (std::holds_alternative<infinite>(_historyLimit) && linesAvailable < linesCountToScrollUp)
    {
        auto const linesToAllocate = unbox<int>(linesCountToScrollUp - linesAvailable);

        for ([[maybe_unused]] auto const _: ranges::views::iota(0, linesToAllocate))
        {
            _lines.emplace_back(defaultLineFlags(),
                                trivial_line_buffer { _pageSize.columns, graphics_attributes() });
        }
        return scrollUp(linesCountToScrollUp, defaultAttributes);
    }
    if (unbox<size_t>(_linesUsed) == _lines.size()) // with all grid lines in-use
    {
        // TODO: ensure explicit test for this case
        rotateBuffersLeft(linesCountToScrollUp);

        // Initialize (/reset) new lines.
        for (auto y = boxed_cast<line_offset>(_pageSize.lines - linesCountToScrollUp);
             y < boxed_cast<line_offset>(_pageSize.lines);
             ++y)
            lineAt(y).reset(defaultLineFlags(), defaultAttributes);

        return linesCountToScrollUp;
    }
    else
    {
        Require(unbox<size_t>(_linesUsed) < _lines.size());

        // Number of lines in the ring buffer that we can allocate at the head.
        auto const linesAppendCount = std::min(linesCountToScrollUp, linesAvailable);

        if (*linesAppendCount != 0)
        {
            _linesUsed += linesAppendCount;
            Require(unbox<size_t>(_linesUsed) <= _lines.size());
            fill_n(next(_lines.begin(), *_pageSize.lines),
                   unbox<size_t>(linesAppendCount),
                   line<Cell> { defaultLineFlags(),
                                trivial_line_buffer { _pageSize.columns, defaultAttributes } });
            rotateBuffersLeft(linesAppendCount);
        }
        if (linesAppendCount < linesCountToScrollUp)
        {
            auto const incrementCount = linesCountToScrollUp - linesAppendCount;
            rotateBuffersLeft(incrementCount);

            // Initialize (/reset) new lines.
            for (auto y = boxed_cast<line_offset>(_pageSize.lines - linesCountToScrollUp);
                 y < boxed_cast<line_offset>(_pageSize.lines);
                 ++y)
                lineAt(y).reset(defaultLineFlags(), defaultAttributes);
        }
        return LineCount::cast_from(linesAppendCount);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
LineCount grid<Cell>::scrollUp(LineCount n, graphics_attributes defaultAttributes, margin m) noexcept
{
    verifyState();
    Require(0 <= *m.hori.from && *m.hori.to < *_pageSize.columns);
    Require(0 <= *m.vert.from && *m.vert.to < *_pageSize.lines);

    // these two booleans could be cached and updated whenever margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        m.hori == margin::horizontal { column_offset { 0 }, unbox<column_offset>(_pageSize.columns) - 1 };
    auto const fullVertical =
        m.vert == margin::vertical { line_offset(0), unbox<line_offset>(_pageSize.lines) - 1 };

    if (fullHorizontal)
    {
        if (fullVertical) // full-screen scroll-up
            return scrollUp(n, defaultAttributes);

        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = LineCount(m.vert.length());
        auto const n2 = std::min(n, marginHeight);
        if (*n2 && n2 < marginHeight)
        {
            // rotate line attribs
            for (auto topLineOffset = *m.vert.from; topLineOffset <= *m.vert.to - *n2; ++topLineOffset)
                _lines[topLineOffset] = std::move(_lines[topLineOffset + *n2]);
        }

        auto const topEmptyLineNr = *m.vert.to - *n2 + 1;
        auto const bottomLineNumber = *m.vert.to;
        for (auto lineNumber = topEmptyLineNr; lineNumber <= bottomLineNumber; ++lineNumber)
        {
            _lines[lineNumber].reset(defaultLineFlags(), defaultAttributes, _pageSize.columns);
        }
    }
    else
    {
        // a full "inside" scroll-up
        auto const marginHeight = m.vert.length();
        auto const n2 = std::min(n, marginHeight);
        // auto const bottomLineOffsetToCopy = min(margin.vertical.from + *n2, margin.vertical.to - 1);
        auto const topTargetLineOffset = m.vert.from;
        auto const bottomTargetLineOffset = m.vert.to - *n2;
        auto const columnsToMove = unbox<size_t>(m.hori.length());

        for (line_offset targetLineOffset = topTargetLineOffset; targetLineOffset <= bottomTargetLineOffset;
             ++targetLineOffset)
        {
            auto const sourceLineOffset = targetLineOffset + *n2;
            auto t = &useCellAt(targetLineOffset, m.hori.from);
            auto s = &at(sourceLineOffset, m.hori.from);
            std::copy_n(s, columnsToMove, t);
        }

        for (line_offset line = m.vert.to - *n2 + 1; line <= m.vert.to; ++line)
        {
            auto a = &useCellAt(line, m.hori.from);
            auto b = a + unbox<int>(m.hori.length());
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
void grid<Cell>::scrollDown(LineCount vN, graphics_attributes const& defaultAttributes, margin const& m)
{
    verifyState();
    Require(vN >= LineCount(0));

    // these two booleans could be cached and updated whenever margin updates,
    // so not even this needs to be computed for the general case.
    auto const fullHorizontal =
        m.hori
        == margin::horizontal { column_offset { 0 },
                                unbox<column_offset>(_pageSize.columns) - column_offset(1) };
    auto const fullVertical =
        m.vert == margin::vertical { line_offset(0), unbox<line_offset>(_pageSize.lines) - line_offset(1) };

    auto const n = std::min(vN, m.vert.length());

    if (fullHorizontal && fullVertical)
    {
        // full screen scrolling

        // on the full screen (all lines)
        // move all lines up by N lines
        // bottom N lines are wiped out

        rotateBuffersRight(n);

        for (line<Cell>& line: mainPage().subspan(0, unbox<size_t>(n)))
            line.reset(defaultLineFlags(), defaultAttributes);
        return;
    }

    if (fullHorizontal) // => but ont fully vertical
    {
        // scroll down only inside vertical margin with full horizontal extend
        auto a = std::next(begin(_lines), *m.vert.from);
        auto b = std::next(begin(_lines), *m.vert.to + 1 - *n);
        auto c = std::next(begin(_lines), *m.vert.to + 1);
        std::rotate(a, b, c);
        for (auto const i: ranges::views::iota(*m.vert.from, *m.vert.from + *n))
            _lines[i].reset(defaultLineFlags(), defaultAttributes);
    }
    else
    {
        // a full "inside" scroll-down
        if (n <= m.vert.length())
        {
            for (line_offset line = m.vert.to; line >= m.vert.to - *n; --line)
            {
                auto s = &at(line - *n, m.hori.from);
                auto t = &at(line, m.hori.from);
                std::copy_n(s, unbox<size_t>(m.hori.length()), t);
            }

            for (line_offset line = m.vert.from; line < m.vert.from + *n; ++line)
            {
                auto a = &at(line, m.hori.from);
                auto b = &at(line, m.hori.to + 1);
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
void grid<Cell>::scrollLeft(graphics_attributes defaultAttributes, margin m) noexcept
{
    for (line_offset lineNo = m.vert.from; lineNo <= m.vert.to; ++lineNo)
    {
        auto& line = lineAt(lineNo);
        auto column0 = line.inflatedBuffer().begin() + *m.hori.from;
        auto column1 = line.inflatedBuffer().begin() + *m.hori.from + 1;
        auto column2 = line.inflatedBuffer().begin() + *m.hori.to + 1;
        std::rotate(column0, column1, column2);

        auto const emptyCell = Cell { defaultAttributes };
        auto const emptyCellsBegin = line.inflatedBuffer().begin() + *m.hori.to;
        std::fill_n(emptyCellsBegin, 1, emptyCell);
    }
}

// }}}
// {{{ Grid impl: resize
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void grid<Cell>::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotate_right(_lines.zero_index());
    for (int i = 0; i < unbox<int>(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), graphics_attributes {});
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
cell_location grid<Cell>::growLines(LineCount newHeight, cell_location cursor)
{
    // Grow line count by splicing available lines from history back into buffer, if available,
    // or create new ones until _pageSize.lines == newHeight.

    Require(newHeight > _pageSize.lines);
    // _lines.reserve(unbox<size_t>(_maxHistoryLineCount + newHeight));

    // Pull down from history if cursor is at bottom and if scrollback available.
    cell_location cursorMove {};
    if (*cursor.line + 1 == *_pageSize.lines)
    {
        auto const totalLinesToExtend = newHeight - _pageSize.lines;
        auto const linesToTakeFromSavedLines = std::min(totalLinesToExtend, historyLineCount());
        Require(totalLinesToExtend >= linesToTakeFromSavedLines);
        Require(*linesToTakeFromSavedLines >= 0);
        rotateBuffersRight(linesToTakeFromSavedLines);
        _pageSize.lines += linesToTakeFromSavedLines;
        cursorMove.line += boxed_cast<line_offset>(linesToTakeFromSavedLines);
    }

    auto const wrappableFlag = _lines.back().wrappableFlag();
    auto const totalLinesToExtend = newHeight - _pageSize.lines;
    Require(*totalLinesToExtend >= 0);
    // ? Require(linesToTakeFromSavedLines == LineCount(0));

    auto const newTotalLineCount = maxHistoryLineCount() + newHeight;
    auto const currentTotalLineCount = LineCount::cast_from(_lines.size());
    auto const linesToFill = max(0, *newTotalLineCount - *currentTotalLineCount);

    for ([[maybe_unused]] auto const _: ranges::views::iota(0, linesToFill))
        _lines.emplace_back(wrappableFlag, trivial_line_buffer { _pageSize.columns, graphics_attributes {} });

    _pageSize.lines += totalLinesToExtend;
    _linesUsed = min(_linesUsed + totalLinesToExtend, LineCount::cast_from(_lines.size()));

    Ensures(_pageSize.lines == newHeight);
    Ensures(_lines.size() >= unbox<size_t>(maxHistoryLineCount() + _pageSize.lines));
    verifyState();

    return cursorMove;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
cell_location grid<Cell>::resize(PageSize newSize, cell_location currentCursorPos, bool wrapPending)
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
    auto const shrinkLines = [this](LineCount newHeight, cell_location cursor) -> cell_location {
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
            return cell_location { -boxed_cast<line_offset>(numLinesToPushUp), {} };
        }

        verifyState();
        return cell_location {};
    };

    auto const growColumns = [this, wrapPending](ColumnCount newColumnCount) -> cell_location {
        using line_buffer = typename line<Cell>::inflated_buffer;

        if (!_reflowOnResize)
        {
            for (auto& line: _lines)
                if (line.size() < newColumnCount)
                    line.resize(newColumnCount);
            _pageSize.columns = newColumnCount;
            verifyState();
            return cell_location { line_offset(0), column_offset(wrapPending ? 1 : 0) };
        }
        else
        {
            // Grow columns by inverse shrink,
            // i.e. the lines are traversed in reverse order.

            auto const extendCount = newColumnCount - _pageSize.columns;
            Require(*extendCount > 0);

            lines<Cell> grownLines;
            line_buffer
                logicalLineBuffer; // Temporary state, representing wrapped columns from the line "below".
            line_flags logicalLineFlags = line_flags::None;

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
                [&logicalLineBuffer]([[maybe_unused]] line_flags lineFlags,
                                     [[maybe_unused]] std::string_view msg) {
                    GridLog()("{} |> \"{}\"", msg, line<Cell>(lineFlags, logicalLineBuffer).toUtf8());
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
                        logicalLineFlags = line.flags() & ~line_flags::Wrapped;
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
                    grownLines.emplace_back(defaultLineFlags(),
                                            trivial_line_buffer { newColumnCount,
                                                                  graphics_attributes {},
                                                                  graphics_attributes {} });

                Ensures(LineCount::cast_from(grownLines.size()) == _pageSize.lines);
            }

            _linesUsed = LineCount::cast_from(grownLines.size());

            // Fill scrollback lines.
            auto const totalLineCount = unbox<size_t>(_pageSize.lines + maxHistoryLineCount());
            while (grownLines.size() < totalLineCount)
                grownLines.emplace_back(
                    defaultLineFlags(),
                    trivial_line_buffer { newColumnCount, graphics_attributes {}, graphics_attributes {} });

            _lines = std::move(grownLines);
            _pageSize.columns = newColumnCount;

            auto const newHistoryLineCount = _linesUsed - _pageSize.lines;
            rotateBuffersLeft(newHistoryLineCount);

            verifyState();
            return cell_location { -boxed_cast<line_offset>(cy), column_offset(wrapPending ? 1 : 0) };
        }
    };

    auto const shrinkColumns = [this](ColumnCount newColumnCount,
                                      LineCount /*newLineCount*/,
                                      cell_location cursor) -> cell_location {
        using line_buffer = typename line<Cell>::inflated_buffer;

        if (!_reflowOnResize)
        {
            _pageSize.columns = newColumnCount;
            crispy::for_each(_lines, [=](line<Cell>& line) {
                if (newColumnCount < line.size())
                    line.resize(newColumnCount);
            });
            verifyState();
            return cursor + std::min(cursor.column, boxed_cast<column_offset>(newColumnCount));
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

            lines<Cell> shrinkedLines;
            line_buffer wrappedColumns;
            line_flags previousFlags = _lines.front().inheritableFlags();

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
                    line_flags::None,
                    trivial_line_buffer { newColumnCount, graphics_attributes {}, graphics_attributes {} });

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

    cell_location cursor = currentCursorPos;

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
void grid<Cell>::clampHistory()
{
    // TODO: needed?
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void grid<Cell>::appendNewLines(LineCount count, graphics_attributes attr)
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
            return terminal::line<Cell>(wrappableFlag, trivial_line_buffer { _pageSize.columns, attr, attr });
        });
        clampHistory();
    }
}
// }}}
// {{{ dumpGrid impl
template <typename Cell>
std::ostream& dumpGrid(std::ostream& os, grid<Cell> const& grid)
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
        terminal::line<Cell> const& lineAttribs = grid.lineAt(line_offset(lineOffset));

        os << fmt::format("[{:>2}] \"{}\" | {}\n",
                          lineOffset,
                          grid.lineText(line_offset::cast_from(lineOffset)),
                          (unsigned) lineAttribs.flags());
    }

    return os;
}

template <typename Cell>
std::string dumpGrid(grid<Cell> const& grid)
{
    std::stringstream sstr;
    dumpGrid(sstr, grid);
    return sstr.str();
}
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
cell_location_range grid<Cell>::wordRangeUnderCursor(cell_location position,
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
                current.column = boxed_cast<column_offset>(pageSize().columns) - 1;
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
                current.column = column_offset(0);
                current = stretchedColumn(cell_location { current.line, current.column + 1 });
            }

            if (*current.column + 1 < *pageSize().columns)
            {
                current = stretchedColumn(cell_location { current.line, current.column + 1 });
            }
            else if (*current.line + 1 < *pageSize().lines)
            {
                current.line++;
                current.column = column_offset(0);
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
bool grid<Cell>::cellEmptyOrContainsOneOf(cell_location position, u32string_view delimiters) const noexcept
{
    // Word selection may be off by one
    position.column = min(position.column, boxed_cast<column_offset>(pageSize().columns - 1));

    auto const& cell = at(position.line, position.column);
    return CellUtil::empty(cell) || delimiters.find(cell.codepoint(0)) != std::u32string_view::npos;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
u32string grid<Cell>::extractText(cell_location_range range) const noexcept
{
    if (range.first.line > range.second.line)
        std::swap(range.first, range.second);

    auto output = u32string {};

    assert(range.first.line == range.second.line);

    auto const rightMargin = boxed_cast<column_offset>(pageSize().columns) - 1;
    auto const numLines = LineCount::cast_from(range.second.line - range.first.line + 1);
    auto ranges = vector<column_range> {};

    switch (numLines.value)
    {
        case 1:
            ranges.emplace_back(
                column_range { range.first.line, range.first.column, min(range.second.column, rightMargin) });
            break;
        case 2:
            ranges.emplace_back(column_range { range.first.line, range.first.column, rightMargin });
            ranges.emplace_back(
                column_range { range.first.line, column_offset(0), min(range.second.column, rightMargin) });
            break;
        default:
            ranges.emplace_back(column_range { range.first.line, range.first.column, rightMargin });
            for (auto j = range.first.line + 1; j < range.second.line; ++j)
                ranges.emplace_back(column_range { j, column_offset(0), rightMargin });
            ranges.emplace_back(
                column_range { range.second.line, column_offset(0), min(range.second.column, rightMargin) });
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
template class terminal::grid<terminal::compact_cell>;
template std::string terminal::dumpGrid<terminal::compact_cell>(
    terminal::grid<terminal::compact_cell> const&);

#include <vtbackend/cell/SimpleCell.h>
template class terminal::grid<terminal::simple_cell>;
template std::string terminal::dumpGrid<terminal::simple_cell>(terminal::grid<terminal::simple_cell> const&);
