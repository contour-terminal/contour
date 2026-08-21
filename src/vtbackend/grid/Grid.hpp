// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellProxy.hpp>
#include <vtbackend/grid/Line.hpp>

#include <crispy/Assert.hpp>
#include <crispy/Defines.hpp>
#include <crispy/Ring.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace vtbackend
{

/// The one statement of the rule, because two consumers must agree on it to the row: Grid::render()
/// places the list it draws by it, and Viewport translates coordinates through it. Negative while
/// smooth scrolling supplies rows above the page, and zero once the list is no longer than the page.
///
/// Rows BEYOND the page count are the smooth-scrolling ones and belong above it, so the last row of the
/// list still lands on the bottom of the page however many extra ones precede it. FEWER rows than the
/// page means the walk ran out of grid at the top and there is nothing above them, so they start at the
/// top and the page is short at the BOTTOM -- exactly as a terminal that has not filled its page
/// already looks.
///
/// @param pageLines How many lines the page holds.
/// @param rowCount How many rows the list holds.
/// @return The screen row of the list's first row; zero unless the list is longer than the page.
[[nodiscard]] constexpr LineOffset foldedRowsTopRow(LineCount pageLines, size_t rowCount) noexcept
{
    return LineOffset::cast_from(std::min(0, unbox<int>(pageLines) - static_cast<int>(rowCount)));
}

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
            // unsigned arithmetic avoids signed-overflow UB in the +1 when
            // the difference is INT_MAX (the practical range makes this
            // impossible, but the compiler may still exploit the UB).
            return ColumnCount::cast_from(static_cast<unsigned>(unbox<int>(to - from)) + 1u);
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
            return LineCount::cast_from(static_cast<unsigned>(unbox<int>(to - from)) + 1u);
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
    // Avoid signed-overflow UB from `to.value + 1` when to.value is INT_MAX:
    // rewrite as `to == other - 1` (page sizes are always >= 1).
    return a.horizontal.from.value == 0 && a.horizontal.to.value == b.columns.value - 1
           && a.vertical.from.value == 0 && a.vertical.to.value == b.lines.value - 1;
}

constexpr bool operator!=(Margin const& a, PageSize b) noexcept
{
    return !(a == b);
}
// }}}

using Lines = crispy::Ring<Line>;

struct RenderPassHints
{
    bool containsBlinkingCells = false;
};

/**
 * Represents a logical grid line, i.e. a sequence lines that were written without
 * an explicit linefeed, triggering an auto-wrap.
 */
struct LogicalLine
{
    LineOffset top {};
    LineOffset bottom {};
    std::vector<std::reference_wrapper<Line>> lines {};

    [[nodiscard]] std::string text() const
    {
        std::string output;
        for (auto const& line: lines)
            output += line.get().toUtf8();
        return output;
    }

    // Searches from left to right, taking into account line wrapping
    [[nodiscard]] std::optional<vtbackend::CellLocation> search(std::u32string_view searchText,
                                                                ColumnOffset startPosition,
                                                                bool isCaseSensitive) const
    {
        auto const lineLength = unbox<size_t>(lines.front().get().size());
        auto i = top;
        if (searchText.size() > lineLength)
        {
            for (auto line = lines.begin(); line != lines.end(); ++line)
            {
                auto const textOnThisLine = searchText.substr(0, lineLength - unbox<size_t>(startPosition));
                // Find how much of searchText is on this line
                auto const result = searchPartialMatch(textOnThisLine, line->get(), isCaseSensitive);
                if (result != 0)
                {
                    // Match the remaining text
                    std::u32string_view const remainingTextToMatch(searchText.data() + result,
                                                                   searchText.size() - result);
                    if (matchTextAt(remainingTextToMatch, ColumnOffset(0), line + 1, isCaseSensitive))
                        return CellLocation { .line = i,
                                              .column = ColumnOffset(static_cast<int>(lineLength - result)) };
                }
                startPosition = ColumnOffset(0);
                ++i;
            }
            return std::nullopt;
        }
        for (auto line = lines.begin(); line != lines.end(); ++line)
        {
            auto result = line->get().search(searchText, startPosition, isCaseSensitive);
            if (result.has_value())
            {
                if (result->partialMatchLength == 0)
                    return CellLocation { .line = i, .column = result->column };
                auto remainingText = searchText;
                remainingText.remove_prefix(result->partialMatchLength);
                if (line + 1 != lines.end()
                    && (line + 1)->get().matchTextAtWithSensitivityMode(
                        remainingText, ColumnOffset(0), isCaseSensitive))
                    return CellLocation { .line = i,
                                          .column = ColumnOffset::cast_from(
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
                                                                       ColumnOffset startPosition,
                                                                       bool isCaseSensitive) const
    {
        auto i = bottom;
        auto const lineLength = unbox<size_t>(lines.front().get().size());
        if (searchText.size() > lineLength)
        {
            for (auto line = lines.rbegin(); line != lines.rend(); ++line)
            {
                std::u32string_view const textOnThisLine(searchText.data() + searchText.size()
                                                             - unbox<size_t>(startPosition),
                                                         unbox<size_t>(startPosition));
                auto const result = searchPartialMatchReverse(textOnThisLine, line->get(), isCaseSensitive);
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

                    if (matchTextAtReverse(remainingText, startCol, line + startLine, isCaseSensitive))
                        return CellLocation { .line = LineOffset::cast_from(i.value - startLine),
                                              .column = startCol };
                }
                startPosition = ColumnOffset::cast_from(lineLength - 1);
                --i;
            }
            return std::nullopt;
        }
        auto const lastColumn = ColumnOffset::cast_from(lineLength);
        for (auto line = lines.rbegin(); line != lines.rend(); ++line)
        {
            auto result = line->get().searchReverse(searchText, startPosition, isCaseSensitive);
            if (result.has_value())
            {
                if (result->partialMatchLength == 0)
                    return CellLocation { .line = i, .column = result->column };
                auto remainingText = searchText;
                remainingText.remove_suffix(result->partialMatchLength);
                if (line + 1 != lines.rend()
                    && (line + 1)->get().matchTextAtWithSensitivityMode(
                        remainingText, lastColumn - static_cast<int>(remainingText.size()), isCaseSensitive))
                    return CellLocation { .line = i - 1,
                                          .column = lastColumn - static_cast<int>(remainingText.size()) };
            }
            startPosition = lastColumn - 1;
            --i;
        }
        return std::nullopt;
    }

  private:
    // Finds the maximum number of characters of searchText that can be matched from right end of line
    [[nodiscard]] size_t searchPartialMatch(std::u32string_view searchText,
                                            Line const& line,
                                            bool isCaseSensitive) const noexcept
    {
        auto const lineLength = unbox<size_t>(line.size());
        while (!searchText.empty())
        {
            if (line.matchTextAtWithSensitivityMode(
                    searchText,
                    ColumnOffset(static_cast<int>(lineLength - searchText.size())),
                    isCaseSensitive))
                return searchText.size();
            searchText.remove_suffix(1);
        }
        return 0;
    }

    // Finds the maximum number of characters of searchText that can be matched from left end of line
    [[nodiscard]] size_t searchPartialMatchReverse(std::u32string_view searchText,
                                                   Line const& line,
                                                   bool isCaseSensitive) const noexcept
    {
        while (!searchText.empty())
        {
            if (line.matchTextAtWithSensitivityMode(searchText, ColumnOffset(0), isCaseSensitive))
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
                                   Itr startLine,
                                   bool isCaseSensitive) const noexcept
    {
        auto segments = segmentSearchText(searchText, startCol);
        for (auto segment: segments)
        {
            if (!startLine->get().matchTextAtWithSensitivityMode(segment, startCol, isCaseSensitive))
                return false;
            ++startLine;
        }
        return true;
    }

    // Match searchText right to left starting at startCol in line startLine
    template <typename Itr>
    [[nodiscard]] bool matchTextAtReverse(std::u32string_view searchText,
                                          ColumnOffset startCol,
                                          Itr startLine,
                                          bool isCaseSensitive) const noexcept
    {
        auto segments = segmentSearchText(searchText, startCol);
        for (auto i: segments)
        {
            if (!startLine->get().matchTextAtWithSensitivityMode(i, startCol, isCaseSensitive))
                return false;
            startCol = ColumnOffset::cast_from(0);
            --startLine;
        }
        return true;
    }
};

inline bool operator==(LogicalLine const& a, LogicalLine const& b) noexcept
{
    return a.top == b.top && a.bottom == b.bottom;
}

inline bool operator!=(LogicalLine const& a, LogicalLine const& b) noexcept
{
    return !(a == b);
}

struct LogicalLines
{
    LineOffset topMostLine;
    LineOffset bottomMostLine;
    std::reference_wrapper<Lines> lines;

    // NOLINTNEXTLINE(readability-identifier-naming)
    struct iterator // {{{
    {
        std::reference_wrapper<Lines> lines;
        LineOffset top;
        LineOffset next; // index to next logical line's beginning
        LineOffset bottom;
        LogicalLine current;

        iterator(std::reference_wrapper<Lines> lines, LineOffset top, LineOffset next, LineOffset bottom):
            lines { lines }, top { top }, next { next }, bottom { bottom }
        {
            Require(top <= next);
            Require(next <= bottom + 1);
            ++*this;
        }

        LogicalLine const& operator*() const noexcept { return current; }
        LogicalLine const* operator->() const noexcept { return &current; }

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

        iterator operator++(int)
        {
            auto c = *this;
            ++*this;
            return c;
        }
        iterator operator--(int)
        {
            auto c = *this;
            --*this;
            return c;
        }

        bool operator==(iterator const& other) const noexcept { return current == other.current; }
        bool operator!=(iterator const& other) const noexcept { return current != other.current; }
    }; // }}}

    [[nodiscard]] iterator begin() const { return { lines, topMostLine, topMostLine, bottomMostLine }; }
    [[nodiscard]] iterator end() const { return { lines, topMostLine, bottomMostLine + 1, bottomMostLine }; }
};

struct ReverseLogicalLines
{
    LineOffset topMostLine;
    LineOffset bottomMostLine;
    std::reference_wrapper<Lines> lines;

    // NOLINTNEXTLINE(readability-identifier-naming)
    struct iterator // {{{
    {
        std::reference_wrapper<Lines> lines;
        LineOffset top;
        LineOffset next; // index to next logical line's beginning
        LineOffset bottom;
        LogicalLine current;

        iterator(std::reference_wrapper<Lines> lines, LineOffset top, LineOffset next, LineOffset bottom):
            lines { lines }, top { top }, next { next }, bottom { bottom }
        {
            Require(top - 1 <= next);
            Require(next <= bottom);
            ++*this;
        }

        LogicalLine const& operator*() const noexcept { return current; }

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

        iterator operator++(int)
        {
            auto c = *this;
            ++*this;
            return c;
        }
        iterator operator--(int)
        {
            auto c = *this;
            --*this;
            return c;
        }

        bool operator==(iterator const& other) const noexcept { return current == other.current; }
        bool operator!=(iterator const& other) const noexcept { return current != other.current; }
    }; // }}}

    [[nodiscard]] iterator begin() const { return { lines, topMostLine, bottomMostLine, bottomMostLine }; }
    [[nodiscard]] iterator end() const { return { lines, topMostLine, topMostLine - 1, bottomMostLine }; }
};

/// A consumer's position in a grid's change stream (one per followed grid per client).
struct GridDeltaCursor
{
    uint64_t generation = 0; ///< The generation this cursor is valid within.
    uint64_t seqno = 0;      ///< Every batch up to and including this one was seen.
    int64_t stableBase = 0;  ///< The base at the last query; bounds the history scan depth.
};

/// What a delta query yielded.
enum class GridDeltaResult : uint8_t
{
    Delta,          ///< Changed lines were reported and the cursor advanced.
    ResyncRequired, ///< Row identity was rebuilt: snapshot via forEachValidLine().
};

/// Whether a captured row carries the rendition its cells wear (tmux `capture-pane -e`).
enum class CaptureRendition : uint8_t
{
    PlainText = 0, ///< Text only; colours and style flags are dropped.
    WithSgr = 1,   ///< SGR sequences interleaved wherever the rendition changes.
};

/// What a captured row does with the default cells that pad it out to the page width
/// (tmux `capture-pane -J`/`-N`, either of which asks for @c Keep).
enum class CaptureTrailingSpaces : uint8_t
{
    /// Drop them, which is what tmux does by default (`grid_line_length`). @see Line::trimmedColumns.
    Trim = 0,
    /// Keep every column, so each row is exactly the page width.
    Keep = 1,
};

/**
 * Manages the screen grid buffer (main screen + scrollback history).
 *
 * Non-templated version using LineSoA-backed Lines.
 */
class Grid
{
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

    /// Resizes the main page area of the grid and adapts the scrollback area's width accordingly.
    [[nodiscard]] CellLocation resize(PageSize newSize, CellLocation currentCursorPos, bool wrapPending);
    // }}}

    // {{{ Line API
    [[nodiscard]] Line& lineAt(LineOffset line) noexcept;
    [[nodiscard]] Line const& lineAt(LineOffset line) const noexcept;

    /// The row at @p line, for a caller that is about to CHANGE it below the page top.
    ///
    /// Records how deep the change reached, so the batch stamping and the delta scan cover the row
    /// even though nothing scrolled it into range (@see _dirtyHistoryFloor). Deliberately a named
    /// operation rather than a side effect of picking the non-const `lineAt` overload: which
    /// overload a caller lands on follows the constness of the *enclosing function*, so read-only
    /// walks of the scrollback that happen to sit in a non-const method (Screen::captureBuffer,
    /// the marker scans) would arm a full-history rescan they have no business arming.
    ///
    /// Only needed where the row may lie ABOVE the page — a page row is always scanned, and a row
    /// that scrolls out is covered by the base delta. In practice that means the semantic marks,
    /// which are stamped on a logical line's head and so follow wrapped rows into the history.
    /// @param line The row about to be changed (negative addresses history).
    /// @return The row.
    [[nodiscard]] Line& changingLineAt(LineOffset line) noexcept
    {
        noteMutableRow(line);
        return lineAt(line);
    }

    [[nodiscard]] std::string lineText(LineOffset line) const;
    [[nodiscard]] std::string lineTextTrimmed(LineOffset line) const;
    [[nodiscard]] std::string lineText(Line const& line) const;

    void setLineText(LineOffset line, std::string_view text);

    [[nodiscard]] ColumnCount lineLength(LineOffset line) const noexcept { return lineAt(line).size(); }
    [[nodiscard]] bool isLineBlank(LineOffset line) const noexcept;
    [[nodiscard]] bool isLineWrapped(LineOffset line) const noexcept;

    /// The first physical line of the logical line @p line belongs to.
    ///
    /// A logical line is what was actually written; the wrapped lines below its head are only where the
    /// window happened to be too narrow. Semantic marks (HeadOnlyLineFlags) name the logical line, so
    /// they are stamped here and read from here.
    ///
    /// @param line Any physical line of the logical line.
    /// @return Its head, or @p line itself when it is not a continuation. Stops at the top of the
    ///         history, so a logical line whose head has already scrolled away reports its oldest
    ///         surviving piece as the head.
    [[nodiscard]] LineOffset logicalLineHead(LineOffset line) const noexcept;

    /// Where @p position sits within its logical line, counted in columns from that line's head.
    ///
    /// @param position A position in the grid.
    /// @return The number of columns of the logical line that precede @p position.
    [[nodiscard]] ColumnOffset logicalColumnOf(CellLocation position) const noexcept;

    [[nodiscard]] int computeLogicalLineNumberFromBottom(LineCount n) const noexcept;

    [[nodiscard]] size_t zeroIndex() const noexcept { return _lines.zeroIndex(); }
    // }}}

    /// Gets a CellProxy to the cell relative to screen origin (top left, 0:0).
    [[nodiscard]] CellProxy useCellAt(LineOffset line, ColumnOffset column) noexcept;
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) noexcept;
    [[nodiscard]] CellProxy at(LineOffset line, ColumnOffset column) const noexcept;

    // Page view API
    //
    // There is deliberately NO accessor handing out a contiguous span over the page. `_lines` is a
    // ring whose rotation moves an index, not the data, so once enough lines have scrolled into
    // history the logical page straddles the ring's physical end. A `span{&_lines[0], pageSize}`
    // therefore walks straight off the underlying vector -- a real out-of-bounds write that ASan
    // caught in DECALN. Iterate with `lineAt(LineOffset)`, which goes through the ring's indexing.

    [[nodiscard]] LogicalLines logicalLines()
    {
        return LogicalLines { .topMostLine = boxed_cast<LineOffset>(-historyLineCount()),
                              .bottomMostLine = boxed_cast<LineOffset>(_pageSize.lines - 1),
                              .lines = _lines };
    }

    [[nodiscard]] LogicalLines logicalLinesFrom(LineOffset offset)
    {
        return LogicalLines { .topMostLine = offset,
                              .bottomMostLine = boxed_cast<LineOffset>(_pageSize.lines - 1),
                              .lines = _lines };
    }

    [[nodiscard]] ReverseLogicalLines logicalLinesReverse()
    {
        return ReverseLogicalLines { .topMostLine = boxed_cast<LineOffset>(-historyLineCount()),
                                     .bottomMostLine = boxed_cast<LineOffset>(_pageSize.lines - 1),
                                     .lines = _lines };
    }

    [[nodiscard]] ReverseLogicalLines logicalLinesReverseFrom(LineOffset offset)
    {
        return ReverseLogicalLines { .topMostLine = boxed_cast<LineOffset>(-historyLineCount()),
                                     .bottomMostLine = offset,
                                     .lines = _lines };
    }

    // {{{ buffer manipulation

    /// Completely deletes all scrollback lines.
    void clearHistory();

    LineCount scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept;
    LineCount scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes = {}) noexcept;
    void scrollDown(LineCount n, GraphicsAttributes const& defaultAttributes, Margin const& margin);
    void unscroll(LineCount n, GraphicsAttributes const& defaultAttributes);
    void scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept;
    // }}}

    // {{{ Rendering API
    /// Passes every cell of the visible rows to @p render.
    ///
    /// @param render        The render pass (RenderBufferBuilder in production).
    /// @param scrollOffset  How far the viewport is scrolled into the history.
    /// @param highlightSearchMatches  Whether the pass highlights search matches.
    /// @param extraLines    Rows to draw ABOVE the page, for smooth scrolling. Ignored when @p rows is
    ///                      given, which names its own extra rows.
    /// @param rows          The exact rows to draw, top first, when the caller needs a NON-CONTIGUOUS
    ///                      selection of them -- which is what output folding needs, its collapsed
    ///                      blocks leaving gaps a linear walk cannot express. Empty (the default) walks
    ///                      the page linearly, as it always has. Rows beyond the page count are drawn
    ///                      above it, exactly as @p extraLines does, so smooth scrolling works either
    ///                      way. Grid deliberately knows nothing about folding: it draws the rows it is
    ///                      handed and the decision of WHICH stays with the caller.
    template <typename RendererT>
    [[nodiscard]] RenderPassHints render(
        RendererT&& render,
        ScrollOffset scrollOffset = {},
        HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes,
        LineCount extraLines = LineCount(0),
        std::span<LineOffset const> rows = {}) const;

    [[nodiscard]] std::string renderMainPageText() const;
    [[nodiscard]] std::string renderAllText() const;

    /// Renders the inclusive row range [@p start, @p end] (LineOffsets: 0 = top of the page, negative =
    /// into scrollback, positive = down the page) as one string per row. The range is clamped to the
    /// rows that still hold valid data — the same floor `forEachValidLine` uses, NOT
    /// -historyLineCount(): at-capacity scrollDown wraps destroyed page rows into the oldest history
    /// slots without resetting them, and capturing from below the floor returns that garbage.
    /// An empty/inverted range yields no rows. Backs `capture-pane` (including `-e` for SGR,
    /// `-J`/`-N` for trailing spaces and `-S -`/`-E` for scrollback).
    /// @param start First row (inclusive).
    /// @param end Last row (inclusive).
    /// @param rendition Whether to interleave SGR escape sequences preserving each cell's rendition.
    /// @param trailing Whether the default cells padding a row out to the page width are kept.
    /// @return One captured line per row.
    [[nodiscard]] std::vector<std::string> renderRange(LineOffset start,
                                                       LineOffset end,
                                                       CaptureRendition rendition,
                                                       CaptureTrailingSpaces trailing) const;
    // }}}

    // {{{ Stable row identity (the daemon's delta addressing)
    //
    // A stable id names a PHYSICAL row across ring rotations: scrolling changes a row's
    // LineOffset but never its id. Ids are only meaningful within one generation; a
    // generation bump means row identity was destroyed wholesale (resize/reflow, history
    // limit change, reset) and clients must resync. Plain ints, guarded by the terminal
    // lock like all grid state.

    /// The wholesale-rebuild counter: a change tells a mirror to resync from scratch.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

    /// The counter that invalidates stable ids, and a STRICT SUBSET of generation().
    ///
    /// The two were one counter, and that cost more than it said: a change of LINE count alone --
    /// a status line appearing, a window growing taller -- rotates the ring through the stable-id
    /// primitives, so every row keeps the id it had, yet it bumps the generation because a mirror
    /// still has to resync its geometry. A consumer that STORES ids across frames, as output folding
    /// does, read that as "your ids are worthless" and dropped everything the user had collapsed.
    ///
    /// Bumped only where identity truly dies: a column change (which reflows, rebuilding the ring),
    /// a history-limit change, a reset, and a reverse scroll past the addressable history.
    [[nodiscard]] uint64_t stableIdGeneration() const noexcept { return _stableIdGeneration; }

    /// The stable id of the (existing) row at @p offset.
    [[nodiscard]] int64_t stableLineIdOf(LineOffset offset) const noexcept
    {
        return _stableBase + unbox<int64_t>(offset);
    }

    /// The offset the stable id @p id currently maps to, or nullopt if the row was
    /// evicted (below the floor) or does not exist yet.
    [[nodiscard]] std::optional<LineOffset> lineOffsetOf(int64_t id) const noexcept
    {
        if (id < _stableFloor || id >= _stableBase + unbox<int64_t>(_pageSize.lines))
            return std::nullopt;
        return LineOffset::cast_from(id - _stableBase);
    }

    /// The oldest stable id still addressable; monotonic within a generation.
    /// Deliberately NOT derived from historyLineCount(): at-capacity scrollDown wraps
    /// destroyed page rows into the oldest history slots without resetting them, and a
    /// derived floor would re-validate those evicted ids against garbage.
    [[nodiscard]] int64_t stableRangeFloor() const noexcept { return _stableFloor; }

    /// The batch counter every line revision draws from (advanced by finalizeRevisions).
    [[nodiscard]] uint64_t seqno() const noexcept { return _seqno; }

    /// Stamps every dirty line with the next batch number in one pass over the page, the
    /// rows scrolled out since the last finalize (so a row written and then scrolled away
    /// within one batch still gets stamped), and any history row handed out mutably since
    /// then (so a row dirtied in place deep in the scrollback gets stamped at all). Bumps
    /// the seqno only if anything was stamped — an idle grid finalizes for free, and
    /// nothing runs at all when no consumer queries.
    void finalizeRevisions() noexcept;

    /// Reports every line changed since @p cursor and advances it.
    ///
    /// Self-finalizing. On a generation mismatch the cursor is re-anchored to the
    /// current state and ResyncRequired is returned WITHOUT reporting lines — the caller
    /// snapshots via forEachValidLine() instead. (A resync must never be "changes since
    /// seqno 0": post-rebuild rows legitimately keep revision 0 forever.)
    /// @param cursor The consumer's stream position (updated).
    /// @param callback Invoked as callback(LineOffset, Line const&) per changed line.
    template <typename F>
    [[nodiscard]] GridDeltaResult forEachLineChangedSince(GridDeltaCursor& cursor, F&& callback)
    {
        finalizeRevisions();
        if (cursor.generation != _generation)
        {
            cursor =
                GridDeltaCursor { .generation = _generation, .seqno = _seqno, .stableBase = _stableBase };
            return GridDeltaResult::ResyncRequired;
        }

        // Scan the page plus however far the ring advanced since the consumer last
        // looked, clamped to the rows that still hold valid data (the floor excludes
        // at-capacity-wrapped garbage slots).
        //
        // Plus, for a consumer that has not passed the seqno a history row was last stamped at, down
        // to that row: a scrollback line can be dirtied WITHOUT anything scrolling (Screen's OSC 133
        // handlers mark a logical line's head, which walks up wrapped rows into the history), and no
        // later scroll ever brings it back into the prefix above — so it would be stamped and then
        // never reported. A consumer already past that seqno has seen it and scans nothing extra.
        auto&& report = std::forward<F>(callback);
        auto const scrolledFloor = _stableBase - scrolledOutDepthSince(cursor.stableBase);
        auto const floorId = cursor.seqno < _changedHistorySeqno
                                 ? std::min(scrolledFloor, _changedHistoryFloor)
                                 : scrolledFloor;
        for (auto offset = scanTopFor(floorId); offset < boxed_cast<LineOffset>(_pageSize.lines); ++offset)
        {
            auto const& line = std::as_const(*this).lineAt(offset);
            if (line.revision() > cursor.seqno)
                report(offset, line);
        }

        cursor.seqno = _seqno;
        cursor.stableBase = _stableBase;
        return GridDeltaResult::Delta;
    }

    /// The topmost offset whose row still holds valid data — the shallower of the history depth and
    /// the stable floor.
    ///
    /// Both bounds are needed, and neither implies the other: at capacity, scrollDown wraps
    /// destroyed page rows into the oldest history slots without resetting them, so the floor
    /// excludes rows historyLineCount() still counts; and a floor below the history is simply an id
    /// range no row exists for. Every walk over "the rows this grid actually has" starts here, so a
    /// second caller cannot pick a different top (@see forEachValidLine, renderRange).
    /// @return The offset to start at; 0 or negative.
    [[nodiscard]] LineOffset addressableTop() const noexcept
    {
        return LineOffset::cast_from(
            std::max(-unbox<int64_t>(historyLineCount()), _stableFloor - _stableBase));
    }

    /// Walks every valid line — the whole addressable range, no change filter — for the
    /// attach/resync snapshot.
    /// @param callback Invoked as callback(LineOffset, Line const&) per line.
    template <typename F>
    void forEachValidLine(F&& callback) const
    {
        auto&& report = std::forward<F>(callback);
        for (auto offset = addressableTop(); offset < boxed_cast<LineOffset>(_pageSize.lines); ++offset)
            report(offset, lineAt(offset));
    }

    /// Re-anchors @p cursor to the change stream's current head WITHOUT a scan.
    /// After an attach/resync snapshot (forEachValidLine reported the whole grid),
    /// the consumer has seen everything up to now, so its cursor jumps straight to
    /// the head — running forEachLineChangedSince purely to advance it would walk
    /// the grid a second time. Self-finalizing exactly like forEachLineChangedSince,
    /// and lands the cursor on the same {generation, seqno, stableBase} either of
    /// that method's branches would, so a following delta.seqno read stays consistent.
    /// @param cursor The consumer's stream position (re-anchored to now).
    void anchorCursorToHead(GridDeltaCursor& cursor) noexcept
    {
        finalizeRevisions();
        cursor = GridDeltaCursor { .generation = _generation, .seqno = _seqno, .stableBase = _stableBase };
    }
    // }}}

    [[nodiscard]] constexpr LineFlags defaultLineFlags() const noexcept;
    [[nodiscard]] constexpr LineCount linesUsed() const noexcept;

    void verifyState() const noexcept;

    [[nodiscard]] CellLocationRange wordRangeUnderCursor(CellLocation position,
                                                         std::u32string_view delimiters) const noexcept;

    [[nodiscard]] bool cellEmptyOrContainsOneOf(CellLocation position,
                                                std::u32string_view delimiters) const noexcept;

    [[nodiscard]] std::u32string extractText(CellLocationRange range) const noexcept;

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
        auto const cols = unbox<size_t>(line.size());
        auto const used = trimBlankRight(line.storage(), cols);

        if (used == 0)
            return CellLocation { .line = lineOffset, .column = ColumnOffset(0) };

        return CellLocation { .line = lineOffset, .column = ColumnOffset::cast_from(used - 1) };
    }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept
    {
        return lineAt(position.line).cellWidthAt(position.column);
    }

  private:
    CellLocation growLines(LineCount newHeight, CellLocation cursor);
    void clampHistory();

    // {{{ buffer helpers
    void resizeBuffers(PageSize newSize)
    {
        auto const newTotalLineCount = historyLineCount() + newSize.lines;
        _lines.resize(unbox<size_t>(newTotalLineCount));
        _pageSize = newSize;
    }

    void rezeroBuffers() noexcept { _lines.rezero(); }

    // The ONLY ring-rotation entry points: stable-id accounting lives here so every
    // scroll/unscroll/grow path keeps row identity by construction. (The former
    // uncentralized rotateBuffers(int)/appendNewLines paths were dead and are gone —
    // they would have been silent identity-desync holes.)

    void rotateBuffersLeft(LineCount count) noexcept
    {
        _lines.rotateLeft(unbox<size_t>(count));
        _stableBase += unbox<int64_t>(count);
        syncStableFloor();
    }

    void rotateBuffersRight(LineCount count) noexcept
    {
        _lines.rotateRight(unbox<size_t>(count));
        _stableBase -= unbox<int64_t>(count);
        if (_stableBase < _stableFloor)
        {
            if (historyLineCount() == LineCount(0))
            {
                // A zero-history grid (the alternate screen): there are no history
                // slots to hold garbage and no ids were ever issued below the base,
                // so the newly exposed top rows take strictly-fresh ids with no
                // collision. Drop the floor to the new base and KEEP the generation,
                // so a full-page reverse scroll stays an incremental delta instead
                // of forcing a whole-screen resnapshot to every attached mirror.
                _stableFloor = _stableBase;
                return;
            }
            // Reverse-scrolling past the addressable history sinks the base below the
            // floor: the newly exposed top page rows would take ids already issued to
            // evicted rows, and the floor cannot follow them down without re-validating
            // garbage slots. Row identity cannot survive this — rebuild it wholesale.
            // Every history row that was still valid provably lands in the caller's
            // blanked region, so after the bump the page is the entire valid range.
            _stableFloor = _stableBase;
            bumpGeneration(RowIdentity::Destroyed); // re-syncs the floor itself
            return;
        }
        syncStableFloor();
    }

    /// The count of rows that scrolled out of the page top since stable base @p priorBase,
    /// clamped to the still-valid history depth (the floor excludes at-capacity-wrapped
    /// garbage slots). This is the negative-offset span a delta or finalize scan must cover
    /// so a row written and then scrolled away within one batch is still seen at its new
    /// offset. Single-sources the boundary math shared by finalizeRevisions() and
    /// forEachLineChangedSince().
    [[nodiscard]] int64_t scrolledOutDepthSince(int64_t priorBase) const noexcept
    {
        return std::clamp<int64_t>(_stableBase - priorBase, std::int64_t { 0 }, _stableBase - _stableFloor);
    }

    /// The offset a scan bounded below by the stable id @p floorId must start at.
    ///
    /// Clamps the id to the oldest one that still holds valid data, then to the page top: a floor at
    /// or above the base means "the page only". Single-sources the id→offset conversion the finalize
    /// and delta scans share, so neither can walk off the addressable range.
    /// @param floorId The lowest stable id the scan wants to cover.
    /// @return Its offset, never positive.
    [[nodiscard]] LineOffset scanTopFor(int64_t floorId) const noexcept
    {
        return LineOffset::cast_from(std::min<int64_t>(0, std::max(floorId, _stableFloor) - _stableBase));
    }

    /// Records that the row at @p line was handed out for writing, so the batch stamping covers it
    /// even though nothing scrolled it into range. Page rows are always scanned and cost nothing here.
    /// @param line The offset just handed out mutably.
    void noteMutableRow(LineOffset line) noexcept
    {
        if (line < LineOffset(0))
            _dirtyHistoryFloor = std::min(_dirtyHistoryFloor, stableLineIdOf(line));
    }

    /// The row at @p line without the mutable-access bookkeeping — the raw ring index every accessor
    /// above is built from, and what the scans themselves must use so a finalize pass does not
    /// re-arm the very floor it is clearing.
    /// @param line The row offset (negative addresses history).
    /// @return The row.
    [[nodiscard]] Line& rowAt(LineOffset line) noexcept { return _lines[unbox<long>(line)]; }

    /// Re-establishes the floor invariant `_stableFloor >= _stableBase - history` after
    /// anything moved the base or shrank the history. max() keeps it monotonic: eviction
    /// only ever advances it within a generation.
    void syncStableFloor() noexcept
    {
        _stableFloor = std::max(_stableFloor, _stableBase - unbox<int64_t>(historyLineCount()));
    }

    /// Whether a rebuild left the rows' stable ids naming the rows they named before.
    enum class RowIdentity : uint8_t
    {
        Preserved = 0, ///< The ring rotated; every row kept its id.
        Destroyed,     ///< The ring was rebuilt; no id names what it did.
    };

    /// Tells mirrors to resync, and consumers holding stable ids whether those ids survived it.
    ///
    /// @param identity Whether the rebuild kept stable row identity (@see stableIdGeneration).
    void bumpGeneration(RowIdentity identity) noexcept
    {
        ++_generation;
        if (identity == RowIdentity::Destroyed)
            ++_stableIdGeneration;
        syncStableFloor();
        // Row identity is gone, so an id-keyed history watermark means nothing now. Consumers
        // resync on the generation mismatch anyway.
        _dirtyHistoryFloor = NoHistoryFloor;
        _changedHistoryFloor = NoHistoryFloor;
        _changedHistorySeqno = 0;
        // Re-anchor the finalize scan: the pre-rebuild base delta is meaningless now.
        _stableBaseAtLastFinalize = _stableBase;
    }

    /// Resets the topmost @p count lines of the main page to blank.
    ///
    /// Always index the page through `lineAt()` like this rather than over a contiguous span: the
    /// callers below run right after a rotation, which is precisely when the logical page is most
    /// likely to straddle the ring's physical end.
    ///
    /// @param count             How many lines from the top of the page to reset.
    /// @param defaultAttributes The attributes the blanked cells take on.
    void resetPageLines(LineCount count, GraphicsAttributes defaultAttributes) noexcept;
    // }}}

    // private fields
    //
    PageSize _pageSize;
    bool _reflowOnResize = false;
    MaxHistoryLineCount _historyLimit;
    Lines _lines;
    LineCount _linesUsed;

    // Stable row identity (see the accessors above): maintained exclusively by the
    // ring-rotation primitives, syncStableFloor() and bumpGeneration().
    uint64_t _generation = 0;
    uint64_t _stableIdGeneration = 0;
    int64_t _stableBase = 0;  ///< Stable id of page row 0; signed — SD/unscroll push it down.
    int64_t _stableFloor = 0; ///< Oldest addressable id; monotonic within a generation.

    // Batch stamping (see finalizeRevisions()).

    /// The "no history row involved" value of the two floors below: above every real stable id, so
    /// a std::min against it is a no-op and no clamping special case is needed.
    static constexpr int64_t NoHistoryFloor = std::numeric_limits<int64_t>::max();

    /// The lowest stable id announced through changingLineAt() since the last finalize, or
    /// NoHistoryFloor when no history row was. A scrollback row can be dirtied with nothing
    /// scrolling at all — Screen's OSC 133 handlers stamp semantic marks on a logical line's HEAD,
    /// and logicalLineHead() walks up wrapped rows into the history — and such a row lies outside
    /// the scrolled-out prefix, so without this it would never be stamped and the change would
    /// never reach a client. Cleared by every finalize.
    int64_t _dirtyHistoryFloor = NoHistoryFloor;

    /// The lowest history stable id a finalize actually STAMPED, and the seqno it stamped at.
    /// Sticky within a generation (bumpGeneration clears both): a consumer whose cursor predates
    /// that seqno has not seen the row and no scroll will bring it into range, so
    /// forEachLineChangedSince extends its scan down to the floor for that consumer alone. The
    /// floor is clamped to _stableFloor at use, so eviction bounds how deep the extension can go.
    /// Never derived from _dirtyHistoryFloor: an announced change that turned out not to change
    /// anything must not widen every later scan for the rest of the generation.
    int64_t _changedHistoryFloor = NoHistoryFloor;
    uint64_t _changedHistorySeqno = 0;

    uint64_t _seqno = 0;                   ///< The single monotonic source revisions draw from.
    int64_t _stableBaseAtLastFinalize = 0; ///< Bounds the finalize scan to scrolled-out rows.
                                           ///< Bootstrap: starts at 0 matching _stableBase,
                                           ///< so before the first finalize, no scrolled-out
                                           ///< prefix is scanned. New lines are born dirty,
                                           ///< so they ARE stamped on the first pass — the
                                           ///< missing prefix scan is harmless by construction.
                                           ///< Any code that advances _stableBase outside of
                                           ///< rotateBuffersLeft/Right must update this.
};

std::ostream& dumpGrid(std::ostream& os, Grid const& grid);
std::string dumpGrid(Grid const& grid);

// {{{ impl
constexpr LineFlags Grid::defaultLineFlags() const noexcept
{
    return _reflowOnResize ? LineFlag::Wrappable : LineFlag::None;
}

constexpr LineCount Grid::linesUsed() const noexcept
{
    return _linesUsed;
}

inline bool Grid::isLineWrapped(LineOffset line) const noexcept
{
    return line >= -boxed_cast<LineOffset>(historyLineCount())
           && boxed_cast<LineCount>(line) < _pageSize.lines && lineAt(line).wrapped();
}

template <typename RendererT>
[[nodiscard]] RenderPassHints Grid::render(
    RendererT&& render, // NOLINT(cppcoreguidelines-missing-std-forward)
    ScrollOffset scrollOffset,
    HighlightSearchMatches highlightSearchMatches,
    LineCount extraLines,
    std::span<LineOffset const> rows) const
{
    assert(!scrollOffset || unbox<LineCount>(scrollOffset) <= historyLineCount());

    auto hints = RenderPassHints {};

    auto const renderRow = [&](Line const& line, LineOffset y) {
        // Fast path: uniform-attribute line — render as a single batch. Blank lines have no
        // codepoints, so no search pattern can match them; always use the trivial path for
        // blanks to avoid constructing ConstCellProxy on un-materialized SoA arrays.
        if (line.isBlank()
            || (line.isTrivialBuffer() && highlightSearchMatches == HighlightSearchMatches::No))
        {
            std::u32string trivialText;
            auto const tb = line.trivialBuffer(trivialText);
            auto const cellFlags = tb.textAttributes.flags;
            hints.containsBlinkingCells = hints.containsBlinkingCells || (cellFlags & CellFlag::Blinking)
                                          || (cellFlags & CellFlag::RapidBlinking);
            render.renderTrivialLine(tb, y, line.flags(), line.contextId(), trivialText);
        }
        else
        {
            // Per-cell rendering for lines with mixed attributes or search highlighting.
            auto x = ColumnOffset(0);
            auto const& storage = line.storage();
            auto const cols = unbox<size_t>(line.size());

            render.startLine(y, line.flags(), line.contextId());
            for (size_t col = 0; col < cols; ++col)
            {
                auto const proxy = ConstCellProxy(storage, col);
                auto const cellFlags = proxy.flags();
                hints.containsBlinkingCells = hints.containsBlinkingCells || (cellFlags & CellFlag::Blinking)
                                              || (cellFlags & CellFlag::RapidBlinking);
                render.renderCell(proxy, y, x++);
            }
            render.endLine();
        }
    };

    if (rows.empty())
    {
        auto const availableAbove = *historyLineCount() - *scrollOffset;
        auto const extraOffset = std::min(*extraLines, std::max(0, availableAbove));
        auto y = LineOffset(-extraOffset);
        for (int i = -*scrollOffset - extraOffset, e = i + *_pageSize.lines + extraOffset; i != e; ++i, ++y)
            renderRow(_lines[i], y);
    }
    else
    {
        // Placed by the one statement of the rule, which Viewport's coordinate translation reads as well
        // (@see foldedRowsTopRow): those two disagreeing is precisely what misplaces a selection.
        auto y = foldedRowsTopRow(_pageSize.lines, rows.size());
        for (auto const row: rows)
            renderRow(lineAt(row), y++);
    }

    render.finish();
    return hints;
}
// }}}

} // namespace vtbackend

// {{{ fmt formatter
template <>
struct std::formatter<vtbackend::Margin::Horizontal>: std::formatter<std::string>
{
    auto format(vtbackend::Margin::Horizontal const range, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}..{}", range.from, range.to), ctx);
    }
};

template <>
struct std::formatter<vtbackend::Margin::Vertical>: std::formatter<std::string>
{
    auto format(vtbackend::Margin::Vertical const range, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}..{}", range.from, range.to), ctx);
    }
};

// }}}
