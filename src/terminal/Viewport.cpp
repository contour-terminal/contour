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

#include <terminal/Terminal.h>
#include <terminal/Viewport.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <optional>

namespace terminal
{

bool Viewport::scrollUp(LineCount _numLines)
{
    scrollOffset_ =
        std::min(scrollOffset_ + _numLines.as<ScrollOffset>(), boxed_cast<ScrollOffset>(historyLineCount()));
    return scrollTo(scrollOffset_);
}

bool Viewport::scrollDown(LineCount _numLines)
{
    scrollOffset_ = std::max(scrollOffset_ - _numLines.as<ScrollOffset>(), ScrollOffset(0));
    return scrollTo(scrollOffset_);
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
    if (!scrollOffset_)
        return false;

#if defined(CONTOUR_LOG_VIEWPORT)
    Log()("forcing scroll to bottom from {}", scrollOffset_);
#endif
    scrollOffset_ = ScrollOffset(0);
    modified_();
    return true;
}

bool Viewport::makeVisible(LineOffset lineOffset)
{
    auto const viewportTop = -scrollOffset_.as<LineOffset>();
    auto const viewportBottom = boxed_cast<LineOffset>(screenLineCount() - 1) - scrollOffset_.as<int>();

    // Is the line above the viewport?
    if (!(viewportTop < lineOffset))
        return scrollUp(LineCount::cast_from(viewportTop.as<int>() - lineOffset.as<int>()));

    // Is the line below the viewport?
    if (!(lineOffset < viewportBottom))
        return scrollDown(LineCount::cast_from(lineOffset.as<int>() - viewportBottom.as<int>()));

    return false;
}

bool Viewport::scrollTo(ScrollOffset _offset)
{
    if (scrollingDisabled())
        return false;

    if (_offset == scrollOffset_)
        return false;

    if (0 <= *_offset && _offset <= boxed_cast<ScrollOffset>(historyLineCount()))
    {
#if defined(CONTOUR_LOG_VIEWPORT)
        Log()("Scroll to offset {}", _offset);
#endif
        scrollOffset_ = _offset;
        modified_();
        return true;
    }

    return false;
}

bool Viewport::scrollMarkUp()
{
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        terminal_.primaryScreen().findMarkerUpwards(-boxed_cast<LineOffset>(scrollOffset_));
    if (newScrollOffset.has_value())
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));

    return false;
}

bool Viewport::scrollMarkDown()
{
    if (scrollingDisabled())
        return false;

    auto const newScrollOffset =
        terminal_.primaryScreen().findMarkerDownwards(-boxed_cast<LineOffset>(scrollOffset_));
    if (newScrollOffset)
        return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));
    else
        return forceScrollToBottom();

    return true;
}

LineCount Viewport::historyLineCount() const noexcept
{
    return terminal_.primaryScreen().historyLineCount();
}

LineCount Viewport::screenLineCount() const noexcept
{
    return terminal_.pageSize().lines;
}

bool Viewport::scrollingDisabled() const noexcept
{
    // TODO: make configurable
    return terminal_.isAlternateScreen();
}

} // namespace terminal
