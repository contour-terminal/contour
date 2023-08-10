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

#include <vtbackend/Screen.h>
#include <vtbackend/primitives.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <optional>

namespace terminal
{

// #define CONTOUR_LOG_VIEWPORT 1

class viewport
{
  public:
#if defined(CONTOUR_LOG_VIEWPORT)
    static auto inline const ViewportLog = logstore::Category("vt.viewport", "Logs viewport details.");
#endif

    using modify_event = std::function<void()>;

    explicit viewport(Terminal& term, modify_event onModify = {}):
        _terminal { term }, _modified { onModify ? std::move(onModify) : []() {
        } }
    {
    }

    // Configures the vim-like `scrolloff` feature.
    void setScrollOff(LineCount count) noexcept { _scrollOff = count; }

    [[nodiscard]] LineCount scrollOff() const noexcept { return _scrollOff; }

    [[nodiscard]] scroll_offset scrollOffset() const noexcept { return _scrollOffset; }

    /// Tests if the viewport has been moved(/scrolled) off its main view position.
    ///
    /// @retval true viewport has been moved/scrolled off its main view position.
    /// @retval false viewport has NOT been moved/scrolled and is still located at its main view position.
    [[nodiscard]] bool scrolled() const noexcept { return _scrollOffset.value != 0; }

    [[nodiscard]] bool isLineVisible(line_offset line) const noexcept
    {
        auto const a = -_scrollOffset.as<int>();
        auto const b = line.as<int>();
        auto const c = unbox<int>(screenLineCount()) - _scrollOffset.as<int>();
        return a <= b && b < c;
    }

    bool scrollUp(LineCount numLines);
    bool scrollDown(LineCount numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool forceScrollToBottom();
    bool scrollTo(scroll_offset offset);
    bool scrollMarkUp();
    bool scrollMarkDown();

    /// Ensures given line is visible by optionally scrolling the
    /// screen's viewport up or down in order to make that line visible.
    ///
    /// If the line is already visible, no scrolling is applied.
    bool makeVisible(line_offset line);

    bool makeVisibleWithinSafeArea(line_offset line);
    bool makeVisibleWithinSafeArea(line_offset line, LineCount paddingLines);

    /// Translates a screen coordinate to a Grid-coordinate by applying
    /// the scroll-offset to it.
    constexpr cell_location translateScreenToGridCoordinate(cell_location p) const noexcept
    {
        return cell_location {
            p.line - boxed_cast<line_offset>(_scrollOffset),
            p.column,
        };
    }

    constexpr cell_location translateGridToScreenCoordinate(cell_location p) const noexcept
    {
        return cell_location {
            p.line + boxed_cast<line_offset>(_scrollOffset),
            p.column,
        };
    }

    [[nodiscard]] constexpr line_offset translateGridToScreenCoordinate(line_offset p) const noexcept
    {
        return p + boxed_cast<line_offset>(_scrollOffset);
    }

  private:
    [[nodiscard]] LineCount historyLineCount() const noexcept;
    [[nodiscard]] LineCount screenLineCount() const noexcept;
    [[nodiscard]] bool scrollingDisabled() const noexcept;

    // private fields
    //
    Terminal& _terminal;
    modify_event _modified;
    //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
    scroll_offset _scrollOffset;

    LineCount _scrollOff = LineCount(8);
};

} // namespace terminal
