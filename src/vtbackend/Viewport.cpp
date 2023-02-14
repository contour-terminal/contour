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

#include <vtbackend/Terminal.h>
#include <vtbackend/Viewport.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <optional>

namespace terminal
{

bool Viewport::scrollUp(LineCount numLines)
{
    return scrollTo(
        std::min(_scrollOffset + numLines.as<ScrollOffset>(), boxed_cast<ScrollOffset>(historyLineCount())));
}

bool Viewport::scrollDown(LineCount numLines)
{
    return scrollTo(std::max(_scrollOffset - numLines.as<ScrollOffset>(), ScrollOffset(0)));
}

bool Viewport::scrollToTop()
{
    return scrollTo(boxed_cast<ScrollOffset>(historyLineCount()));
}

bool Viewport::scrollToBottom()
{
    if (scrollingDisabled())
        return false;

    return forceScrollToBottom();
}

bool Viewport::forceScrollToBottom()
{
    return scrollTo(ScrollOffset(0));
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset)
{
    return makeVisibleWithinSafeArea(lineOffset, _scrollOff);
}

bool Viewport::makeVisibleWithinSafeArea(LineOffset lineOffset, LineCount paddingLines)
{
    auto const viewportTop = -_scrollOffset.as<LineOffset>() + boxed_cast<LineOffset>(paddingLines);
    auto const viewportBottom = boxed_cast<LineOffset>(screenLineCount() - 1) - _scrollOffset.as<int>()
                                - boxed_cast<LineOffset>(paddingLines);

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
    return makeVisibleWithinSafeArea(lineOffset, LineCount(0));
}

bool Viewport::scrollTo(ScrollOffset offset)
{
    if (scrollingDisabled() && offset != ScrollOffset(0))
        return false;

    if (offset == _scrollOffset)
        return false;

    if (0 <= *offset && offset <= boxed_cast<ScrollOffset>(historyLineCount()))
    {
#if defined(CONTOUR_LOG_VIEWPORT)
        ViewportLog()("Scroll to offset {}", offset);
#endif
        _scrollOffset = offset;
        _modified();
        return true;
    }

#if defined(CONTOUR_LOG_VIEWPORT)
    ViewportLog()("Scroll to offset {} ignored. Out of bounds.", offset);
#endif
    return false;
}

bool Viewport::scrollMarkUp()
{
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        _terminal.primaryScreen().findMarkerUpwards(-boxed_cast<LineOffset>(_scrollOffset));
    if (newScrollOffset.has_value())
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));

    return false;
}

bool Viewport::scrollMarkDown()
{
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        _terminal.primaryScreen().findMarkerDownwards(-boxed_cast<LineOffset>(_scrollOffset));
    if (newScrollOffset)
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));
    else
        return forceScrollToBottom();

    return true;
}

LineCount Viewport::historyLineCount() const noexcept
{
    return _terminal.currentScreen().historyLineCount();
}

LineCount Viewport::screenLineCount() const noexcept
{
    return _terminal.pageSize().lines;
}

bool Viewport::scrollingDisabled() const noexcept
{
    // TODO: make configurable
    return _terminal.isAlternateScreen();
}

} // namespace terminal
