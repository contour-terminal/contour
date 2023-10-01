// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Line.h>
#include <vtbackend/cell/CellConcept.h>
#include <vtbackend/primitives.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/defines.h>
#include <crispy/ring.h>

#include <range/v3/algorithm/copy.hpp>
#include <range/v3/iterator/insert_iterators.hpp>
#include <range/v3/view/iota.hpp>

#include <gsl/span>
#include <gsl/span_ext>

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <libunicode/convert.h>

namespace vtbackend
{

// {{{ Margin
struct Margin
{
    struct Horizontal
    {
        ColumnOffset from;
        ColumnOffset
            to; // TODO: call it begin and end and have end point to to+1 to avoid unnecessary +1's later

        [[nodiscard]] constexpr ColumnCount length() const noexcept
        {
            return unbox<ColumnCount>(to - from) + ColumnCount(1);
        }
        [[nodiscard]] constexpr bool contains(ColumnOffset value) const noexcept
        {
            return from <= value && value <= to;
        }
        [[nodiscard]] constexpr bool operator==(Horizontal rhs) const noexcept
        {
            return from == rhs.from && to == rhs.to;
        }
        [[nodiscard]] constexpr bool operator!=(Horizontal rhs) const noexcept { return !(*this == rhs); }

        [[nodiscard]] constexpr ColumnOffset clamp(ColumnOffset value) const noexcept
        {
            return std::clamp(value, from, to);
        }
    };

    struct Vertical
    {
        LineOffset from;
        // TODO: call it begin and end and have end point to to+1 to avoid unnecessary +1's later
        LineOffset to;

        [[nodiscard]] constexpr LineCount length() const noexcept
        {
            return unbox<LineCount>(to - from) + LineCount(1);
        }
        [[nodiscard]] constexpr bool contains(LineOffset value) const noexcept
        {
            return from <= value && value <= to;
        }
        [[nodiscard]] constexpr bool operator==(Vertical const& rhs) const noexcept
        {
            return from == rhs.from && to == rhs.to;
        }
        [[nodiscard]] constexpr bool operator!=(Vertical const& rhs) const noexcept
        {
            return !(*this == rhs);
        }

        [[nodiscard]] constexpr LineOffset clamp(LineOffset value) const noexcept
        {
            return std::clamp(value, from, to);
        }
    };

    Vertical vertical {};     // top-bottom
    Horizontal horizontal {}; // left-right
};

constexpr bool operator==(Margin const& a, PageSize b) noexcept
{
    return a.horizontal.from.value == 0 && a.horizontal.to.value + 1 == b.columns.value
           && a.vertical.from.value == 0 && a.vertical.to.value + 1 == b.lines.value;
}

constexpr bool operator!=(Margin const& a, PageSize b) noexcept
{
    return !(a == b);
}
// }}}

template <typename Cell>
using Lines = crispy::ring<Line<Cell>>;

struct RenderPassHints
{
    bool containsBlinkingCells = false;
};

/**
 * Represents a logical grid line, i.e. a sequence lines that were written without
 * an explicit linefeed, triggering an auto-wrap.
 */
template <typename Cell>
struct LogicalLine
{
    LineOffset top {};
    LineOffset bottom {};
    std::vector<std::reference_wrapper<Line<Cell>>> lines {};

    [[nodiscard]] Line<Cell> joinWithRightTrimmed() const
    {
        // TODO: determine final line's column count and pass it to ctor.
        typename Line<Cell>::Buffer output;
        auto lineFlags = lines.front().get().flags();
        for (Line<Cell> const& line: lines)
            for (Cell const& cell: line.cells())
                output.emplace_back(cell);

        while (!output.empty() && output.back().empty())
            output.pop_back();

        return Line<Cell>(output, lineFlags);
    }

    [[nodiscard]] std::string text() const
    {
        std::string output;
        for (auto const& line: lines)
            output += line.get().toUtf8();
        return output;
    }

    // Searches from left to right, taking into account line wrapping
    [[nodiscard]] std::optional<vtbackend::CellLocation> search(std::u32string_view searchText,
                                                                ColumnOffset startPosition) const
    {
        auto const lineLength = unbox<size_t>(lines.front().get().size());
        auto i = top;
        if (searchText.size() > lineLength)
        {
            for (auto line = lines.begin(); line != lines.end(); ++line)
            {
                std::u32string_view textOnThisLine(searchText.data(),
                                                   lineLength - unbox<size_t>(startPosition));
                // Find how much of searchText is on this line
                auto const result = searchPartialMatch(textOnThisLine, line->get());
                if (result != 0)
                {
                    // Match the remaining text
                    std::u32string_view remainingTextToMatch(searchText.data() + result,
                                                             searchText.size() - result);
                    if (matchTextAt(remainingTextToMatch, ColumnOffset(0), line + 1))
                        return CellLocation { i, ColumnOffset(static_cast<int>(lineLength - result)) };
                }
                startPosition = ColumnOffset(0);
                ++i;
            }
            return std::nullopt;
        }
        for (auto line = lines.begin(); line != lines.end(); ++line)
        {
            auto result = line->get().search(searchText, startPosition);
            if (result.has_value())
            {
                if (result->partialMatchLength == 0)
                    return CellLocation { i, result->column };
                auto remainingText = searchText;
                remainingText.remove_prefix(result->partialMatchLength);
                if (line + 1 != lines.end() && (line + 1)->get().matchTextAt(remainingText, ColumnOffset(0)))
                    return CellLocation { i,
                                          ColumnOffset::cast_from(
                                              static_cast<int>(unbox<size_t>(line->get().size())
                                                               - result->partialMatchLength)) };
            }
            startPosition = ColumnOffset(0);
            ++i;
        }
        return std::nullopt;
    }

    // Searches from right to left, taking into account line wrapping
    [[nodiscard]] std::optional<vtbackend::CellLocation> searchReverse(std::u32string_view searchText,
                                                                       ColumnOffset startPosition) const
    {
        auto i = bottom;
        auto const lineLength = unbox<size_t>(lines.front().get().size());
        if (searchText.size() > lineLength)
        {
            for (auto line = lines.rbegin(); line != lines.rend(); ++line)
            {
                std::u32string_view textOnThisLine(searchText.data() + searchText.size()
                                                       - unbox<size_t>(startPosition),
                                                   unbox<size_t>(startPosition));
                auto const result = searchPartialMatchReverse(textOnThisLine, line->get());
                if (result != 0)
                {
                    std::u32string_view remainingText(searchText.data(), searchText.size() - result);
                    // Check if the searchText can even fit in the available lines
                    auto const willFit = [&] {
                        auto const count = static_cast<size_t>(
                            std::max(long { 0 }, static_cast<long>(std::distance(line + 1, lines.rend()))));
                        auto const total = count * lineLength;
                        return total >= remainingText.size();
                    }();
                    if (!willFit)
                        return std::nullopt;

                    // Column where the remaining text should start at
                    auto const startCol = ColumnOffset::cast_from(
                        (lineLength - (remainingText.size() % lineLength)) % lineLength);

                    // Line where the remaining text should start at
                    long const startLine = static_cast<long>(std::ceil(
                        static_cast<double>(remainingText.size()) / static_cast<double>(lineLength)));

                    if (matchTextAtReverse(remainingText, startCol, line + startLine))
                        return CellLocation { LineOffset::cast_from(i.value - startLine), startCol };
                }
                startPosition = ColumnOffset::cast_from(lineLength - 1);
                --i;
            }
            return std::nullopt;
        }
        auto const lastColumn = ColumnOffset::cast_from(lineLength);
        for (auto line = lines.rbegin(); line != lines.rend(); ++line)
        {
            auto result = line->get().searchReverse(searchText, startPosition);
            if (result.has_value())
            {
                if (result->partialMatchLength == 0)
                    return CellLocation { i, result->column };
                auto remainingText = searchText;
                remainingText.remove_suffix(result->partialMatchLength);
                if (line + 1 != lines.rend()
                    && (line + 1)->get().matchTextAt(remainingText,
                                                     lastColumn - static_cast<int>(remainingText.size())))
                    return CellLocation { i - 1, lastColumn - static_cast<int>(remainingText.size()) };
            }
            startPosition = lastColumn - 1;
            --i;
        }
        return std::nullopt;
    }

  private:
    // Finds the maximum number of charecters of searchText that can be matched from right end of line
    [[nodiscard]] size_t searchPartialMatch(std::u32string_view searchText,
                                            const Line<Cell>& line) const noexcept
    {
        auto const lineLength = unbox<size_t>(line.size());
        while (!searchText.empty())
        {
            if (line.matchTextAt(searchText, ColumnOffset(static_cast<int>(lineLength - searchText.size()))))
                return searchText.size();
            searchText.remove_suffix(1);
        }
        return 0;
    }

    // Finds the maximum number of charecters of searchText that can be matched from left end of line
    [[nodiscard]] size_t searchPartialMatchReverse(std::u32string_view searchText,
                                                   const Line<Cell>& line) const noexcept
    {
        while (!searchText.empty())
        {
            if (line.matchTextAt(searchText, ColumnOffset(0)))
                return searchText.size();
            searchText.remove_prefix(1);
        }
        return 0;
    }

    [[nodiscard]] auto segmentSearchText(std::u32string_view searchText, ColumnOffset startCol) const noexcept
    {
        std::vector<std::u32string_view> segments;
        auto const lineLength = unbox<size_t>(lines.front().get().size());
        if (startCol > ColumnOffset(0))
        {
            segments.emplace_back(searchText.data(), lineLength - unbox<size_t>(startCol));
            searchText.remove_prefix(lineLength - unbox<size_t>(startCol));
        }
        while (!searchText.empty())
        {
            if (searchText.size() < lineLength)
            {
                segments.emplace_back(searchText);
                break;
            }
            segments.emplace_back(searchText.data(), lineLength);
            searchText.remove_prefix(lineLength);
        }
        return segments;
    }

    // Match searchText right to left starting at startCol in line startLine
    template <typename Itr>
    [[nodiscard]] bool matchTextAt(std::u32string_view searchText,
                                   ColumnOffset startCol,
                                   Itr startLine) const noexcept
    {
        auto segments = segmentSearchText(searchText, startCol);
        for (auto segment: segments)
        {
            if (!startLine->get().matchTextAt(segment, startCol))
                return false;
            ++startLine;
        }
        return true;
    }

    // Match searchText right to left starting at startCol in line startLine
    template <typename Itr>
    [[nodiscard]] bool matchTextAtReverse(std::u32string_view searchText,
                                          ColumnOffset startCol,
                                          Itr startLine) const noexcept
    {
        auto segments = segmentSearchText(searchText, startCol);
        for (auto i: segments)
        {
            if (!startLine->get().matchTextAt(i, startCol))
                return false;
            startCol = ColumnOffset::cast_from(0);
            --startLine;
        }
        return true;
    }
};

template <typename Cell>
bool operator==(LogicalLine<Cell> const& a, LogicalLine<Cell> const& b) noexcept
{
    return a.top == b.top && a.bottom == b.bottom;
}

template <typename Cell>
bool operator!=(LogicalLine<Cell> const& a, LogicalLine<Cell> const& b) noexcept
{
    return !(a == b);
}

template <typename Cell>
struct LogicalLines
{
    LineOffset topMostLine;
    LineOffset bottomMostLine;
    std::reference_wrapper<Lines<Cell>> lines;

    // NOLINTNEXTLINE(readability-identifier-naming)
    struct iterator // {{{
    {
        std::reference_wrapper<Lines<Cell>> lines;
        LineOffset top;
        LineOffset next; // index to next logical line's beginning
        LineOffset bottom;
        LogicalLine<Cell> current;

        iterator(std::reference_wrapper<Lines<Cell>> lines,
                 LineOffset top,
                 LineOffset next,
                 LineOffset bottom):
            lines { lines }, top { top }, next { next }, bottom { bottom }
        {
            Require(top <= next);
            Require(next <= bottom + 1);
            ++*this;
        }

        LogicalLine<Cell> const& operator*() const noexcept { return current; }
        LogicalLine<Cell> const* operator->() const noexcept { return &current; }

        iterator& operator++()
        {
            if (next == bottom + 1)
            {
                current.top = next;
                current.bottom = next;
                return *this;
            }

            // Require(!lines.get()[unbox<int>(next)].wrapped());

            current.top = LineOffset::cast_from(next);
            current.lines.clear();
            do
                current.lines.emplace_back(lines.get()[unbox(next++)]);
            while (next <= bottom && lines.get()[unbox(next)].wrapped());

            current.bottom = LineOffset::cast_from(next - 1);

            return *this;
        }

        iterator& operator--()
        {
            if (next == top - 1)
            {
                current.top = top - 1;
                current.bottom = top - 1;
                return *this;
            }

            auto const bottomMost = next - 1;
            do
                --next;
            while (lines.get()[unbox(next)].wrapped());
            auto const topMost = next;

            current.top = topMost;
            current.bottom = bottomMost;

            current.lines.clear();
            for (auto i = topMost; i <= bottomMost; ++i)
                current.lines.emplace_back(lines.get()[unbox(i)]);

            return *this;
        }

        iterator& operator++(int)
        {
            auto c = *this;
            ++*this;
            return c;
        }
        iterator& operator--(int)
        {
            auto c = *this;
            --*this;
            return c;
        }

        bool operator==(iterator const& other) const noexcept { return current == other.current; }
        bool operator!=(iterator const& other) const noexcept { return current != other.current; }
    }; // }}}

    [[nodiscard]] iterator begin() const { return iterator(lines, topMostLine, topMostLine, bottomMostLine); }
    [[nodiscard]] iterator end() const
    {
        return iterator(lines, topMostLine, bottomMostLine + 1, bottomMostLine);
    }
};

template <typename Cell>
struct ReverseLogicalLines
{
    LineOffset topMostLine;
    LineOffset bottomMostLine;
    std::reference_wrapper<Lines<Cell>> lines;

    // NOLINTNEXTLINE(readability-identifier-naming)
    struct iterator // {{{
    {
        std::reference_wrapper<Lines<Cell>> lines;
        LineOffset top;
        LineOffset next; // index to next logical line's beginning
        LineOffset bottom;
        LogicalLine<Cell> current;

        iterator(std::reference_wrapper<Lines<Cell>> lines,
                 LineOffset top,
                 LineOffset next,
                 LineOffset bottom):
            lines { lines }, top { top }, next { next }, bottom { bottom }
        {
            Require(top - 1 <= next);
            Require(next <= bottom);
            ++*this;
        }

        LogicalLine<Cell> const& operator*() const noexcept { return current; }

        iterator& operator--()
        {
            if (next == bottom + 1)
            {
                current.top = bottom + 1;
                current.bottom = bottom + 1;
                return *this;
            }

            Require(!lines.get()[unbox(next)].wrapped());

            current.top = LineOffset::cast_from(next);
            current.lines.clear();
            do
                current.lines.emplace_back(lines.get()[unbox(next++)]);
            while (next <= bottom && lines.get()[unbox(next)].wrapped());

            current.bottom = LineOffset::cast_from(next - 1);

            return *this;
        }

        iterator& operator++()
        {
            if (next == top - 1)
            {
                current.top = next;
                current.bottom = next;
                return *this;
            }

            auto const bottomMost = next;
            while (lines.get()[unbox(next)].wrapped())
                --next;
            auto const topMost = next;
            --next; // jump to next logical line's bottom line above the current logical one

            current.top = topMost;
            current.bottom = bottomMost;

            current.lines.clear();
            for (auto i = topMost; i <= bottomMost; ++i)
                current.lines.emplace_back(lines.get()[unbox(i)]);

            return *this;
        }

        iterator& operator++(int)
        {
            auto c = *this;
            ++*this;
            return c;
        }
        iterator& operator--(int)
        {
            auto c = *this;
            --*this;
            return c;
        }

        bool operator==(iterator const& other) const noexcept { return current == other.current; }
        bool operator!=(iterator const& other) const noexcept { return current != other.current; }
    }; // }}}

    [[nodiscard]] iterator begin() const
    {
        return iterator(lines, topMostLine, bottomMostLine, bottomMostLine);
    }
    [[nodiscard]] iterator end() const
    {
        return iterator(lines, topMostLine, topMostLine - 1, bottomMostLine);
    }
};

/**
 * Manages the screen grid buffer (main screen + scrollback history).
 *
 * <h3>Future motivations</h3>
 *
 * <ul>
 *   <li>manages text reflow upon resize
 *   <li>manages underlying disk storage for very old scrollback history lines.
 * </ul>
 *
 * <h3>Layout</h3>
 *
 * <pre>
 *      +0========================-3+   <-- scrollback top
 *      |1                        -2|
 *      |2   Scrollback history   -1|
 *      |3                         0|   <-- scrollback bottom
 *      +4-------------------------1+   <-- main page top
 *      |5                         2|
 *      |6   main page area        3|
 *      |7                         4|   <-- main page bottom
 *      +---------------------------+
 *       ^                          ^
 *       1                          pageSize.columns
 * </pre>
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
class Grid
{
    // TODO: Rename all "History" to "Scrollback"?
  public:
    Grid(PageSize pageSize, bool reflowOnResize, MaxHistoryLineCount maxHistoryLineCount);

    Grid(): Grid(PageSize { LineCount(25), ColumnCount(80) }, false, LineCount(0)) {}

    void reset();

    // {{{ grid global properties
    [[nodiscard]] LineCount maxHistoryLineCount() const noexcept
    {
        if (auto const* maxLineCount = std::get_if<LineCount>(&_historyLimit))
            return *maxLineCount;
        else
            return LineCount::cast_from(_lines.size()) - _pageSize.lines;
    }

    void setMaxHistoryLineCount(MaxHistoryLineCount maxHistoryLineCount);

    [[nodiscard]] LineCount totalLineCount() const noexcept
    {
        return maxHistoryLineCount() + _pageSize.lines;
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept { return _linesUsed - _pageSize.lines; }

    [[nodiscard]] bool reflowOnResize() const noexcept { return _reflowOnResize; }
    void setReflowOnResize(bool enabled) { _reflowOnResize = enabled; }

    [[nodiscard]] PageSize pageSize() const noexcept { return _pageSize; }
    [[nodiscard]] Margin margin() const noexcept { return _margin; }
    [[nodiscard]] Margin& margin() noexcept { return _margin; }

    /// Resizes the main page area of the grid and adapts the scrollback area's width accordingly.
    ///
    /// @param pageSize          new size of the main page area
    /// @param currentCursorPos  current cursor position
    /// @param wrapPending       AutoWrap is on and a wrap is pending
    ///
    /// @returns updated cursor position.
    [[nodiscard]] CellLocation resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending);
    // }}}

    // {{{ Line API
    /// @returns reference to Line at given relative offset @p line.
    [[nodiscard]] Line<Cell>& lineAt(LineOffset line) noexcept;
    [[nodiscard]] Line<Cell> const& lineAt(LineOffset line) const noexcept;

    [[nodiscard]] gsl::span<Cell const> lineBuffer(LineOffset line) const noexcept
    {
        return lineAt(line).cells();
    }
    [[nodiscard]] gsl::span<Cell const> lineBufferRightTrimmed(LineOffset line) const noexcept;

    [[nodiscard]] std::string lineText(LineOffset line) const;
    [[nodiscard]] std::string lineTextTrimmed(LineOffset line) const;
    [[nodiscard]] std::string lineText(Line<Cell> const& line) const;

    void setLineText(LineOffset line, std::string_view text);

    // void resetLine(LineOffset line, GraphicsAttributes attribs) noexcept
    // { lineAt(line).reset(attribs); }

    [[nodiscard]] ColumnCount lineLength(LineOffset line) const noexcept { return lineAt(line).size(); }
    [[nodiscard]] bool isLineBlank(LineOffset line) const noexcept;
    [[nodiscard]] bool isLineWrapped(LineOffset line) const noexcept;

    [[nodiscard]] int computeLogicalLineNumberFromBottom(LineCount n) const noexcept;

    [[nodiscard]] size_t zero_index() const noexcept { return _lines.zero_index(); }
    // }}}

    /// Gets a reference to the cell relative to screen origin (top left, 0:0).
    [[nodiscard]] Cell& useCellAt(LineOffset line, ColumnOffset column) noexcept;
    [[nodiscard]] Cell& at(LineOffset line, ColumnOffset column) noexcept;
    [[nodiscard]] Cell const& at(LineOffset line, ColumnOffset column) const noexcept;

    // page view API
    [[nodiscard]] gsl::span<Line<Cell>> pageAtScrollOffset(ScrollOffset scrollOffset);
    [[nodiscard]] gsl::span<Line<Cell> const> pageAtScrollOffset(ScrollOffset scrollOffset) const;
    [[nodiscard]] gsl::span<Line<Cell>> mainPage();
    [[nodiscard]] gsl::span<Line<Cell> const> mainPage() const;

    [[nodiscard]] LogicalLines<Cell> logicalLines()
    {
        return LogicalLines<Cell> { boxed_cast<LineOffset>(-historyLineCount()),
                                    boxed_cast<LineOffset>(_pageSize.lines - 1),
                                    _lines };
    }

    [[nodiscard]] LogicalLines<Cell> logicalLinesFrom(LineOffset offset)
    {
        return LogicalLines<Cell> { offset, boxed_cast<LineOffset>(_pageSize.lines - 1), _lines };
    }

    [[nodiscard]] ReverseLogicalLines<Cell> logicalLinesReverse()
    {
        return ReverseLogicalLines<Cell> { boxed_cast<LineOffset>(-historyLineCount()),
                                           boxed_cast<LineOffset>(_pageSize.lines - 1),
                                           _lines };
    }

    [[nodiscard]] ReverseLogicalLines<Cell> logicalLinesReverseFrom(LineOffset offset)
    {
        return ReverseLogicalLines<Cell> { boxed_cast<LineOffset>(-historyLineCount()), offset, _lines };
    }

    // {{{ buffer manipulation

    /// Completely deletes all scrollback lines.
    void clearHistory();

    /// Scrolls up by @p n lines within the given margin.
    ///
    /// @param n number of lines to scroll up within the given margin.
    /// @param defaultAttributes SGR attributes the newly created grid cells will be initialized with.
    /// @param margin the margin coordinates to perform the scrolling action into.
    ///
    /// @return Number of lines the main page has been scrolled.
    LineCount scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept;

    /// Scrolls up main page by @p n lines and re-initializes grid cells with @p defaultAttributes.
    LineCount scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes = {}) noexcept;

    /// Scrolls down by @p n lines within the given margin.
    ///
    /// @param n number of lines to scroll down within the given margin.
    /// @param defaultAttributes SGR attributes the newly created grid cells will be initialized with.
    /// @param margin the margin coordinates to perform the scrolling action into.
    void scrollDown(LineCount n, GraphicsAttributes const& defaultAttributes, Margin const& margin);

    // Scrolls the data within the margins to the left filling the new space on the right with empty cells.
    void scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept;
    // }}}

    // {{{ Rendering API
    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    [[nodiscard]] RenderPassHints render(
        RendererT&& render,
        ScrollOffset scrollOffset = {},
        HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes) const;

    /// Takes text-screenshot of the main page.
    [[nodiscard]] std::string renderMainPageText() const;

    /// Renders the full grid's text characters.
    ///
    /// Empty cells are represented as strings and lines split by LF.
    [[nodiscard]] std::string renderAllText() const;
    // }}}

    [[nodiscard]] constexpr LineFlags defaultLineFlags() const noexcept;

    [[nodiscard]] constexpr LineCount linesUsed() const noexcept;

    void verifyState() const noexcept;

    // Retrieves the cell location range of the underlying word at the given cursor position.
    [[nodiscard]] CellLocationRange wordRangeUnderCursor(CellLocation position,
                                                         std::u32string_view delimiters) const noexcept;

    [[nodiscard]] bool cellEmptyOrContainsOneOf(CellLocation position,
                                                std::u32string_view delimiters) const noexcept;

    // Lineary extracts the text of a given grid cell range.
    [[nodiscard]] std::u32string extractText(CellLocationRange range) const noexcept;

    // Conditionally extends the cell location forward if the grid cell at the given location holds a wide
    // character.
    [[nodiscard]] CellLocation stretchedColumn(CellLocation coord) const noexcept
    {
        CellLocation stretched = coord;
        if (auto const w = cellWidthAt(coord); w > 1) // wide character
        {
            stretched.column += ColumnOffset::cast_from(w) - 1;
            return stretched;
        }

        return stretched;
    }

    [[nodiscard]] CellLocation rightMostNonEmptyAt(LineOffset lineOffset) const noexcept
    {
        auto const& line = lineAt(lineOffset);

        if (line.isTrivialBuffer())
        {
            if (line.empty())
                return CellLocation { lineOffset, ColumnOffset(0) };

            auto const& trivial = line.trivialBuffer();
            auto const columnOffset = ColumnOffset::cast_from(trivial.usedColumns - 1);
            return CellLocation { lineOffset, columnOffset };
        }

        auto const& inflatedLine = line.cells();
        auto columnOffset = ColumnOffset::cast_from(_pageSize.columns - 1);
        while (columnOffset > ColumnOffset(0) && inflatedLine[unbox<size_t>(columnOffset)].empty())
            --columnOffset;
        return CellLocation { lineOffset, columnOffset };
    }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept
    {
        return lineAt(position.line).cellWidthAt(position.column);
    }

  private:
    CellLocation growLines(LineCount newHeight, CellLocation cursor);
    void appendNewLines(LineCount count, GraphicsAttributes attr);
    void clampHistory();

    // {{{ buffer helpers
    void resizeBuffers(PageSize newSize)
    {
        auto const newTotalLineCount = historyLineCount() + newSize.lines;
        _lines.resize(unbox<size_t>(newTotalLineCount));
        _pageSize = newSize;
    }

    void rezeroBuffers() noexcept { _lines.rezero(); }

    void rotateBuffers(int offset) noexcept { _lines.rotate(offset); }

    void rotateBuffersLeft(LineCount count) noexcept { _lines.rotate_left(unbox<size_t>(count)); }

    void rotateBuffersRight(LineCount count) noexcept { _lines.rotate_right(unbox<size_t>(count)); }
    // }}}

    // private fields
    //
    PageSize _pageSize;
    Margin _margin;
    bool _reflowOnResize = false;
    MaxHistoryLineCount _historyLimit;

    // Number of lines is at least the sum of _maxHistoryLineCount + _pageSize.lines,
    // because shrinking the page height does not necessarily
    // have to resize the array (as optimization).
    Lines<Cell> _lines;

    // Number of lines used in the Lines buffer.
    LineCount _linesUsed;
};

template <typename Cell>
std::ostream& dumpGrid(std::ostream& os, Grid<Cell> const& grid);

template <typename Cell>
std::string dumpGrid(Grid<Cell> const& grid);

// {{{ impl
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
constexpr LineFlags Grid<Cell>::defaultLineFlags() const noexcept
{
    return _reflowOnResize ? LineFlags::Wrappable : LineFlags::None;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
constexpr LineCount Grid<Cell>::linesUsed() const noexcept
{
    return _linesUsed;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Grid<Cell>::isLineWrapped(LineOffset line) const noexcept
{
    return line >= -boxed_cast<LineOffset>(historyLineCount())
           && boxed_cast<LineCount>(line) < _pageSize.lines && lineAt(line).wrapped();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
template <typename RendererT>
[[nodiscard]] RenderPassHints Grid<Cell>::render(RendererT&& render,
                                                 ScrollOffset scrollOffset,
                                                 HighlightSearchMatches highlightSearchMatches) const
{
    assert(!scrollOffset || unbox<LineCount>(scrollOffset) <= historyLineCount());

    auto y = LineOffset(0);
    auto hints = RenderPassHints {};
    for (int i = -*scrollOffset, e = i + *_pageSize.lines; i != e; ++i, ++y)
    {
        auto x = ColumnOffset(0);
        Line<Cell> const& line = _lines[i];
        // NB: trivial liner rendering only works trivially if we don't do cell-based operations
        // on the text. Therefore, we only move to the trivial fast path here if we don't want to
        // highlight search matches.
        if (line.isTrivialBuffer() && highlightSearchMatches == HighlightSearchMatches::No)
        {
            auto const cellFlags = line.trivialBuffer().textAttributes.flags;
            hints.containsBlinkingCells = hints.containsBlinkingCells || (CellFlags::Blinking & cellFlags)
                                          || (CellFlags::RapidBlinking & cellFlags);
            render.renderTrivialLine(line.trivialBuffer(), y);
        }
        else
        {
            render.startLine(y);
            for (Cell const& cell: line.cells())
            {
                hints.containsBlinkingCells = hints.containsBlinkingCells
                                              || (CellFlags::Blinking & cell.flags())
                                              || (CellFlags::RapidBlinking & cell.flags());
                render.renderCell(cell, y, x++);
            }
            render.endLine();
        }
    }
    render.finish();
    return hints;
}
// }}}

} // namespace vtbackend

// {{{ fmt formatter
template <>
struct fmt::formatter<vtbackend::Margin::Horizontal>: fmt::formatter<std::string>
{
    auto format(const vtbackend::Margin::Horizontal range, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}..{}", range.from, range.to), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::Margin::Vertical>: fmt::formatter<std::string>
{
    auto format(const vtbackend::Margin::Vertical range, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}..{}", range.from, range.to), ctx);
    }
};

// }}}
