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

class Viewport
{
  public:
#if defined(CONTOUR_LOG_VIEWPORT)
    static auto inline Log = logstore::Category("vt.viewport", "Logs viewport details.");
#endif

    using ModifyEvent = std::function<void()>;

    explicit Viewport(Terminal& term, ModifyEvent _onModify = {}):
        terminal_ { term }, modified_ { _onModify ? std::move(_onModify) : []() {
        } }
    {
    }

    [[nodiscard]] ScrollOffset scrollOffset() const noexcept { return scrollOffset_; }

    /// Tests if the viewport has been moved(/scrolled) off its main view position.
    ///
    /// @retval true viewport has been moved/scrolled off its main view position.
    /// @retval false viewport has NOT been moved/scrolled and is still located at its main view position.
    [[nodiscard]] bool scrolled() const noexcept { return scrollOffset_.value != 0; }

    [[nodiscard]] bool isLineVisible(LineOffset _line) const noexcept
    {
        auto const a = -scrollOffset_.as<int>();
        auto const b = _line.as<int>();
        auto const c = unbox<int>(screenLineCount()) - scrollOffset_.as<int>();
        return a <= b && b < c;
    }

    bool scrollUp(LineCount _numLines);
    bool scrollDown(LineCount _numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool forceScrollToBottom();
    bool scrollTo(ScrollOffset _offset);
    bool scrollMarkUp();
    bool scrollMarkDown();

    /// Ensures given line is visible by optionally scrolling the
    /// screen's viewport up or down in order to make that line visible.
    ///
    /// If the line is already visible, no scrolling is applied.
    bool makeVisible(LineOffset line);

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
    [[nodiscard]] LineCount historyLineCount() const noexcept;
    [[nodiscard]] LineCount screenLineCount() const noexcept;
    [[nodiscard]] bool scrollingDisabled() const noexcept;

    // private fields
    //
    Terminal& terminal_;
    ModifyEvent modified_;
    //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
    ScrollOffset scrollOffset_;
};

} // namespace terminal
