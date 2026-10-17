// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/LinkDecoration.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/StickyHeaderKind.hpp>
#include <vtbackend/core/TextScale.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/grid/Line.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

struct RenderAttributes
{
    RGBColor foregroundColor {};
    RGBColor backgroundColor {};
    RGBColor decorationColor {};
    CellFlags flags {};
    LineFlags lineFlags = LineFlag::None;
};

/// Blends each color channel of @p attrs toward @p target by @p t.
/// At t=0 the colors remain unchanged; at t=1 every channel equals @p target.
///
/// @param attrs   render attributes modified in place (foreground, background, decoration).
/// @param target  the color each channel converges to as @p t approaches 1.
/// @param t       interpolation factor in [0, 1].
constexpr void blendAttributesTo(RenderAttributes& attrs, RGBColor target, float t) noexcept
{
    attrs.foregroundColor = mixColor(attrs.foregroundColor, target, t);
    attrs.backgroundColor = mixColor(attrs.backgroundColor, target, t);
    attrs.decorationColor = mixColor(attrs.decorationColor, target, t);
}

/// Blends each color channel of @p attrs from @p source toward the current value by @p t.
/// At t=0 every channel equals @p source; at t=1 the colors remain unchanged.
///
/// @param attrs   render attributes modified in place (foreground, background, decoration).
/// @param source  the color each channel starts from when @p t is 0.
/// @param t       interpolation factor in [0, 1].
constexpr void blendAttributesFrom(RenderAttributes& attrs, RGBColor source, float t) noexcept
{
    attrs.foregroundColor = mixColor(source, attrs.foregroundColor, t);
    attrs.backgroundColor = mixColor(source, attrs.backgroundColor, t);
    attrs.decorationColor = mixColor(source, attrs.decorationColor, t);
}

/**
 * Renderable representation of a grid cell with color-altering pre-applied and
 * additional information for cell ranges that can be text-shaped together.
 */
struct RenderCell
{
    std::u32string codepoints;
    std::shared_ptr<ImageFragment> image;
    CellLocation position;
    RenderAttributes attributes;
    uint8_t width = 1;

    /// How this cell's glyph is sized and placed, and which row of its block it draws (kitty text
    /// sizing protocol, `OSC 66`). Ordinary text leaves it at its default.
    GlyphSizing sizing {};

    /// Whether, and how, this cell is drawn as part of a link (OSC 8 or detected).
    LinkDecorationRole linkRole = LinkDecorationRole::None;

    bool groupStart = false;
    bool groupEnd = false;
};

/**
 * Renderable representation of a grid line with monochrome SGR styling.
 */
struct RenderLine
{
    std::u32string text;
    LineOffset lineOffset;
    ColumnCount usedColumns;
    ColumnCount displayWidth;
    RenderAttributes textAttributes;
    RenderAttributes fillAttributes;
    LineFlags flags = LineFlag::None;
};

/// One cell of the gutter: a character drawn in the strip left of the grid.
///
/// A cell of its own rather than a RenderCell in @ref RenderBuffer::cells, deliberately: the gutter sits
/// at NEGATIVE columns, outside the page entirely, and everything that walks the cells -- selection,
/// hit-testing, the accessibility bridge, screenshots -- is entitled to assume a column is on the page.
/// Keeping the two apart means none of them has to learn otherwise.
struct RenderGutterCell
{
    LineOffset lineOffset;       ///< The screen row this cell belongs to.
    char32_t codepoint {};       ///< The character.
    RenderAttributes attributes; ///< Its colors.

    /// The column it is drawn at, relative to grid column 0: -1 is the cell beside the grid, and a
    /// gutter N columns wide spans -N .. -1.
    ColumnOffset column { -1 };
};

/// The sticky command header drawn over the main page's first row.
///
/// Beside @ref RenderBuffer::cells rather than in it, for the reason RenderGutterCell gives: everything
/// that walks the cells -- selection, hit-testing, the accessibility bridge -- describes the GRID, and
/// the header repeats a row that is still there, underneath it.
///
/// Laid out whole by the terminal, the chip included, so the renderer draws it without deciding anything:
/// the band, the cells, the chip, the separator.
///
/// @see Terminal::fillStickyHeader
struct RenderStickyHeader
{
    StickyHeaderKind kind = StickyHeaderKind::Command; ///< The command's input row, or the placeholder.
    CommandBlockId block {};                           ///< The block it names; a press on it jumps there.
    std::vector<RenderCell> cells {}; ///< Its text, already on the row it covers, short of the chip.
    /// The right-aligned status -- `✓ 3s`, `✗ 2 1m 04s`, `running 12s` -- already in its columns.
    std::vector<RenderCell> chip {};
    RGBColor background {}; ///< The band's colour.
    RGBColor separator {};  ///< The one-pixel line along the band's bottom edge.
};

/// Lays @p text out as render cells from @p origin rightwards, appended to @p output, for host-drawn text
/// that has no grid row of its own: the sticky header's chip and placeholder, a collapsed fold's label.
///
/// One cell per glyph, as wide as the glyph; a zero-width codepoint (a combining mark, a variation
/// selector) joins the cell before it. A glyph that would reach @p end is dropped whole, and so is
/// everything after it. The cells appended form one shaping group.
///
/// Appended rather than returned, so a frame lays its overlays out into storage RenderBuffer::clear()
/// keeps, and a frame like the last allocates nothing.
///
/// @param output Where the cells go, after whatever it already holds.
/// @param text The text to lay out.
/// @param attributes Every cell's colours and flags.
/// @param origin Where the first cell goes.
/// @param end The first column the text may not reach.
/// @return The first column after the last cell placed; @p origin's column when nothing fits.
ColumnOffset layoutRenderText(std::vector<RenderCell>& output,
                              std::u32string_view text,
                              RenderAttributes const& attributes,
                              CellLocation origin,
                              ColumnOffset end);

/// Marks @p cells as one shaping group: the text shaper takes them as a single run, first to last.
/// @param cells The cells, left to right; none is marked when it is empty.
void markShapingGroup(std::span<RenderCell> cells) noexcept;

struct RenderCursor
{
    CellLocation position;
    CursorShape shape;
    int width = 1;
    /// Screen position the cursor is animating from: between two cells where the motion took over part of the
    /// way along another, as the cursor was drawn there (@see lerpCellLocation for where that holds).
    std::optional<FractionalCellLocation> animateFrom {};
    float animationProgress = 1.0f;              ///< 0.0 = at animateFrom, 1.0 = at position.
    RGBColor cursorColor {};                     ///< Resolved cursor color at target position.
    std::optional<RGBColor> animateFromColor {}; ///< Cursor color at animation source position.
};

/// A status line whose bar spans the window's whole width: the gutter and the page margins beside it, too.
///
/// Its background reaches the window's edges. Its text starts at the gutter's left edge: the status line's
/// cells are numbered from its own column 0 as every row's are, and drawn @ref leadingColumns columns left of
/// the page's -- shifted as a whole by the renderer, rather than written at negative columns, which nothing
/// that walks @ref RenderBuffer::cells expects.
struct RenderStatusBar
{
    LineOffset lineOffset;          ///< Its first row.
    LineCount lines;                ///< How many rows it takes.
    RGBColor background;            ///< What the bar is filled with.
    StatusDisplayPosition position; ///< The edge it sits on, which it reaches past the margin there too.
    ColumnCount leadingColumns {};  ///< How far left of the page's column 0 its own column 0 is drawn.
};

/// A row flashed where a jump landed -- a press on the sticky header (@see
/// Terminal::scrollToCommandBlockHead) -- so the eye finds the line the view moved to: a band across the
/// page's columns, under the row's text, fading out.
struct RenderLineFlash
{
    LineOffset lineOffset; ///< The screen row, as RenderCell::position.line counts them.
    RGBColor color;        ///< The band's colour.
    float alpha = 0.0f;    ///< Its opacity on this frame, from Terminal's peak down to nothing.
};

struct RenderBuffer
{
    std::vector<RenderCell> cells {};
    std::vector<RenderLine> lines {};
    std::vector<RenderGutterCell> gutter {};

    /// The indicator status line's bar, edge to edge. Absent for a host-writable status line: that one is
    /// the application's, gets no wider than the main page, and leaves the strip beside it to the default
    /// background.
    std::optional<RenderStatusBar> statusBar {};

    /// Render-only text the terminal lays over the page -- a collapsed fold's `⋯ 1,234 lines` -- already
    /// laid out (@see layoutRenderText), one shaping group per annotation.
    ///
    /// Not in @ref cells, for the gutter's reason: selection, copying and the accessibility bridge walk
    /// those, and an annotation is something to read, never something to select.
    std::vector<RenderCell> annotations {};

    std::optional<RenderCursor> cursor {};

    /// The sticky command header covering the main page's first row on this frame, if any.
    std::optional<RenderStickyHeader> stickyHeader {};

    /// The row a jump just landed on, while its flash fades; absent otherwise.
    std::optional<RenderLineFlash> lineFlash {};

    uint64_t frameID {};

    /// Starts this frame's sticky header on the storage the last one left behind, so a header shown frame
    /// after frame allocates nothing -- as @ref cells, whose capacity clear() keeps, do not.
    /// @return The header, its cells and chip empty, for the caller to fill in.
    RenderStickyHeader& showStickyHeader()
    {
        auto& header = stickyHeader.emplace(std::move(_retiredStickyHeader));
        header.cells.clear();
        header.chip.clear();
        return header;
    }

    void clear()
    {
        cells.clear();
        lines.clear();
        gutter.clear();
        statusBar.reset();
        annotations.clear();
        cursor.reset();
        lineFlash.reset();
        if (stickyHeader)
        {
            _retiredStickyHeader = std::move(*stickyHeader);
            stickyHeader.reset();
        }
    }

  private:
    /// The last header clear() took off the frame, kept for its cells' storage (@see showStickyHeader).
    RenderStickyHeader _retiredStickyHeader {};
};

/// Lock-guarded handle to a read-only RenderBuffer object: holds the buffer's reader lock for as long as it
/// lives. Move-only, as the lock is: a copy would unlock it a second time.
///
/// @see RenderBuffer
struct RenderBufferRef
{
    /// Takes @p lock, which the reference holds until it goes.
    /// @param buf The buffer to read.
    /// @param lock The buffer's reader lock.
    RenderBufferRef(RenderBuffer const& buf, std::mutex& lock): _buffer { buf }, _lock { lock } {}

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    /// Takes @p lock, which the reference holds until it goes, and calls @p onRelease once it has let go.
    /// @param buf The buffer to read.
    /// @param lock The buffer's reader lock.
    /// @param onRelease Called on the thread letting go, after the unlock; an empty one is not called.
    RenderBufferRef(RenderBuffer const& buf, std::mutex& lock, std::function<void()> const& onRelease):
        _buffer { buf }, _lock { lock }, _onRelease { &onRelease }
    {
    }
#endif

    // The same in every build, though only a passive build's destructor does more than unlock.
    RenderBufferRef(RenderBufferRef&&) noexcept = default;
    RenderBufferRef(RenderBufferRef const&) = delete;
    /// Not assigned: a passive build's reference calls back as it lets go, which assigning over it would
    /// skip.
    RenderBufferRef& operator=(RenderBufferRef&&) = delete;
    RenderBufferRef& operator=(RenderBufferRef const&) = delete;

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    ~RenderBufferRef()
    {
        if (!_lock.owns_lock())
            return;
        _lock.unlock();
        if (_onRelease != nullptr && *_onRelease)
            (*_onRelease)();
    }
#else
    ~RenderBufferRef() = default;
#endif

    [[nodiscard]] RenderBuffer const& get() const noexcept { return _buffer.get(); }

  private:
    std::reference_wrapper<RenderBuffer const> _buffer;
    std::unique_lock<std::mutex> _lock;
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    std::function<void()> const* _onRelease = nullptr; ///< What to call once the lock is let go; may be null.
#endif
};

/// Reflects the current state of a RenderDoubleBuffer object.
///
enum class RenderBufferState : uint8_t
{
    WaitingForRefresh,
    RefreshBuffersAndTrySwap,
    TrySwapBuffers
};

constexpr std::string_view toString(RenderBufferState state) noexcept
{
    switch (state)
    {
        case RenderBufferState::WaitingForRefresh: return "WaitingForRefresh";
        case RenderBufferState::RefreshBuffersAndTrySwap: return "RefreshBuffersAndTrySwap";
        case RenderBufferState::TrySwapBuffers: return "TrySwapBuffers";
    }
    return "INVALID";
}

struct RenderDoubleBuffer
{
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
    RenderDoubleBuffer() = default;

    /// @param onFrontBufferReleased Called on a reader's thread each time it lets go of the front buffer
    ///        (@see RenderBufferRef), without any lock held: a frame filled while the reader held the front
    ///        buffer can be swapped in now.
    explicit RenderDoubleBuffer(std::function<void()> onFrontBufferReleased):
        _onFrontBufferReleased { std::move(onFrontBufferReleased) }
    {
    }
#endif

    std::mutex mutable readerLock;
    std::atomic<size_t> currentBackBufferIndex = 0;

    /// The block the sticky header of the front buffer names; the zero id when that frame drew none.
    ///
    /// Published by swapBuffers(), with the frame it describes: a fill that is never swapped in -- one that
    /// lost the race for the front buffer, or a throwaway rendered for a crossfade -- leaves it alone.
    std::atomic<CommandBlockId> frontStickyHeaderBlock {};

    std::array<RenderBuffer, 2> buffers {};
    std::atomic<RenderBufferState> state = RenderBufferState::WaitingForRefresh;
    /// When the front buffer was last swapped in. Atomic: set by whichever thread swaps, and read without a
    /// lock as each thread decides whether a frame is due.
    std::atomic<std::chrono::steady_clock::time_point> lastUpdate {};

    RenderBuffer& backBuffer() noexcept { return buffers[currentBackBufferIndex]; }

    RenderBufferRef frontBuffer() const
    {
        // if (state == RenderBufferState::TrySwapBuffers)
        //     const_cast<RenderDoubleBuffer*>(this)->swapBuffers(lastUpdate);
        RenderBuffer const& frontBuffer = buffers.at((currentBackBufferIndex + 1) % 2);
#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
        return { frontBuffer, readerLock, _onFrontBufferReleased };
#else
        return { frontBuffer, readerLock };
#endif
    }

    void clear() { backBuffer().clear(); }

    // Swaps front with back buffer. May only be invoked by the writer thread.
    bool swapBuffers(std::chrono::steady_clock::time_point now) noexcept;

#ifdef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
  private:
    std::function<void()> _onFrontBufferReleased; ///< @see RenderDoubleBuffer(std::function<void()>)
#endif
};

} // namespace vtbackend
