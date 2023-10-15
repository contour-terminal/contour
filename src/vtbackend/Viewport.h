// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Screen.h>
#include <vtbackend/primitives.h>

#include <crispy/logstore.h>

#include <gsl/pointers>

namespace vtbackend
{

// #define CONTOUR_LOG_VIEWPORT 1

class Viewport
{
  public:
    static auto inline const viewportLog = logstore::category("vt.viewport", "Logs viewport details.");

    using ModifyEvent = std::function<void()>;

    explicit Viewport(Terminal& term, ModifyEvent onModify = {}):
        _terminal { &term }, _modified { onModify ? std::move(onModify) : []() {
        } }
    {
    }

    // Configures the vim-like `scrolloff` feature.
    void setScrollOff(LineCount count) noexcept { _scrollOff = count; }

    [[nodiscard]] LineCount scrollOff() const noexcept { return _scrollOff; }

    [[nodiscard]] ScrollOffset scrollOffset() const noexcept { return _scrollOffset; }

    /// Tests if the viewport has been moved(/scrolled) off its main view position.
    ///
    /// @retval true viewport has been moved/scrolled off its main view position.
    /// @retval false viewport has NOT been moved/scrolled and is still located at its main view position.
    [[nodiscard]] bool scrolled() const noexcept { return _scrollOffset.value != 0; }

    [[nodiscard]] bool isLineVisible(LineOffset line) const noexcept
    {
        auto const a = -_scrollOffset.as<int>();
        auto const b = line.as<int>();
        auto const c = unbox(screenLineCount()) - _scrollOffset.as<int>();
        return a <= b && b < c;
    }

    bool scrollUp(LineCount numLines);
    bool scrollDown(LineCount numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool forceScrollToBottom();
    bool scrollTo(ScrollOffset offset);
    bool scrollMarkUp();
    bool scrollMarkDown();

    /// Ensures given line is visible by optionally scrolling the
    /// screen's viewport up or down in order to make that line visible.
    ///
    /// If the line is already visible, no scrolling is applied.
    bool makeVisible(LineOffset line);

    bool makeVisibleWithinSafeArea(LineOffset line);
    bool makeVisibleWithinSafeArea(LineOffset line, LineCount paddingLines);

    CellLocation clampCellLocation(CellLocation const& location) const noexcept;

    /// Translates a screen coordinate to a Grid-coordinate by applying
    /// the scroll-offset to it.
    constexpr CellLocation translateScreenToGridCoordinate(CellLocation p) const noexcept
    {
        return CellLocation {
            p.line - boxed_cast<LineOffset>(_scrollOffset),
            p.column,
        };
    }

    constexpr CellLocation translateGridToScreenCoordinate(CellLocation p) const noexcept
    {
        return CellLocation {
            p.line + boxed_cast<LineOffset>(_scrollOffset),
            p.column,
        };
    }

    [[nodiscard]] constexpr LineOffset translateGridToScreenCoordinate(LineOffset p) const noexcept
    {
        return p + boxed_cast<LineOffset>(_scrollOffset);
    }

  private:
    [[nodiscard]] LineCount historyLineCount() const noexcept;
    [[nodiscard]] LineCount screenLineCount() const noexcept;
    [[nodiscard]] bool scrollingDisabled() const noexcept;

    // private fields
    //
    gsl::not_null<Terminal*> _terminal;
    ModifyEvent _modified;
    //!< scroll offset relative to scroll top (0) or nullopt if not scrolled into history
    ScrollOffset _scrollOffset;

    LineCount _scrollOff = LineCount(8);
};

} // namespace vtbackend
