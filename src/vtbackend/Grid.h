// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellProxy.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/defines.h>
#include <crispy/ring.h>

#include <libunicode/convert.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

using Lines = crispy::ring<Line>;

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
                    && (line + 1)->get().matchTextAtWithSensetivityMode(
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
                    && (line + 1)->get().matchTextAtWithSensetivityMode(
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
            if (line.matchTextAtWithSensetivityMode(
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
            if (line.matchTextAtWithSensetivityMode(searchText, ColumnOffset(0), isCaseSensitive))
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
            if (!startLine->get().matchTextAtWithSensetivityMode(segment, startCol, isCaseSensitive))
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
            if (!startLine->get().matchTextAtWithSensetivityMode(i, startCol, isCaseSensitive))
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

    [[nodiscard]] size_t zero_index() const noexcept { return _lines.zero_index(); }
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
    template <typename RendererT>
    [[nodiscard]] RenderPassHints render(
        RendererT&& render,
        ScrollOffset scrollOffset = {},
        HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes,
        LineCount extraLines = LineCount(0)) const;

    [[nodiscard]] std::string renderMainPageText() const;
    [[nodiscard]] std::string renderAllText() const;
    // }}}

    // {{{ Stable row identity (the daemon's delta addressing)
    //
    // A stable id names a PHYSICAL row across ring rotations: scrolling changes a row's
    // LineOffset but never its id. Ids are only meaningful within one generation; a
    // generation bump means row identity was destroyed wholesale (resize/reflow, history
    // limit change, reset) and clients must resync. Plain ints, guarded by the terminal
    // lock like all grid state.

    /// The wholesale-rebuild counter: a change invalidates every stable id.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

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
        _lines.rotate_left(unbox<size_t>(count));
        _stableBase += unbox<int64_t>(count);
        syncStableFloor();
    }

    void rotateBuffersRight(LineCount count) noexcept
    {
        _lines.rotate_right(unbox<size_t>(count));
        _stableBase -= unbox<int64_t>(count);
        syncStableFloor();
    }

    /// Re-establishes the floor invariant `_stableFloor >= _stableBase - history` after
    /// anything moved the base or shrank the history. max() keeps it monotonic: eviction
    /// only ever advances it within a generation.
    void syncStableFloor() noexcept
    {
        _stableFloor = std::max(_stableFloor, _stableBase - unbox<int64_t>(historyLineCount()));
    }

    /// Destroys stable row identity wholesale (resize/reflow, history-limit change,
    /// reset): clients observe the change and resync.
    void bumpGeneration() noexcept
    {
        ++_generation;
        syncStableFloor();
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
    int64_t _stableBase = 0;  ///< Stable id of page row 0; signed — SD/unscroll push it down.
    int64_t _stableFloor = 0; ///< Oldest addressable id; monotonic within a generation.
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
    LineCount extraLines) const
{
    assert(!scrollOffset || unbox<LineCount>(scrollOffset) <= historyLineCount());

    auto const availableAbove = *historyLineCount() - *scrollOffset;
    auto const extraOffset = std::min(*extraLines, std::max(0, availableAbove));
    auto y = LineOffset(-extraOffset);
    auto hints = RenderPassHints {};
    for (int i = -*scrollOffset - extraOffset, e = i + *_pageSize.lines + extraOffset; i != e; ++i, ++y)
    {
        Line const& line = _lines[i];

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
            render.renderTrivialLine(tb, y, line.flags(), trivialText);
        }
        else
        {
            // Per-cell rendering for lines with mixed attributes or search highlighting.
            auto x = ColumnOffset(0);
            auto const& storage = line.storage();
            auto const cols = unbox<size_t>(line.size());

            render.startLine(y, line.flags());
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
