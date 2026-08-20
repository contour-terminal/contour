// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Screen.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/Folding.hpp>

#include <crispy/LogStore.hpp>

#include <gsl/pointers>

#include <optional>

namespace vtbackend
{

// #define CONTOUR_LOG_VIEWPORT 1

class Viewport
{
  public:
    static auto inline const ViewportLog = logstore::Category("vt.viewport", "Logs viewport details.");

    using ModifyEvent = std::function<void()>;

    explicit Viewport(Terminal& term, ModifyEvent onModify = {}):
        _terminal { &term }, _modified { onModify ? std::move(onModify) : []() {} }
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

    /// Whether the grid line @p line is currently drawn.
    ///
    /// Not an interval test once folds are in play: the rows on screen are a non-contiguous selection of
    /// grid rows, and a line INSIDE the drawn span may still be hidden. Goes through the same
    /// translation the render pass agrees with, so a hidden line reports invisible and a backfilled one
    /// reports visible.
    ///
    /// Not noexcept, where the plain interval test it replaced was: the projection is built lazily and
    /// building it allocates, so promising otherwise would turn an allocation failure into a
    /// std::terminate. The same goes for every query below that reads it.
    [[nodiscard]] bool isLineVisible(LineOffset line) const;

    /// How far the viewport can be scrolled up, in VISIBLE rows.
    ///
    /// The history depth less whatever collapsed folds hide: scrolling moves by rows the user can see,
    /// and a collapsed block has taken its output out of that count. Equal to historyLineCount() when
    /// nothing is collapsed, which is every existing caller's expectation.
    ///
    /// The single authority on the scroll bound. Everything that clamps a scroll position -- the
    /// smooth-scroll accumulator, the scrollbar's range, the post-fold re-clamp -- reads it from here,
    /// because a caller that bounded itself by the raw history instead would compute positions this
    /// class then rejects, freezing the viewport while the sub-cell offset kept moving.
    [[nodiscard]] LineCount scrollableLineCount() const;

    bool scrollUp(LineCount numLines);
    bool scrollDown(LineCount numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool forceScrollToBottom();
    bool scrollTo(ScrollOffset offset);

    /// Scrolls to the marked line above the viewport, bringing it to the top row.
    ///
    /// Skips marks a collapsed fold hides: there is no scroll offset that would show one, so stopping
    /// there would consume the key and move the viewport to whichever row stands in its place.
    ///
    /// @return Whether the viewport moved.
    bool scrollMarkUp();

    /// Scrolls to the marked line below the viewport, bringing it to the top row.
    ///
    /// Falls through to the bottom of the scrollback when there is no such mark, and skips hidden ones
    /// for the reason scrollMarkUp() gives.
    ///
    /// @return Whether the viewport moved.
    bool scrollMarkDown();

    /// Ensures given line is visible by optionally scrolling the
    /// screen's viewport up or down in order to make that line visible.
    ///
    /// If the line is already visible, no scrolling is applied.
    bool makeVisible(LineOffset line);

    bool makeVisibleWithinSafeArea(LineOffset line);
    bool makeVisibleWithinSafeArea(LineOffset line, LineCount paddingLines);

    /// Brings @p location inside the rows and columns the viewport currently draws.
    ///
    /// Not noexcept, and not an interval clamp against the scroll offset: @see isLineVisible() for the
    /// first, translateScreenToGridLine() for the second.
    [[nodiscard]] CellLocation clampCellLocation(CellLocation const& location) const;

    /// Translates a screen coordinate to a Grid-coordinate by applying
    /// the scroll-offset to it.
    ///
    /// With folds collapsed the mapping is no longer an addition: the rows on screen are a
    /// NON-contiguous selection of grid rows, and the one authority on which is the projection the
    /// render pass draws from. Every consumer -- selection, vi mode, the mouse protocol, hyperlink
    /// hover -- goes through here, so this is the single place that has to agree with it.
    [[nodiscard]] CellLocation translateScreenToGridCoordinate(CellLocation p) const
    {
        return CellLocation { .line = translateScreenToGridLine(p.line), .column = p.column };
    }

    [[nodiscard]] CellLocation translateGridToScreenCoordinate(CellLocation p) const
    {
        return CellLocation { .line = translateGridToScreenCoordinate(p.line), .column = p.column };
    }

    [[nodiscard]] LineOffset translateGridToScreenCoordinate(LineOffset p) const;

    /// The grid line the visible row @p line draws, honoring collapsed folds.
    [[nodiscard]] LineOffset translateScreenToGridLine(LineOffset line) const;

    /// The grid line the viewport's top row draws.
    ///
    /// Emphatically NOT -scrollOffset once anything is collapsed: that offset counts VISIBLE rows, so
    /// negating it names a grid line far down the page rather than the one at the top. Spelled once
    /// here because four callers wanted it and each open-coded translation is a chance to get that
    /// wrong again.
    [[nodiscard]] LineOffset topLine() const { return translateScreenToGridLine(LineOffset(0)); }

    /// Returns the sub-cell-height pixel offset for smooth scrolling.
    [[nodiscard]] float pixelOffset() const noexcept { return _pixelOffset; }

    /// Sets the sub-cell-height pixel offset for smooth scrolling.
    void setPixelOffset(float offset) noexcept { _pixelOffset = offset; }

    /// Resets the pixel offset to zero.
    void resetPixelOffset() noexcept { _pixelOffset = 0.0f; }

    /// Brings the scroll offset back inside the scrollable range, and reports whether it moved.
    ///
    /// Collapsing a fold takes its rows out of that range, so an offset that was legal a moment ago can
    /// now sit above the top. Nothing else corrects it: scrollTo() REJECTS an out-of-range request
    /// rather than repairing an offset already stored, so without this the viewport stays parked
    /// somewhere it can no longer be scrolled away from.
    bool clampScrollOffset();

  private:
    /// The scroll offset that puts grid line @p line on the viewport's top row, clamped to what the
    /// viewport can actually reach.
    ///
    /// The inverse of translateScreenToGridLine(LineOffset(0)), and emphatically not a negation: a
    /// scroll offset counts VISIBLE rows, so with collapsed folds in between it is not the grid
    /// distance at all. Negating instead asks for an offset that counts the hidden rows too, which
    /// scrollTo() then rejects as out of bounds -- silently, since the action reports success either
    /// way.
    ///
    /// @param line The grid line to bring to the top.
    /// @return The offset to hand scrollTo().
    [[nodiscard]] ScrollOffset scrollOffsetForTopLine(LineOffset line) const;

    /// The nearest marked grid line in @p direction that no collapsed fold hides.
    ///
    /// @param start The grid line to search from, exclusive.
    /// @param direction Which way to search.
    /// @return The marked line, or nullopt when that direction holds none.
    [[nodiscard]] std::optional<LineOffset> findVisibleMarker(LineOffset start,
                                                              VerticalDirection direction) const;

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

    /// Sub-cell-height pixel offset for smooth scrolling.
    float _pixelOffset = 0.0f;
};

} // namespace vtbackend
