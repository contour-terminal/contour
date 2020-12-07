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

#include <algorithm>
#include <optional>

namespace terminal {

class Screen;

class Viewport {
  public:
    explicit Viewport(Screen& _screen) :
        screen_{ _screen }
    {}

    /// Returns the absolute offset where 0 is the top of scrollback buffer, and the maximum value the bottom of the screeen (plus history).
    std::optional<int> absoluteScrollOffset() const noexcept
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
    int relativeScrollOffset() const noexcept
    {
        return scrollOffset_.has_value()
            ? historyLineCount() - scrollOffset_.value()
            : 0;
    }

    bool isLineVisible(int _row) const noexcept
    {
        return crispy::ascending(1 - relativeScrollOffset(), _row, screenLineCount() - relativeScrollOffset());
    }

    bool scrollUp(int _numLines)
    {
        if (_numLines <= 0)
            return false;

        auto const newOffset = std::max(absoluteScrollOffset().value_or(historyLineCount()) - _numLines, 0);
        return scrollToAbsolute(newOffset);
    }

    bool scrollDown(int _numLines)
    {
        if (_numLines <= 0)
            return false;

        auto const newOffset = absoluteScrollOffset().value_or(historyLineCount()) + _numLines;
        return scrollToAbsolute(newOffset);
    }

    bool scrollToTop()
    {
        if (absoluteScrollOffset() != 0)
            return scrollToAbsolute(0);
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
        scrollOffset_.reset();
        return true;
    }

    bool scrollToAbsolute(int _absoluteScrollOffset)
    {
        if (scrollingDisabled())
            return false;

        if (0 <= _absoluteScrollOffset && _absoluteScrollOffset < historyLineCount())
        {
            scrollOffset_.emplace(_absoluteScrollOffset);
            return true;
        }

        if (_absoluteScrollOffset >= historyLineCount())
            return forceScrollToBottom();

        return false;
    }

    bool scrollMarkUp()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerBackward(absoluteScrollOffset().value_or(historyLineCount()));
        if (newScrollOffset.has_value())
            return scrollToAbsolute(newScrollOffset.value());

        return false;
    }

    bool scrollMarkDown()
    {
        if (scrollingDisabled())
            return false;

        auto const newScrollOffset = screen_.findMarkerForward(absoluteScrollOffset().value_or(historyLineCount()));

        if (newScrollOffset.has_value())
            return scrollToAbsolute(newScrollOffset.value());
        else
            return forceScrollToBottom();

        return true;
    }

  private:
    int historyLineCount() const noexcept { return screen_.historyLineCount(); }
    int screenLineCount() const noexcept { return screen_.size().height; }

    bool scrollingDisabled() const noexcept
    {
        // TODO: make configurable
        return screen_.isAlternateScreen();
    }

  private:
    Screen& screen_;
    std::optional<int> scrollOffset_; //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
};

}
