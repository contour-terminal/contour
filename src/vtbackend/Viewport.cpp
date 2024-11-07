// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Terminal.h>
#include <vtbackend/Viewport.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <optional>

namespace vtbackend
{

bool Viewport::scrollUp(LineCount numLines)
{
    viewportLog()("scrollUp");
    auto offset =
        std::min(_scrollOffset + numLines.as<ScrollOffset>(), boxed_cast<ScrollOffset>(historyLineCount()));
    return scrollTo(offset);
}

bool Viewport::scrollDown(LineCount numLines)
{

    viewportLog()("scrollDown");
    return scrollTo(std::max(_scrollOffset - numLines.as<ScrollOffset>(), ScrollOffset(0)));
}

bool Viewport::scrollToTop()
{
    viewportLog()("scrollToTop");
    return scrollTo(boxed_cast<ScrollOffset>(historyLineCount()));
}

bool Viewport::scrollToBottom()
{
    viewportLog()("scrollToBottom");
    if (scrollingDisabled())
        return false;

    return forceScrollToBottom();
}

bool Viewport::forceScrollToBottom()
{
    viewportLog()("force ScrollToBottom");
    return scrollTo(ScrollOffset(0));
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset)
{
    viewportLog()("makeVisibleWithinSafeArea");
    return makeVisibleWithinSafeArea(lineOffset, _scrollOff);
}

CellLocation Viewport::clampCellLocation(CellLocation const& location) const noexcept
{
    auto const scrollOffset = _scrollOffset.as<LineOffset>();

    auto const viewportTop = -scrollOffset;
    auto const viewportBottom = boxed_cast<LineOffset>(screenLineCount() - 1) - scrollOffset;
    auto const viewportLeft = ColumnOffset(0);
    auto const viewportRight = boxed_cast<ColumnOffset>(_terminal->pageSize().columns - 1);

    auto const line = std::clamp(location.line, viewportTop, viewportBottom);
    auto const column = std::clamp(location.column, viewportLeft, viewportRight);

    return CellLocation { .line = line, .column = column };
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset, LineCount paddingLines)
{
    auto const viewportTop = -_scrollOffset.as<LineOffset>() + boxed_cast<LineOffset>(paddingLines);
    auto const viewportBottom = boxed_cast<LineOffset>(screenLineCount() - 1) - _scrollOffset.as<int>()
                                - boxed_cast<LineOffset>(paddingLines);

    viewportLog()("viewportTop {} viewportBottom {} lineOffset {}", viewportTop, viewportBottom, lineOffset);
    // Is the line above the viewport?
    if (!(viewportTop < lineOffset))
        return scrollUp(LineCount::cast_from(viewportTop - lineOffset));

    // Is the line below the viewport?
    if (!(lineOffset < viewportBottom))
        return scrollDown(LineCount::cast_from(lineOffset - viewportBottom));

    return false;
}

bool Viewport::makeVisible(LineOffset lineOffset)
{
    viewportLog()("makeVisible {}", unbox(lineOffset));
    return makeVisibleWithinSafeArea(lineOffset, LineCount(0));
}

bool Viewport::scrollTo(ScrollOffset offset)
{
    viewportLog()("scroll to {}", offset);
    if (scrollingDisabled() && offset != ScrollOffset(0))
        return false;

    if (offset == _scrollOffset)
        return false;

    if (0 <= *offset && offset <= boxed_cast<ScrollOffset>(historyLineCount()))
    {
        viewportLog()("Scroll to offset {}", offset);
        _scrollOffset = offset;
        _modified();
        return true;
    }

    viewportLog()("Scroll to offset {} ignored. Out of bounds.", offset);
    return false;
}

bool Viewport::scrollMarkUp()
{
    viewportLog()("Scroll to Mark Down");
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        _terminal->primaryScreen().findMarkerUpwards(-boxed_cast<LineOffset>(_scrollOffset));
    if (newScrollOffset.has_value())
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));

    return false;
}

bool Viewport::scrollMarkDown()
{
    viewportLog()("Scroll to Mark Down");
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        _terminal->primaryScreen().findMarkerDownwards(-boxed_cast<LineOffset>(_scrollOffset));
    if (newScrollOffset)
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));
    else
        return forceScrollToBottom();

    return true;
}

LineCount Viewport::historyLineCount() const noexcept
{
    return _terminal->currentScreen().historyLineCount();
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
