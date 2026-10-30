// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/grid/CellProxy.hpp>
#include <vtbackend/grid/Line.hpp>

#include <crispy/Ring.hpp>

#include <core/Assert.hpp>
#include <core/Defines.hpp>

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

/// A change of the page's width, as a resize makes it.
struct WidthChange
{
    ColumnCount from; ///< The width before.
    ColumnCount to;   ///< The width after.
};

/// Where the logical column @p column of a line whose rows a resize leaves as they are -- each cut at the
/// new width or padded to it, as Grid::resize() does from its unreflowedFrom on -- lies afterwards: on the
/// same row of the line, held to its last column where the cut took the cell away. That is where a cursor
/// on such a row goes too, so a column marked where the cursor stood still names the cursor's cell.
/// @param column The column, counted across the logical line's rows of the width before.
/// @param width The resize's change of width.
/// @return The column, counted across the logical line's rows of the width after.
[[nodiscard]] constexpr ColumnOffset unreflowedColumn(ColumnOffset column, WidthChange width) noexcept
{
    auto const row = unbox<int>(column) / unbox<int>(width.from);
    auto const inRow = std::min(unbox<int>(column) % unbox<int>(width.from), unbox<int>(width.to) - 1);
    return ColumnOffset::cast_from((row * unbox<int>(width.to)) + inRow);
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
    Grid(PageSize pageSize, bool reflowOnResize, HistoryLimits historyLimits);

    /// A scrollback with no headroom, which is what a single depth has always meant. Spelled here
    /// rather than as a converting constructor on HistoryLimits, so the widening from one number to
    /// two bounds happens only where a caller asked for it.
    Grid(PageSize pageSize, bool reflowOnResize, MaxHistoryLineCount maxHistoryLineCount):
        Grid(pageSize, reflowOnResize, HistoryLimits::plain(maxHistoryLineCount))
    {
    }

    Grid(): Grid(PageSize { LineCount(25), ColumnCount(80) }, false, LineCount(0)) {}

    void reset();

    // {{{ grid global properties
    /// @return The ring's history capacity -- the depth beyond which rows are dropped no matter what.
    [[nodiscard]] LineCount maxHistoryLineCount() const noexcept
    {
        if (auto const* maxLineCount = std::get_if<LineCount>(&_historyLimits.capacity))
            return *maxLineCount;
        else
            return LineCount::cast_from(_lines.size()) - _pageSize.lines;
    }

    [[nodiscard]] HistoryLimits const& historyLimits() const noexcept { return _historyLimits; }

    /// Changes how much scrollback the ring keeps, rebuilding it -- which destroys stable row identity, so
    /// stableIdGeneration() moves. Limits equal to the ones in force change nothing at all.
    /// @param historyLimits The new limits.
    void setHistoryLimits(HistoryLimits historyLimits);

    [[nodiscard]] LineCount totalLineCount() const noexcept
    {
        return maxHistoryLineCount() + _pageSize.lines;
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept { return _linesUsed - _pageSize.lines; }

    [[nodiscard]] bool reflowOnResize() const noexcept { return _reflowOnResize; }
    void setReflowOnResize(bool enabled) { _reflowOnResize = enabled; }

    [[nodiscard]] PageSize pageSize() const noexcept { return _pageSize; }

    /// Resizes the main page area of the grid and adapts the scrollback area's width accordingly.
    ///
    /// @param newSize The new page size, at least one column wide.
    /// @param currentCursorPos The cursor's position on the current page.
    /// @param wrapPending Whether the cursor has a deferred wrap pending -- the next character goes one
    ///                    column past the cursor. A column change takes the cursor there -- widening
    ///                    has the room the wrap was waiting for, and a reflow keeps that offset -- so
    ///                    the wrap is no longer pending afterwards, except where a narrowing that does
    ///                    not reflow leaves the cursor on the last column.
    /// @return The cursor's position on the resized page. A column change that reflows keeps the
    ///         cursor on its cell: its logical line and its offset into that line, a row being added
    ///         to the line where the offset lies past its text. One that does not reflow keeps the
    ///         cursor's line, and narrowing moves a cursor past the new width onto the last column.
    ///         Nothing else is clamped: the caller keeps the result on the page.
    [[nodiscard]] CellLocation resize(PageSize newSize,
                                      CellLocation currentCursorPos,
                                      WrapPending wrapPending);

    /// Resizes as the overload above does, carrying several cursors along at once -- the live cursor and
    /// the one DECSC saved, as kitty and foot carry both through a reflow.
    ///
    /// @param newSize The new page size, at least one column wide.
    /// @param cursors In: where the next character goes from each cursor on the current page, as
    ///                nextWritePosition() gives it. Out: the cursor's position on the resized page, under
    ///                the rules the overload above states. The first cursor decides where lines are added
    ///                or removed when the height changes; the others move with the rows.
    /// @param unreflowedFrom A row from which a reflowing column change leaves the rows as they are, if
    ///                       any: from the head of its logical line down to the bottom of the page, each
    ///                       row is cut at the new width or padded to it and stays a row of its own, as a row
    ///                       that does not wrap is cut. A cursor on one keeps its row, held to the last
    ///                       column, and every offset those logical lines carry moves with the cell it names
    ///                       (@see unreflowedColumn). The rows above reflow as ever. Screen leaves the
    ///                       shell's live prompt so, for the shell to redraw.
    void resize(PageSize newSize, std::span<CellLocation> cursors, std::optional<LineOffset> unreflowedFrom);

    /// Where the next character goes from a cursor, as resize() takes it: its cell, or, when its wrap is
    /// pending and the resize to @p newSize changes the column count, the column past it -- which may be
    /// one past the last. A height change alone keeps the cursor on its cell, the wrap still pending.
    /// @param newSize The page size resize() is about to be given.
    /// @param cursor The cursor's position on the current page.
    /// @param wrapPending Whether the cursor's wrap is pending.
    /// @return Where the next character goes from the cursor, for resize().
    [[nodiscard]] CellLocation nextWritePosition(PageSize newSize,
                                                 CellLocation cursor,
                                                 WrapPending wrapPending) const noexcept;
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

    /// Whether row @p line continues the row above it after a soft wrap, that row still being in the grid:
    /// the top row of the history may carry the flag of a row that has since scrolled out.
    /// @param line The row to ask about.
    /// @return Whether a word or a match may run on from the row above into this one.
    [[nodiscard]] bool continuesRowAbove(LineOffset line) const noexcept
    {
        return line > -boxed_cast<LineOffset>(historyLineCount()) && isLineWrapped(line);
    }

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

    /// The cell at logical column @p column of the logical line @p head starts: logicalColumnOf()'s inverse.
    ///
    /// A column past the line's own rows -- as a narrowing that cuts its rows rather than reflowing them can
    /// leave a column the line marks -- is held to the line's last cell, never carried onto a row below it.
    /// @param head The logical line's head.
    /// @param column A column counted from that head, across the line's wraps.
    /// @param limit The lowest row the line is read to (logicalLineEnd()); not above @p head.
    /// @return The cell, on the row the wraps carry that column to, or the line's last cell.
    [[nodiscard]] CellLocation cellAtLogicalColumn(LineOffset head,
                                                   ColumnOffset column,
                                                   LineOffset limit) const;

    /// The last physical line of the logical line @p head starts, but none below @p limit.
    /// @param head The logical line's head.
    /// @param limit The lowest row the line is read to; not above @p head.
    /// @return The row of its last continuation, or @p limit when it runs on below that.
    [[nodiscard]] LineOffset logicalLineEnd(LineOffset head, LineOffset limit) const;

    /// The text of one logical line -- its rows [@p first, @p last] -- over its logical columns [@p begin,
    /// @p end).
    ///
    /// Columns are counted across a logical line's wraps, so they run straight through them -- which is
    /// exactly what makes a ;B or ;D offset survive a resize. A wrap joins its head seamlessly: the blanks
    /// that end a row the line runs on from are part of the line.
    /// @param first The logical line's head.
    /// @param last The last of its rows read; not above @p first.
    /// @param begin The logical column the text starts at.
    /// @param end The logical column the text stops before; nullopt reads through @p last.
    /// @param rendition Text only, or with the SGR sequences its cells wear (Line::toUtf8WithSgr()).
    /// @param maxBytes Stops before the next row once the text holds more: enough for a caller that
    ///        truncates, without copying an unbounded region of the grid.
    /// @return The text, trailing blanks dropped.
    [[nodiscard]] std::string logicalLineText(LineOffset first,
                                              LineOffset last,
                                              ColumnOffset begin,
                                              std::optional<ColumnOffset> end,
                                              CaptureRendition rendition,
                                              size_t maxBytes = std::numeric_limits<size_t>::max()) const;

    /// Whether logicalLineText() reads nothing from the same columns: every cell there is empty or a
    /// space, whatever rendition it wears. Answered from the cells, without rendering them.
    /// @param first The logical line's head.
    /// @param last The last of its rows looked at; not above @p first.
    /// @param begin The logical column to start at.
    /// @param end The logical column to stop before; nullopt looks through @p last.
    /// @return true when the columns hold no text.
    [[nodiscard]] bool isLogicalLineBlank(LineOffset first,
                                          LineOffset last,
                                          ColumnOffset begin,
                                          std::optional<ColumnOffset> end) const;

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

    /// Drops every row above @p head -- the scrollback and the page rows alike -- so that @p head becomes
    /// the topmost row.
    ///
    /// Rows leave through the grid's own bookkeeping -- scrollUp() then clearHistory() for a head on the
    /// page, a shrunken history for one in the scrollback -- so the stable floor and the eviction count
    /// follow by construction (@see evictedRowCount).
    /// @param head The row to keep at the top; in [addressableTop(), the page's last row].
    /// @return How far the page scrolled up: page rows sit this many rows higher than before. Zero for a
    ///         head in the scrollback, whose rows keep their offsets.
    [[nodiscard]] LineCount dropRowsAbove(LineOffset head);

    LineCount scrollUp(LineCount n, GraphicsAttributes defaultAttributes, Margin margin) noexcept;
    LineCount scrollUp(LineCount linesCountToScrollUp, GraphicsAttributes defaultAttributes = {}) noexcept;
    void scrollDown(LineCount n, GraphicsAttributes const& defaultAttributes, Margin const& margin);
    void unscroll(LineCount n, GraphicsAttributes const& defaultAttributes);
    void scrollLeft(GraphicsAttributes defaultAttributes, Margin margin) noexcept;

    /// What scrollUp(@p n, ..., @p margin) is about to do to the contents the stable ids name.
    ///
    /// Asked before the scroll, in the ids that holders of ids have. A scroll of the whole page carries every
    /// id along, and one between left and right margins moves cells, not rows: neither moves a row's content
    /// away from its id.
    /// @return What moved or went under which ids, or nullopt where nothing did.
    [[nodiscard]] std::optional<RowMove> rowMoveOfScrollUp(LineCount n, Margin margin) const noexcept;

    /// What scrollDown(@p n, ..., @p margin) is about to do to the contents the stable ids name. Asked
    /// before the scroll. It drops the rows it pushes off the region's bottom, and never touches the history:
    /// the region's rows keep their ids as they move where there is no history, and otherwise move from id
    /// to id, as they do inside margins.
    /// @return What moved or went under which ids, or nullopt where nothing did.
    [[nodiscard]] std::optional<RowMove> rowMoveOfScrollDown(LineCount n, Margin margin) const noexcept;

    /// What unscroll(@p n) is about to do to the contents the stable ids name: the history rows it pulls back
    /// keep theirs, the rows pushed off the page's bottom are dropped.
    /// @return What went under which ids, or nullopt where nothing did.
    [[nodiscard]] std::optional<RowMove> rowMoveOfUnscroll(LineCount n) const noexcept;
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
    /// rows that still hold valid data — the same top `forEachValidLine` uses, addressableTop() — so a
    /// capture never returns a row a mirror does not hold. An empty/inverted range yields no rows. Backs
    /// `capture-pane` (including `-e` for SGR, `-J`/`-N` for trailing spaces and `-S -`/`-E` for
    /// scrollback).
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
    /// a history-limit change, and a reset.
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
    /// Deliberately NOT derived from historyLineCount(), so that nothing which grows the
    /// history can re-validate ids the floor has already passed.
    [[nodiscard]] int64_t stableRangeFloor() const noexcept { return _stableFloor; }

    /// How many rows have left this grid for good since it was created or last reset.
    ///
    /// The gutter's absolute line numbers rest on it: a row's number is this count plus its distance
    /// from addressableTop(), plus one, so it stays put while the history scrolls away.
    /// "Left for good" means the stable floor passed it -- line-wise eviction at capacity, a
    /// block-atomic trim, ED 3, a shrunken history limit, a page grown into a full ring -- and it is
    /// counted where the floor moves rather than at each of those sites, so a new way of dropping rows
    /// is counted by construction.
    /// @return The count; zero after reset().
    [[nodiscard]] uint64_t evictedRowCount() const noexcept { return _evictedRowCount; }

    /// Takes @p count as the eviction count, for a grid that mirrors another grid's rows.
    ///
    /// A replica's own floor moves are not its source's evictions: it never saw the rows the source
    /// evicted before it began mirroring, nor a reset of the source, and a replay that rebuilds its
    /// history moves its floor across rows the source still holds. So it numbers its rows by the
    /// source's count instead, re-adopted after every update. A grid that owns its rows never calls
    /// this.
    /// @param count The count that makes the addressableTop() row number `count + 1`.
    void adoptEvictedRowCount(uint64_t count) noexcept { _evictedRowCount = count; }

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
    /// seqno 0": the scan covers only the page and the rows scrolled out since the cursor's
    /// base, so after a rebuild it cannot describe the history.)
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
        // evicted rows).
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

    /// The topmost offset whose row still holds valid data: the stable floor's.
    ///
    /// The floor names the oldest history row wherever a grid operation returns, so this equals
    /// -historyLineCount() (verifyState() requires it). Every walk over "the rows this grid actually
    /// has" starts here, so a second caller cannot pick a different top (@see forEachValidLine,
    /// renderRange).
    /// @return The offset to start at; 0 or negative.
    [[nodiscard]] LineOffset addressableTop() const noexcept
    {
        return LineOffset::cast_from(_stableFloor - _stableBase);
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

    /// Whether any line in the used range is a wrapped continuation.
    ///
    /// Reflow exists to rejoin such lines; when none is present, growing the column count produces
    /// exactly the lines already held and can widen them in place instead of rebuilding the ring.
    /// @return @c true when at least one used line is wrapped.
    [[nodiscard]] bool anyUsedLineWrapped() const noexcept;

    /// Whether narrowing to @p newColumnCount would neither rejoin nor cut anything.
    ///
    /// Stricter than @ref anyUsedLineWrapped because shrinking can lose content: it also requires
    /// that no line holds anything past the new width. Uses @c trimBlankRight rather than
    /// @c LineSoA::usedColumns, which @c resizeLineSoA clamps and which therefore reports a width
    /// that fits even for a line whose content was already cut.
    ///
    /// @param newColumnCount The width being narrowed to.
    /// @return @c true when narrowing is a blank-tail trim on every used line.
    [[nodiscard]] bool nothingToReflowOrCut(ColumnCount newColumnCount) const noexcept;

    [[nodiscard]] CellLocationRange wordRangeUnderCursor(CellLocation position,
                                                         std::u32string_view delimiters) const noexcept;

    [[nodiscard]] bool cellEmptyOrContainsOneOf(CellLocation position,
                                                std::u32string_view delimiters) const noexcept;

    [[nodiscard]] std::u32string extractText(CellLocationRange range) const noexcept;

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
    /// @return The depth block-atomic eviction will never cut below.
    [[nodiscard]] LineCount guaranteedHistoryLineCount() const noexcept
    {
        if (auto const* lineCount = std::get_if<LineCount>(&_historyLimits.guaranteed))
            return *lineCount;
        else
            return maxHistoryLineCount();
    }

    CellLocation growLines(LineCount newHeight, CellLocation cursor);

    /// Drops the oldest scrollback down to a command boundary, keeping at least the guaranteed depth.
    ///
    /// A *block start* is a row carrying LineFlag::Marked -- what OSC 133;A stamps at a prompt (and
    /// what the OSC 3008 synthesis stamps too; Vi's `mm` sets LineFlag::UserMark, which is no block
    /// start). The rule is one sentence: drop everything older than the NEWEST block start that still
    /// leaves @ref guaranteedHistoryLineCount rows behind. A block straddling that depth therefore
    /// survives whole, and a block entirely beyond it goes whole -- which is the difference between
    /// losing a command and losing the top half of one.
    ///
    /// Finding no such boundary is not a failure and must not fall back to cutting line-wise HERE:
    /// with no shell integration, or with one command whose output is larger than the whole
    /// headroom, the right answer is to leave the scrollback alone and let it fill to capacity,
    /// where scrollUp's at-capacity path evicts line-wise as it always has. That is the ceiling
    /// which keeps memory bounded when no boundary can be honoured.
    ///
    /// Called when the history is about to cross capacity rather than the moment it passes the
    /// guarantee. That keeps the headroom genuinely used rather than merely allocated, and lets an
    /// attached mirror resync once per trim instead of once per command (@see ScreenMirror's
    /// floor-outran-scroll replay).
    ///
    /// A row is examined once, as it crosses the guarantee, and the newest block start seen is
    /// remembered (@see _newestEligibleBlockStart). The one thing that misses is a mark stamped on a
    /// history row that has already been passed -- a mirror replaying a prompt mark deep in the
    /// scrollback. That boundary is then simply not used, which costs an eviction its snapping
    /// and never the guarantee, and is the price of not re-walking the scrollback per line.
    void clampHistory();

    /// Resets the bottom @p count lines of the page, which a scroll just rotated in from the top of a
    /// full ring, keeping their storage for the write that follows. @see Line::recycle
    /// @param count             How many lines were evicted into the page.
    /// @param defaultAttributes The fill attributes their blank cells appear to have.
    void recycleEvictedLines(LineCount count, GraphicsAttributes const& defaultAttributes) noexcept;

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

    /// Rotates a ring a column reflow has just rebuilt so that its @p history rows sit above the page,
    /// and gives them the ids just below the page's.
    ///
    /// The reflow renamed every row and dropped none -- a narrowing that overflows the capacity keeps
    /// the overflow until the next scroll evicts it, and that scroll counts it -- so unlike
    /// rotateBuffersLeft() this moves the floor without counting an eviction. The caller's generation
    /// bump is what tells id holders their ids are gone.
    /// @param history The rebuilt ring's history row count.
    void rekeyRebuiltRing(LineCount history) noexcept
    {
        _lines.rotateLeft(unbox<size_t>(history));
        _stableFloor = _stableBase;
        _stableBase += unbox<int64_t>(history);
    }

    /// How scrollDown() moves what its region holds: the one branch choice, shared by the scroll and by
    /// rowMoveOfScrollDown(), its prediction, so the two cannot disagree.
    enum class ScrollDownShape : uint8_t
    {
        /// A region narrower than the page: cells shift within each row, and no row moves.
        ShiftRegionCells = 0,
        /// A full-width region: its rows move within it, and the rows pushed off its bottom are gone.
        MoveRegionRows,
        /// The whole page of a grid without history: the ring rotates, which costs nothing and loses
        /// nothing, the rows it wraps round being the ones it blanks, and the rows that stay keep their
        /// ids, so a mirror is sent the blanked rows alone. With history the rotation would turn the
        /// newest history rows into the blanked ones and wrap the rows pushed off the page into the
        /// history's oldest slots, so the page's rows are moved instead.
        RotateRing,
    };

    /// The branch scrollDown() takes for @p margin.
    /// @param margin The scrolling region.
    /// @return How the region's content moves.
    [[nodiscard]] ScrollDownShape scrollDownShape(Margin const& margin) const noexcept;

    void rotateBuffersRight(LineCount count) noexcept
    {
        _lines.rotateRight(unbox<size_t>(count));
        _stableBase -= unbox<int64_t>(count);
        if (_stableBase < _stableFloor)
        {
            // Only a zero-history grid reverse-scrolls below its floor: every caller with history pulls
            // at most the history back. With no history slots to hold anything and no ids ever issued
            // below the base, the newly exposed top rows take strictly-fresh ids with no collision. Drop
            // the floor to the new base and KEEP the generation, so a full-page reverse scroll stays an
            // incremental delta instead of forcing a whole-screen resnapshot to every attached mirror.
            Require(historyLineCount() == LineCount(0));
            _stableFloor = _stableBase;
            return;
        }
        syncStableFloor();
    }

    /// The count of rows that scrolled out of the page top since stable base @p priorBase,
    /// clamped to the still-valid history depth (the floor excludes evicted rows). This is the
    /// negative-offset span a delta or finalize scan must cover so a row written and then scrolled away
    /// within one batch is still seen at its new offset. Single-sources the boundary math shared by
    /// finalizeRevisions() and forEachLineChangedSince().
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
    /// only ever advances it within a generation -- and every row it advances past is counted
    /// as evicted.
    /// @see evictedRowCount
    void syncStableFloor() noexcept
    {
        auto const raised = std::max(_stableFloor, _stableBase - unbox<int64_t>(historyLineCount()));
        _evictedRowCount += static_cast<uint64_t>(raised - _stableFloor);
        _stableFloor = raised;
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
        // The only invalidation the eviction scan needs: its ids name rows, so a rebuild that renames
        // every row leaves them naming nothing. Everything else -- scrolling either way, a line-count
        // resize, an eviction -- keeps row identity and therefore keeps them meaningful, which is what
        // lets the scan carry no other invalidation hooks.
        _evictionScan.reset();
        // Consumers resync on the generation mismatch, so the watermark of what they have seen goes.
        _changedHistoryFloor = NoHistoryFloor;
        _changedHistorySeqno = 0;
        // What the next finalize has to reach is a different matter: a row still waiting for its stamp
        // must get it, or it stays dirty and unstamped for good -- the scan reaches the history only
        // through these two bounds.
        switch (identity)
        {
            case RowIdentity::Preserved:
                // Every row keeps its id, so the rows written and scrolled out since the last finalize,
                // and those the shrinking page pushed up after them, are where the bounds already say.
                break;
            case RowIdentity::Destroyed:
                // No id names what it did, and a rebuilt row may be fresh, and so dirty: the next
                // finalize scans the whole valid history, once.
                _stableBaseAtLastFinalize = _stableBase;
                _dirtyHistoryFloor = _stableFloor;
                break;
        }
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
    HistoryLimits _historyLimits;

    /// How far @ref clampHistory has examined, and the best boundary it found -- one state, so that
    /// "a block start is remembered but nothing was ever examined" cannot be represented.
    ///
    /// This is what turns the eviction scan from a per-scroll walk of the whole scrollback into a
    /// walk of the rows that just crossed the guarantee. @see clampHistory for why that matters.
    struct EvictionScan
    {
        /// The highest stable id whose eligibility has been examined.
        int64_t examinedThrough {};

        /// The newest block start seen at or below the guarantee, if any. Validated against the
        /// current bound and floor at use rather than invalidated eagerly, because a reverse scroll
        /// can move the bound back underneath it.
        std::optional<int64_t> newestBlockStart;
    };

    /// Nullopt until the first examination, which is the one case that has to sweep the window whole.
    std::optional<EvictionScan> _evictionScan;
    Lines _lines;
    LineCount _linesUsed;

    // Stable row identity (see the accessors above): maintained exclusively by the
    // ring-rotation primitives, syncStableFloor() and bumpGeneration().
    uint64_t _generation = 0;
    uint64_t _stableIdGeneration = 0;
    /// Stable id of page row 0; signed — unscroll, and SD without history, push it down.
    int64_t _stableBase = 0;
    int64_t _stableFloor = 0; ///< Oldest addressable id; monotonic within a generation.
    /// Rows the floor has passed since the last reset, as evictedRowCount() reports them.
    uint64_t _evictedRowCount = 0;

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
