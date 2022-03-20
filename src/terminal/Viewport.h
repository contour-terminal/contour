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
#pragma once

#include <terminal/Screen.h>
#include <terminal/primitives.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <optional>

namespace terminal
{

// #define CONTOUR_LOG_VIEWPORT 1

template <typename EventListener>
class Screen;

template <typename Cell>
class Viewport
{
  public:
#if defined(CONTOUR_LOG_VIEWPORT)
    static auto inline Log = logstore::Category("vt.viewport", "Logs viewport details.");
#endif

    using ModifyEvent = std::function<void()>;

    explicit Viewport(Screen<Cell>& _screen, ModifyEvent _onModify = {}):
        screen_ { _screen }, modified_ { _onModify ? std::move(_onModify) : []() {
        } }
    {
    }

    ScrollOffset scrollOffset() const noexcept { return scrollOffset_; }

    /// Tests if the viewport has been moved(/scrolled) off its main view position.
    ///
    /// @retval true viewport has been moved/scrolled off its main view position.
    /// @retval false viewport has NOT been moved/scrolled and is still located at its main view position.
    bool scrolled() const noexcept { return scrollOffset_.value != 0; }

    bool isLineVisible(LineOffset _line) const noexcept
    {
        auto const a = -scrollOffset_.as<int>();
        auto const b = _line.as<int>();
        auto const c = unbox<int>(screenLineCount()) - scrollOffset_.as<int>();
        return a <= b && b < c;
    }

    bool scrollUp(LineCount _numLines)
    {
        scrollOffset_ = std::min(scrollOffset_ + _numLines.as<ScrollOffset>(),
                                 boxed_cast<ScrollOffset>(historyLineCount()));
        return scrollTo(scrollOffset_);
    }

    bool scrollDown(LineCount _numLines)
    {
        scrollOffset_ = std::max(scrollOffset_ - _numLines.as<ScrollOffset>(), ScrollOffset(0));
        return scrollTo(scrollOffset_);
    }

    bool scrollToTop() { return scrollTo(boxed_cast<ScrollOffset>(historyLineCount())); }

    bool scrollToBottom()
    {
        if (scrollingDisabled())
            return false;

        return forceScrollToBottom();
    }

    bool forceScrollToBottom()
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

    bool scrollTo(ScrollOffset _offset)
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

    bool scrollMarkUp()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerUpwards(-boxed_cast<LineOffset>(scrollOffset_));
        if (newScrollOffset.has_value())
            return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));

        return false;
    }

    bool scrollMarkDown()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerDownwards(-boxed_cast<LineOffset>(scrollOffset_));
        if (newScrollOffset)
            return scrollTo(boxed_cast<ScrollOffset>(-*newScrollOffset));
        else
            return forceScrollToBottom();

        return true;
    }

    /// Translates a screen coordinate to a Grid-coordinate by applying
    /// the scroll-offset to it.
    constexpr CellLocation translateScreenToGridCoordinate(CellLocation p) const noexcept
    {
        return CellLocation {
            p.line - boxed_cast<LineOffset>(scrollOffset_),
            p.column,
        };
    }

  private:
    LineCount historyLineCount() const noexcept { return screen_.historyLineCount(); }
    LineCount screenLineCount() const noexcept { return screen_.pageSize().lines; }

    bool scrollingDisabled() const noexcept
    {
        // TODO: make configurable
        return screen_.isAlternateScreen();
    }

    // private fields
    //
    Screen<Cell>& screen_;
    ModifyEvent modified_;
    //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
    ScrollOffset scrollOffset_;
};

} // namespace terminal
