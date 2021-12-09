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

#include <algorithm>
#include <optional>

namespace terminal {

class Screen;

class Viewport
{
  public:
    using ModifyEvent = std::function<void()>;

    explicit Viewport(Screen& _screen, ModifyEvent _onModify = {}) :
        screen_{ _screen },
        modified_{ _onModify ? std::move(_onModify) : []() {} }
    {}

    /// Returns the absolute offset where 0 is the top of scrollback buffer, and the maximum value the bottom of the screeen (plus history).
    std::optional<StaticScrollbackPosition> absoluteScrollOffset() const noexcept
    {
        return scrollOffset_;
    }

    /// Tests if the viewport has been moved(/scrolled) off its main view position.
    ///
    /// @retval true viewport has been moved/scrolled off its main view position.
    /// @retval false viewport has NOT been moved/scrolled and is still located at its main view position.
    bool scrolled() const noexcept
    {
        return scrollOffset_.has_value();
    }

    /// @returns scroll offset relative to the main screen buffer
    RelativeScrollbackPosition relativeScrollOffset() const noexcept
    {
        if (!scrollOffset_)
            return RelativeScrollbackPosition{0};

        return RelativeScrollbackPosition::cast_from(
            historyLineCount().as<int>() - scrollOffset_->as<int>()
        );
    }

    bool isLineVisible(int _row) const noexcept
    {
        return crispy::ascending(
            long{1} - *relativeScrollOffset(),
            static_cast<long>(_row),
            static_cast<long>(*screenLineCount()) - *relativeScrollOffset()
        );
    }

    bool scrollUp(LineCount _numLines)
    {
        auto const newOffset = std::max(
            absoluteScrollOffset().value_or(boxed_cast<StaticScrollbackPosition>(historyLineCount())) - boxed_cast<StaticScrollbackPosition>(_numLines),
            StaticScrollbackPosition(0)
        );
        return scrollToAbsolute(newOffset);
    }

    bool scrollDown(LineCount _numLines)
    {
        auto const newOffset =
            absoluteScrollOffset().value_or(boxed_cast<StaticScrollbackPosition>(historyLineCount()))
          + boxed_cast<StaticScrollbackPosition>(_numLines);

        return scrollToAbsolute(newOffset);
    }

    bool scrollToTop()
    {
        if (absoluteScrollOffset())
            return scrollToAbsolute(StaticScrollbackPosition{0});
        else
            return false;
    }

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

        scrollOffset_.reset();
        modified_();
        return true;
    }

    bool scrollToAbsolute(StaticScrollbackPosition _absoluteScrollOffset)
    {
        if (scrollingDisabled())
            return false;

        if (StaticScrollbackPosition{0} <= _absoluteScrollOffset && _absoluteScrollOffset < boxed_cast<StaticScrollbackPosition>(historyLineCount()))
        {
            scrollOffset_.emplace(_absoluteScrollOffset);
            modified_();
            return true;
        }

        if (_absoluteScrollOffset >= boxed_cast<StaticScrollbackPosition>(historyLineCount()))
            return forceScrollToBottom();

        return false;
    }

    bool scrollMarkUp()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerBackward(
            absoluteScrollOffset().
            value_or(historyLineCount().as<StaticScrollbackPosition>()).
            as<int>()
        );
        if (newScrollOffset.has_value())
            return scrollToAbsolute(StaticScrollbackPosition::cast_from(*newScrollOffset));

        return false;
    }

    bool scrollMarkDown()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerForward(
            static_cast<int>(*absoluteScrollOffset().value_or(boxed_cast<StaticScrollbackPosition>(historyLineCount())))
        );

        if (newScrollOffset.has_value())
            return scrollToAbsolute(StaticScrollbackPosition{static_cast<StaticScrollbackPosition::inner_type>(*newScrollOffset)});
        else
            return forceScrollToBottom();

        return true;
    }

    /// Translates a screen coordinate to a Grid-coordinate by applying
    /// the scroll-offset to it.
    Coordinate translateScreenToGridCoordinate(Coordinate p) const noexcept
    {
        return Coordinate{
            p.row - *relativeScrollOffset(),
            p.column,
        };
    }

  private:
    LineCount historyLineCount() const noexcept { return screen_.historyLineCount(); }
    LineCount screenLineCount() const noexcept { return screen_.size().lines; }

    bool scrollingDisabled() const noexcept
    {
        // TODO: make configurable
        return screen_.isAlternateScreen();
    }

    // private fields
    //
    Screen& screen_;
    ModifyEvent modified_;
    std::optional<StaticScrollbackPosition> scrollOffset_; //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
};

}
