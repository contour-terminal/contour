// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Viewport.hpp>

#include <vtbackend/Terminal.hpp>

#include <crispy/LogStore.hpp>

#include <algorithm>
#include <optional>

namespace vtbackend
{

bool Viewport::scrollUp(LineCount numLines)
{
    ViewportLog()("scrollUp");
    auto offset = std::min(_scrollOffset + numLines.as<ScrollOffset>(),
                           boxed_cast<ScrollOffset>(scrollableLineCount()));
    return scrollTo(offset);
}

bool Viewport::scrollDown(LineCount numLines)
{

    ViewportLog()("scrollDown");
    return scrollTo(std::max(_scrollOffset - numLines.as<ScrollOffset>(), ScrollOffset(0)));
}

bool Viewport::scrollToTop()
{
    ViewportLog()("scrollToTop");
    return scrollTo(boxed_cast<ScrollOffset>(scrollableLineCount()));
}

bool Viewport::scrollToBottom()
{
    ViewportLog()("scrollToBottom");
    if (scrollingDisabled())
        return false;

    return forceScrollToBottom();
}

bool Viewport::forceScrollToBottom()
{
    ViewportLog()("force ScrollToBottom");
    bool changed = scrollTo(ScrollOffset(0));
    if (_pixelOffset != 0.0f)
    {
        resetPixelOffset();
        if (!changed)
            _modified();
        changed = true;
    }
    return changed;
}

bool Viewport::clampScrollOffset()
{
    auto const maxOffset = boxed_cast<ScrollOffset>(scrollableLineCount());
    if (_scrollOffset <= maxOffset)
        return false;

    ViewportLog()("clampScrollOffset: {} -> {}", _scrollOffset, maxOffset);
    _scrollOffset = maxOffset;

    // The sub-cell offset goes with it. It measures a slide between two adjacent rows, and the row it
    // was sliding towards is one a collapsed fold has just taken away.
    resetPixelOffset();
    _modified();
    return true;
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset)
{
    ViewportLog()("makeVisibleWithinSafeArea");
    return makeVisibleWithinSafeArea(lineOffset, _scrollOff);
}

CellLocation Viewport::clampCellLocation(CellLocation const& location) const
{
    auto const viewportLeft = ColumnOffset(0);
    auto const viewportRight = boxed_cast<ColumnOffset>(_terminal->pageSize().columns - 1);
    auto const column = std::clamp(location.column, viewportLeft, viewportRight);

    auto const projection = _terminal->foldProjection();
    if (projection.empty())
    {
        auto const scrollOffset = _scrollOffset.as<LineOffset>();
        auto const viewportTop = -scrollOffset;
        auto const viewportBottom = boxed_cast<LineOffset>(screenLineCount() - 1) - scrollOffset;
        return CellLocation { .line = std::clamp(location.line, viewportTop, viewportBottom),
                              .column = column };
    }

    // The bounds are the projection's own ends, and emphatically NOT -scrollOffset: the scroll offset
    // counts VISIBLE rows, so with collapsed folds in between, grid line -scrollOffset names a row far
    // DOWN the page rather than the one the viewport starts at. Clamping the Vi cursor against it is
    // what threw the cursor back towards the bottom on every `k` that scrolled.
    //
    // Both ends are rows the viewport draws, so the result is on screen by construction.
    return CellLocation { .line = std::clamp(location.line, projection.front(), projection.back()),
                          .column = column };
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset, LineCount paddingLines)
{
    // Distances measured in SCREEN rows, not grid lines. Scrolling moves by rows the user can see, and
    // with collapsed folds in between the two counts differ -- subtracting grid lines would scroll by
    // however many rows the folds hide as well, overshooting by exactly that much.
    auto const screenRow = translateGridToScreenCoordinate(lineOffset);
    auto const topRow = boxed_cast<LineOffset>(paddingLines);
    auto const bottomRow =
        boxed_cast<LineOffset>(screenLineCount() - 1) - boxed_cast<LineOffset>(paddingLines);

    ViewportLog()("makeVisibleWithinSafeArea: screenRow {} in [{}, {}] for line {}",
                  screenRow,
                  topRow,
                  bottomRow,
                  lineOffset);

    // Is the line above the viewport?
    if (!(topRow < screenRow))
        return scrollUp(LineCount::cast_from(topRow - screenRow));

    // Is the line below the viewport?
    if (!(screenRow < bottomRow))
        return scrollDown(LineCount::cast_from(screenRow - bottomRow));

    return false;
}

bool Viewport::makeVisible(LineOffset lineOffset)
{
    ViewportLog()("makeVisible {}", unbox(lineOffset));
    return makeVisibleWithinSafeArea(lineOffset, LineCount(0));
}

bool Viewport::scrollTo(ScrollOffset offset)
{
    ViewportLog()("scroll to {}", offset);
    if (scrollingDisabled() && offset != ScrollOffset(0))
        return false;

    if (offset == _scrollOffset)
        return false;

    if (0 <= *offset && offset <= boxed_cast<ScrollOffset>(scrollableLineCount()))
    {
        ViewportLog()("Scroll to offset {}", offset);
        _scrollOffset = offset;
        _modified();
        return true;
    }

    ViewportLog()("Scroll to offset {} ignored. Out of bounds.", offset);
    return false;
}

ScrollOffset Viewport::scrollOffsetForTopLine(LineOffset line) const
{
    // Measured from where the line is drawn NOW rather than from grid line 0, and this is the whole
    // subtlety: a scroll offset counts visible rows up from the bottom of the PAGE, and with folds
    // collapsed the page itself can hide rows -- so the visible distance from the oldest line down to
    // grid line 0 overshoots the largest offset scrollableLineCount() admits, by exactly the number of
    // page rows a fold has taken away. Scrolling up by one moves every line down one row, so the offset
    // that lands @p line on row zero is the current one less the row it occupies.
    auto const target =
        _scrollOffset - ScrollOffset::cast_from(unbox<int>(translateGridToScreenCoordinate(line)));

    // Clamped rather than handed out for scrollTo() to refuse: a mark so far up that the viewport
    // cannot bring it all the way to the top still belongs at the top of the scrollback, which shows
    // it. Returning the unreachable number instead is what made the action a silent no-op.
    return std::clamp(target, ScrollOffset(0), boxed_cast<ScrollOffset>(scrollableLineCount()));
}

std::optional<LineOffset> Viewport::findVisibleMarker(LineOffset start, VerticalDirection direction) const
{
    auto const& screen = _terminal->primaryScreen();

    auto line = start;
    while (auto const candidate = direction == VerticalDirection::Up ? screen.findMarkerUpwards(line)
                                                                     : screen.findMarkerDownwards(line))
    {
        // A mark inside a collapsed block is drawn nowhere, so no offset brings it into view and
        // scrolling to it lands on whatever row stands in its place. Walk on rather than stop on a row
        // that cannot be shown.
        if (!_terminal->isLineHiddenByFold(*candidate))
            return candidate;

        // Strictly monotonic in either direction -- findMarker*() searches past its start line -- which
        // is what terminates this.
        line = *candidate;
    }

    return std::nullopt;
}

bool Viewport::scrollMarkUp()
{
    ViewportLog()("scrollMarkUp");
    if (scrollingDisabled())
        return false;

    auto const marker = findVisibleMarker(topLine(), VerticalDirection::Up);
    if (!marker)
        return false;

    return scrollTo(scrollOffsetForTopLine(*marker));
}

bool Viewport::scrollMarkDown()
{
    ViewportLog()("scrollMarkDown");
    if (scrollingDisabled())
        return false;

    auto const marker = findVisibleMarker(topLine(), VerticalDirection::Down);
    if (!marker)
        return forceScrollToBottom();

    return scrollTo(scrollOffsetForTopLine(*marker));
}

bool Viewport::isLineVisible(LineOffset line) const
{
    auto const projection = _terminal->foldProjection();
    if (projection.empty())
    {
        auto const a = -_scrollOffset.as<int>();
        auto const b = line.as<int>();
        auto const c = unbox(screenLineCount()) - _scrollOffset.as<int>();
        return a <= b && b < c;
    }

    // A line inside the drawn span can still be hidden by a collapsed fold, so this is a membership
    // test rather than an interval one. The projection is ascending, so it is one binary search.
    auto const it = std::ranges::lower_bound(projection, line);
    return it != projection.end() && *it == line;
}

LineOffset Viewport::translateScreenToGridLine(LineOffset line) const
{
    auto const projection = _terminal->foldProjection();
    if (projection.empty())
        return line - boxed_cast<LineOffset>(_scrollOffset);

    // BOTTOM-aligned, exactly as Grid::render draws it: when collapsed folds hide more rows than the
    // history can backfill, the projection is shorter than the page and the difference is blank rows at
    // the TOP. Assuming row 0 here while the render pass drew from further down is precisely what would
    // misplace a selection, so the offset comes from the one place that owns the rule.
    auto const index = unbox<int>(line) - unbox<int>(_terminal->foldProjectionTopRow());

    // Rows outside the projection are the blank ones above it and the space below; keep them
    // proportional to the nearest real row so a coordinate there stays ORDERED rather than clamping onto
    // it, which would make a selection dragged off the edge select the same row over and over.
    //
    // Continued in VISIBLE lines, because that is what a row is: a selection dragged one row above the
    // viewport must reach the line the viewport WOULD show next, not whichever grid line happens to sit
    // there with a collapsed block in between. advanceVisibleLines() also bounds the result by the
    // addressable grid, which extrapolation needs on its own account -- enough collapsed rows leave the
    // projection shorter than a page, and running off its bottom would otherwise name lines the grid
    // does not have, which Grid::rowAt resolves by wrapping its ring buffer onto unrelated storage.
    if (index < 0)
        return _terminal->advanceVisibleLines(projection.front(), -index, VerticalDirection::Up);
    if (static_cast<size_t>(index) >= projection.size())
        return _terminal->advanceVisibleLines(
            projection.back(), index - static_cast<int>(projection.size()) + 1, VerticalDirection::Down);
    return projection[static_cast<size_t>(index)];
}

LineOffset Viewport::translateGridToScreenCoordinate(LineOffset p) const
{
    auto const projection = _terminal->foldProjection();
    if (projection.empty())
        return p + boxed_cast<LineOffset>(_scrollOffset);

    // The projection is ascending in grid line, so the row showing p -- or, for a hidden line, the row
    // that stands where it would be -- is one binary search away.
    auto const top = unbox<int>(_terminal->foldProjectionTopRow());

    // Above everything drawn: off the top of the viewport, and it has to REPORT so. Returning the top
    // row instead would tell cursor-line and search-match highlighting that a line scrolled far out of
    // sight is the one on screen row zero.
    //
    // Counted in VISIBLE lines rather than grid lines, because makeVisibleWithinSafeArea() turns this
    // number straight into a scroll distance and a scroll travels in rows the user can see. A search
    // hit fifty rows up but five hundred grid lines up would otherwise ask for a five-hundred-row
    // scroll, which scrollUp() clamps to the top -- landing at the top of the scrollback instead of on
    // the match.
    if (p < projection.front())
        return LineOffset::cast_from(top - _terminal->visibleDistance(p, projection.front()));

    // Below everything drawn, and it has to report HOW far below, in the same visible rows. A flat
    // screenLineCount() would tell makeVisibleWithinSafeArea that every target under the viewport is
    // exactly one row past its bottom, so scrolling down to a line five hundred rows away would advance
    // by the safe-area padding and stop.
    auto const it = std::ranges::lower_bound(projection, p);
    if (it == projection.end())
        return LineOffset::cast_from(top + static_cast<int>(projection.size()) - 1
                                     + _terminal->visibleDistance(projection.back(), p));

    return LineOffset::cast_from(top + static_cast<int>(std::distance(projection.begin(), it)));
}

LineCount Viewport::historyLineCount() const noexcept
{
    return _terminal->currentScreen().historyLineCount();
}

LineCount Viewport::scrollableLineCount() const
{
    return std::max(LineCount(0), historyLineCount() - _terminal->hiddenLineCount());
}

LineCount Viewport::screenLineCount() const noexcept
{
    return _terminal->pageSize().lines;
}

bool Viewport::scrollingDisabled() const noexcept
{
    // TODO: make configurable
    return _terminal->isAlternateScreen();
}

} // namespace vtbackend
