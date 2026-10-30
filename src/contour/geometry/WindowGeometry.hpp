// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <vtrasterizer/GridMetrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace contour::geometry
{

/// The single source of page<->pixel window-geometry math.
///
/// Every conversion between terminal grid cells, device pixels, logical (device-independent) pixels and
/// window sizes lives here — nowhere else. The load-bearing rounding law, relied upon by every caller pair:
///
///   - availability is FLOORED  (window->grid: never claim a device pixel the surface does not have),
///   - requirement  is CEILED   (grid->window: never request fewer pixels than the grid needs),
///
/// so that a window sized for a grid always fits exactly that grid:
/// `pageSizeForPixels(requiredPixelsForPage(p)) == p` for every cell size, margin and content scale
/// (unit-tested as the anti-oscillation invariant). All arithmetic is signed; degenerate inputs clamp
/// instead of wrapping (the historic ImageSize unsigned-underflow class).
///
/// Kept free of Qt and I/O so the decision logic is unit-testable (pattern: display/ScissorRect.h).

/// Symmetric minimum padding around the terminal grid, applied on BOTH sides of each axis.
/// The unit is whatever the caller works in; each function below documents whether it expects
/// device or logical pixels.
struct Margins
{
    int horizontal = 0; ///< Padding applied left AND right of the grid.
    int vertical = 0;   ///< Padding applied above AND below the grid.
};

/// The width of the drawable gutter reserved to the LEFT of the grid, inside the margin.
///
/// Asymmetric, which is why it is not part of @ref Margins: it is a strip the renderer draws the
/// gutter's timestamps, line numbers and block column into, not padding, and it exists on one side only.
/// It is subtracted from the width available to cells and added to the width a page requires, so a page
/// fitted WITH a gutter reports fewer columns to the shell than the same window without one -- which is
/// the honest answer, those columns genuinely not being there. A reserved gutter begins with a blank
/// lead-in (@see gutterLeadIn): the renderer sets the gutter's columns against the grid, so whatever the
/// strip holds beyond them stays empty at its left edge.
///
/// Every function below takes it last and defaults it to zero, so a caller that reserves no gutter gets
/// exactly the arithmetic this header had before one existed.
using GutterWidth = int;

/// How far a frameless window's own resize border reaches in from each edge, in logical pixels.
///
/// The border is a set of invisible hit zones laid over the whole window, above everything else
/// (ResizeBorder.qml, which reads this through UiStyleProvider), and a press inside one starts a window
/// resize instead of reaching what is drawn beneath it.
inline constexpr int ResizeBorderThickness = 6;

/// Window chrome OUTSIDE the terminal content area (e.g. the QML title/tab bar), in logical pixels,
/// DECLARED by the UI layer (Main.qml knows its layout). Never measured from live window-minus-item
/// deltas — such measurements are transiently wrong during relayout and structurally wrong in splits.
struct Chrome
{
    int width = 0;  ///< Horizontal chrome (structurally 0 in the current layout).
    int height = 0; ///< Vertical chrome (title/tab bar height; 0 when hidden).
};

/// A width/height pair in logical (device-independent) pixels.
struct LogicalSize
{
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(LogicalSize const&) const noexcept = default;
};

/// WM size hints in logical pixels, mapped 1:1 onto QWindow::setMinimumSize / setBaseSize /
/// setSizeIncrement. The @c minimum is honored everywhere; the @c base / @c increment pair drives the
/// character-cell resize grid on X11 (WM_NORMAL_HINTS), is ignored by Qt's Windows backend and on
/// Wayland (xdg-shell has no increment concept), and is actively harmful on macOS — see @ref
/// SizeHintPolicy and @ref sizeHintPolicyFor.
struct SizeHints
{
    LogicalSize minimum;   ///< Smallest window the WM should allow.
    LogicalSize base;      ///< Fixed part not participating in the increment grid (margins + chrome).
    LogicalSize increment; ///< Interactive-resize step: one logical cell.

    [[nodiscard]] constexpr bool operator==(SizeHints const&) const noexcept = default;
};

/// Device-pixel height constraints for a window with a single visible terminal pane.
struct RowResizeGrid
{
    int cellHeight = 0;    ///< Device pixels in one terminal row, including line spacing.
    int baseHeight = 0;    ///< Chrome and vertical margins, outside the rows.
    int minimumHeight = 0; ///< Minimum client height allowed by the window.
    int chromeHeight = 0;  ///< Declared chrome in logical pixels, before device conversion.
    double scale = 1.0;    ///< Window device-pixel ratio.
};

/// Snaps a proposed client height to the nearest whole row without going below the window minimum.
/// @param height Proposed client height in device pixels.
/// @param grid Current row metrics and fixed insets, in device pixels.
/// @return The snapped client height, or the original height if metrics are not ready.
[[nodiscard]] inline int snapHeightToRows(int height, RowResizeGrid grid) noexcept;

/// Windowing platform, as far as WM size-hint applicability is concerned. Data (not a scattered
/// `#if defined`) so @ref sizeHintPolicyFor is a pure, per-OS-testable function on any host.
enum class SizeHintPlatform : std::uint8_t
{
    Windows, ///< Qt stores the hints; optional native row snapping uses device-pixel metrics instead.
    MacOS,   ///< Cocoa: base/increment must NOT be set (Qt writes base into the NSWindow frame).
    Other,   ///< X11 honors them; Wayland silently ignores them. Either way, safe to set.
};

/// Which of a @ref SizeHints' three components a platform may safely apply to its QWindow.
struct SizeHintPolicy
{
    bool applyMinimum;   ///< Whether to call QWindow::setMinimumSize.
    bool applyBase;      ///< Whether to call QWindow::setBaseSize.
    bool applyIncrement; ///< Whether to call QWindow::setSizeIncrement.

    [[nodiscard]] constexpr bool operator==(SizeHintPolicy const&) const noexcept = default;
};

/// Decides which size hints @p platform may apply.
///
/// The minimum size is always safe. The base + increment pair is applied on every platform EXCEPT macOS:
/// Qt's Cocoa plugin implements @c QWindow::setBaseSize by writing the base straight into
/// `-[NSWindow setFrame:]`, so a small base (2*margins + chrome) HARD-RESIZES the freshly-mapped window
/// to that tiny frame — the window then shows title-bar-only (effectively invisible) and drives the
/// terminal grid to a degenerate size. macOS therefore gets @c minimum only; optional row snapping
/// is handled separately after a native live resize.
/// @param platform The windowing platform.
/// @return The per-component apply policy.
[[nodiscard]] constexpr SizeHintPolicy sizeHintPolicyFor(SizeHintPlatform platform) noexcept
{
    auto const isMac = platform == SizeHintPlatform::MacOS;
    return { .applyMinimum = true, .applyBase = !isMac, .applyIncrement = !isMac };
}

/// The windowing platform this binary was compiled for.
/// @return The compile-time @ref SizeHintPlatform.
[[nodiscard]] constexpr SizeHintPlatform currentSizeHintPlatform() noexcept
{
#ifdef _WIN32
    return SizeHintPlatform::Windows;
#elifdef __APPLE__
    return SizeHintPlatform::MacOS;
#else
    return SizeHintPlatform::Other;
#endif
}

/// Result of fitting a grid into a pixel area: the floored+clamped page and the margin placement the
/// renderer draws with. The sub-cell remainder is NOT part of the margin; it stays undrawn on the
/// right/bottom edge and shows as background-colored padding.
struct GridFit
{
    vtbackend::PageSize pageSize;           ///< Total page (main page + status line) that fits.
    vtrasterizer::PageMargin pageMargin {}; ///< Margin placement for the renderer (left/top/bottom).
};

/// The smallest total page size the WM minimum-size hint is derived from.
constexpr auto MinimumTotalPageSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(5), .columns = vtbackend::ColumnCount(10) };

namespace detail
{
    /// FP guard for the logical<->device roundtrip: quantization noise from a double multiply/divide must
    /// never eat a whole device pixel. floorScaled() adds it (an accidental `n - 1e-16` still floors to n),
    /// ceilScaled() subtracts it (an accidental `n + 1e-16` still ceils to n). Overshooting by one pixel is
    /// harmless padding; undershooting would break the floor-availability/ceil-requirement law.
    constexpr double RoundingSlack = 1e-6;

    /// floor(value * scale) for non-negative inputs, in constexpr-friendly form.
    [[nodiscard]] constexpr int floorScaled(double value, double scale) noexcept
    {
        auto const scaled = std::max(0.0, value * scale) + RoundingSlack;
        return static_cast<int>(scaled); // truncation == floor for non-negative values
    }

    /// ceil(value * scale) for non-negative inputs, in constexpr-friendly form: the requirement-side
    /// counterpart of floorScaled().
    [[nodiscard]] constexpr int ceilScaled(double value, double scale) noexcept
    {
        auto const scaled = std::max(0.0, value * scale) - RoundingSlack;
        auto const truncated = static_cast<int>(scaled);
        return scaled > static_cast<double>(truncated) ? truncated + 1 : truncated;
    }

    /// ceil(value / scale) for non-negative value and positive scale, in constexpr-friendly form.
    [[nodiscard]] constexpr int ceilUnscaled(double value, double scale) noexcept
    {
        auto const unscaled = std::max(0.0, value / scale) - RoundingSlack;
        auto const truncated = static_cast<int>(unscaled);
        return unscaled > static_cast<double>(truncated) ? truncated + 1 : truncated;
    }

    /// floor(value / scale) for non-negative value and positive scale, in constexpr-friendly form.
    ///
    /// The availability-side counterpart of ceilUnscaled(), and it adds the slack rather than
    /// subtracting it for the same reason floorScaled() does: an accidental `n - 1e-16` must still
    /// floor to n.
    [[nodiscard]] constexpr int floorUnscaled(double value, double scale) noexcept
    {
        auto const unscaled = std::max(0.0, value / scale) + RoundingSlack;
        return static_cast<int>(unscaled); // truncation == floor for non-negative values
    }

    /// The main-page row @p yDevicePx falls on, bounded neither way.
    ///
    /// Shared by mainPageRowAt() and mainPageRowNear() so that only their out-of-range policies differ:
    /// a second spelling of it is how a truncating hit-test came to sit beside a flooring one.
    ///
    /// @param yDevicePx      The y coordinate in device pixels, relative to the item's top edge.
    /// @param pageMarginTop  The renderer's grid origin, in device pixels.
    /// @param cellHeightPx   The cell height in device pixels; must be positive.
    /// @param mainPageTopRow The screen row the main page starts on.
    /// @return The main-page-relative row, which may lie outside the page.
    [[nodiscard]] constexpr int mainPageRowOf(int yDevicePx,
                                              int pageMarginTop,
                                              int cellHeightPx,
                                              int mainPageTopRow) noexcept
    {
        // Floor division, not truncation: a coordinate ABOVE the grid origin is negative, and truncating
        // it towards zero would fold the first row above the grid onto row 0.
        auto const offsetPx = yDevicePx - pageMarginTop;
        auto const screenRow =
            offsetPx >= 0 ? offsetPx / cellHeightPx : -(((-offsetPx) + cellHeightPx - 1) / cellHeightPx);

        return screenRow - mainPageTopRow;
    }
} // namespace detail

/// Scales symmetric margins from logical to device pixels.
/// @param logical Margins in logical pixels (as configured).
/// @param scale   Content scale (device pixels per logical pixel).
/// @return Margins in device pixels (truncated). The ONLY margin-scaling implementation.
[[nodiscard]] constexpr Margins scaled(Margins logical, double scale) noexcept
{
    return { .horizontal = static_cast<int>(logical.horizontal * scale),
             .vertical = static_cast<int>(logical.vertical * scale) };
}

/// Builds the physical row grid from the same declared margins and chrome used for window sizing.
/// @param cellHeight Cell height in device pixels, without rounding it to logical pixels.
/// @param margins Vertical padding in logical pixels.
/// @param scale Window device-pixel ratio.
/// @param chrome Declared window chrome in logical pixels.
/// @param minimumHeight Minimum client height in logical pixels.
/// @return Row metrics and fixed insets in device pixels.
[[nodiscard]] inline RowResizeGrid rowResizeGridFor(
    int cellHeight, Margins margins, double scale, Chrome chrome, int minimumHeight) noexcept
{
    return { .cellHeight = cellHeight,
             .baseHeight =
                 (2 * scaled(margins, scale).vertical) + static_cast<int>(std::round(chrome.height * scale)),
             .minimumHeight = detail::ceilScaled(minimumHeight, scale),
             .chromeHeight = chrome.height,
             .scale = scale };
}

/// Aligns a logical client height to the physical row grid, rounding up to a representable Qt size.
/// At fractional scale the nearest logical size can leave less than one logical pixel of slack.
/// @param height Proposed client height in logical pixels.
/// @param scale Device pixels per logical pixel.
/// @param grid Current physical row metrics, including chrome and margins.
/// @return The aligned logical height, or the proposal if metrics are unavailable.
[[nodiscard]] inline int snapLogicalHeightToRows(int height, double scale, RowResizeGrid grid) noexcept
{
    if (grid.cellHeight <= 0 || scale <= 0.0)
        return height;
    auto const chromeDevice = static_cast<int>(std::round(grid.chromeHeight * scale));
    auto const margins = grid.baseHeight - chromeDevice;
    auto const available = detail::floorScaled(std::max(0, height - grid.chromeHeight), scale);
    auto const minimumContent = std::max(0, grid.minimumHeight - chromeDevice - margins);
    auto const minimumRows = (minimumContent + grid.cellHeight - 1) / grid.cellHeight;
    auto const nearestRows = std::max(0, available - margins + (grid.cellHeight / 2)) / grid.cellHeight;
    return grid.chromeHeight
           + detail::ceilUnscaled(margins + (std::max(minimumRows, nearestRows) * grid.cellHeight), scale);
}

inline int snapHeightToRows(int height, RowResizeGrid grid) noexcept
{
    if (grid.cellHeight <= 0 || grid.scale <= 0.0)
        return height;
    auto const logicalHeight = static_cast<int>(std::round(height / grid.scale));
    return static_cast<int>(
        std::round(snapLogicalHeightToRows(logicalHeight, grid.scale, grid) * grid.scale));
}

/// The blank lead-in a reserved gutter keeps at its left edge, in device pixels: room enough for a
/// frameless window's resize border, so the block column starts where that border ends.
///
/// With no margin the gutter began at the window's edge, and its one-cell block column lay under the
/// border's left zone: pointing at a fold control showed a resize cursor, pressing it resized the window,
/// and the thin fold line itself was drawn hard against the edge. Kept whatever the window's frame -- a
/// gutter flush against a border is hard to see under any frame, and a split pane's gutter is flush against
/// its divider -- so that it costs no more than this one strip, and a title bar toggled on and off changes
/// no page size.
/// @param scale Content scale (device pixels per logical pixel).
/// @return ceil(ResizeBorderThickness * scale): a whole pixel short of it would leave the column's first
///         pixel inside the zone at a fractional scale.
[[nodiscard]] constexpr int gutterLeadIn(double scale) noexcept
{
    return detail::ceilScaled(ResizeBorderThickness, scale);
}

/// Computes how many whole cells fit into the given device-pixel area: floor((available - 2*margins) / cell).
///
/// FLOOR is the load-bearing contract: the grid never claims pixels the surface does not have; the sub-cell
/// remainder is rendered as padding. Degenerate inputs (margins exceeding the area, zero cell size) clamp to
/// a 1x1 page instead of wrapping.
/// @param availableDevicePx Total available area in device pixels.
/// @param cellSize          Cell size in device pixels.
/// @param marginsDevicePx   Symmetric minimum margins in device pixels.
/// @param gutterDevicePx    Gutter reserved left of the grid, in device pixels (@see GutterWidth).
/// @return The floored page size, at least 1x1.
[[nodiscard]] constexpr vtbackend::PageSize pageSizeForPixels(vtbackend::ImageSize availableDevicePx,
                                                              vtbackend::ImageSize cellSize,
                                                              Margins marginsDevicePx,
                                                              GutterWidth gutterDevicePx = 0) noexcept
{
    auto const usableWidth =
        std::max(0, unbox<int>(availableDevicePx.width) - (2 * marginsDevicePx.horizontal) - gutterDevicePx);
    auto const usableHeight =
        std::max(0, unbox<int>(availableDevicePx.height) - (2 * marginsDevicePx.vertical));
    auto const cellWidth = std::max(1, unbox<int>(cellSize.width));
    auto const cellHeight = std::max(1, unbox<int>(cellSize.height));

    return { .lines = vtbackend::LineCount(std::max(1, usableHeight / cellHeight)),
             .columns = vtbackend::ColumnCount(std::max(1, usableWidth / cellWidth)) };
}

/// The initial total page size for a newly spawned session.
///
/// Encodes the spawn-context rule: a new tab or split pane in an EXISTING window adopts that window's
/// currently-running page size (the window the user already sized is the authority — the profile's
/// configured @p profileDefault must not shrink it back). Only a newly-spawned window has no running
/// size to inherit, and there the profile default is honored. Kept pure (no Terminal/display access) so
/// the choice is unit-testable without a window.
/// @param runningPageSize The current focused session's total page size, if a session is already running
///                        in the target window; @c std::nullopt for a brand-new window.
/// @param profileDefault  The profile-configured initial page size (@c profile.terminalSize).
/// @return @p runningPageSize when present, otherwise @p profileDefault.
[[nodiscard]] constexpr vtbackend::PageSize initialPageSize(
    std::optional<vtbackend::PageSize> runningPageSize, vtbackend::PageSize profileDefault) noexcept
{
    return runningPageSize.value_or(profileDefault);
}

/// Device pixels required to show the given total page: cell * page + 2 * margins (CEIL side of the law:
/// exact integer math, no rounding loss).
/// @param totalPageSize   Total page size (main page + status line).
/// @param cellSize        Cell size in device pixels.
/// @param marginsDevicePx Symmetric margins in device pixels.
/// @param gutterDevicePx  Gutter reserved left of the grid, in device pixels (@see GutterWidth).
/// @return Required area in device pixels.
[[nodiscard]] constexpr vtbackend::ImageSize requiredPixelsForPage(vtbackend::PageSize totalPageSize,
                                                                   vtbackend::ImageSize cellSize,
                                                                   Margins marginsDevicePx,
                                                                   GutterWidth gutterDevicePx = 0) noexcept
{
    auto const width = (unbox<int>(cellSize.width) * unbox<int>(totalPageSize.columns))
                       + (2 * marginsDevicePx.horizontal) + gutterDevicePx;
    auto const height =
        (unbox<int>(cellSize.height) * unbox<int>(totalPageSize.lines)) + (2 * marginsDevicePx.vertical);
    return { .width = vtbackend::Width::cast_from(std::max(0, width)),
             .height = vtbackend::Height::cast_from(std::max(0, height)) };
}

/// Cell size (device pixels) -> the cell size applications are told about.
///
/// Applications derive their cell size as `ws_ypixel / ws_row` and size an image canvas from it, so this
/// is what decides how large a sixel an application draws. FLOORED, because a report of available space
/// is an availability (see the law in this header's overview): a cell rounded up would have the
/// application draw past the area it was promised.
/// @param cellDevicePx Cell size in device pixels, as the renderer works in.
/// @param scale        Device pixels per logical pixel to divide out; 1.0 reports device pixels as-is.
/// @return The cell size to report, at least 1x1 -- a zero cell would make the report undivisible.
[[nodiscard]] constexpr vtbackend::ImageSize reportedCellSize(vtbackend::ImageSize cellDevicePx,
                                                              double scale) noexcept
{
    if (!(scale > 0.0))
        return cellDevicePx; // a degenerate scale reports what the renderer uses, rather than nothing
    auto const unscale = [scale](auto value) {
        return std::max(1, detail::floorUnscaled(unbox<double>(value), scale));
    };
    return { .width = vtbackend::Width::cast_from(unscale(cellDevicePx.width)),
             .height = vtbackend::Height::cast_from(unscale(cellDevicePx.height)) };
}

/// The pixel size to report to applications for a page of @p totalPageSize.
///
/// Excludes margins deliberately, and that is the point: `Terminal::resizeScreen` derives the cell size
/// by dividing this by the page, so anything that is not a whole number of cells comes back as cell-size
/// error. requiredPixelsForPage() -- which *does* add margins -- is for sizing a window, not for telling
/// an application what a cell is; passing that here inflates the reported cell by the margins.
/// @param totalPageSize Total page size (main page + status line).
/// @param cellDevicePx  Cell size in device pixels.
/// @param scale         Device pixels per logical pixel; 1.0 reports device pixels as-is.
/// @return The extent to report, exactly `reportedCellSize(cellDevicePx, scale) * totalPageSize`.
[[nodiscard]] constexpr vtbackend::ImageSize reportedPixelsForPage(vtbackend::PageSize totalPageSize,
                                                                   vtbackend::ImageSize cellDevicePx,
                                                                   double scale) noexcept
{
    auto const cell = reportedCellSize(cellDevicePx, scale);
    auto const width = unbox<int>(cell.width) * unbox<int>(totalPageSize.columns);
    auto const height = unbox<int>(cell.height) * unbox<int>(totalPageSize.lines);
    return { .width = vtbackend::Width::cast_from(std::max(0, width)),
             .height = vtbackend::Height::cast_from(std::max(0, height)) };
}

/// A device-pixel extent expressed in the unit reported to applications.
///
/// For extents that are not a page of cells -- the image-canvas ceiling, above all. Every pixel figure an
/// application is told must be in one unit, or it cannot compare them: a ceiling left in device pixels
/// while the window reports logical tells the application it may draw an image twice the size of the
/// screen it is on. FLOORED for the same reason reportedCellSize() is: a report of available space is an
/// availability, and rounding one up promises space that is not there.
/// @param devicePx Extent in device pixels.
/// @param scale    Device pixels per reported pixel to divide out; 1.0 reports device pixels as-is.
/// @return The extent to report.
[[nodiscard]] constexpr vtbackend::ImageSize reportedPixels(vtbackend::ImageSize devicePx,
                                                            double scale) noexcept
{
    if (!(scale > 0.0))
        return devicePx; // a degenerate scale reports what the renderer uses, rather than nothing
    return { .width =
                 vtbackend::Width::cast_from(detail::floorUnscaled(unbox<double>(devicePx.width), scale)),
             .height =
                 vtbackend::Height::cast_from(detail::floorUnscaled(unbox<double>(devicePx.height), scale)) };
}

/// The renderer's grid origin for the given margins and gutter.
///
/// The one statement of where the grid starts: the gutter is folded INTO the left inset, because it is
/// asymmetric where the configured margin is not, and everything downstream reads that one number --
/// the mouse hit-test, the gutter hit-test (@see isInGutter), the renderer itself. A second, hand-built
/// PageMargin beside this one is how a grid origin comes to be short by exactly one gutter.
///
/// @param marginsDevicePx The configured margins in device pixels (@see scaled).
/// @param gutterDevicePx  Gutter reserved left of the grid, in device pixels (@see GutterWidth).
/// @param bottomDevicePx  The bottom inset; the configured vertical margin when omitted, which is what
///                        a page that has not yet been fitted to a surface has.
/// @return The page margin the renderer draws from.
[[nodiscard]] constexpr vtrasterizer::PageMargin pageMarginFor(
    Margins marginsDevicePx,
    GutterWidth gutterDevicePx = 0,
    std::optional<int> bottomDevicePx = std::nullopt) noexcept
{
    return { .left = marginsDevicePx.horizontal + gutterDevicePx,
             .top = marginsDevicePx.vertical,
             .bottom = bottomDevicePx.value_or(marginsDevicePx.vertical) };
}

/// Fits a grid into a device-pixel area: page = clamp(pageSizeForPixels(...)), margin placement computed
/// against the CLAMPED page so the renderer and the terminal can never disagree (below-minimum windows
/// previously computed margins for a page the terminal then silently enlarged).
///
/// Margin placement follows the historic computeMargin() semantics: left/top = configured margin,
/// bottom = leftover space capped at the configured margin — but clamped at 0 instead of wrapping when the
/// clamped page overflows the available area.
/// @param availableDevicePx  Total available area in device pixels.
/// @param cellSize           Cell size in device pixels.
/// @param marginsDevicePx    Symmetric minimum margins in device pixels.
/// @param clampTotalPageSize Injected total-page clamp; production passes
///                           `Terminal::clampedTotalPageSize` (status-line rule) so this header stays
///                           free of vtbackend::Terminal and tests can exercise the contract directly.
/// @param gutterDevicePx     Gutter reserved left of the grid, in device pixels (@see GutterWidth). It
///                           lands INSIDE the returned pageMargin.left, so every pixel<->cell
///                           conversion in the tree -- GridMetrics::map, the mouse hit-test, the
///                           accessibility bridge -- shifts the grid across by it for free, and the
///                           strip [horizontal, horizontal + gutter) is what the renderer draws into.
/// @return The fitted page and the renderer margin placement.
template <typename ClampFn>
    requires std::is_invocable_r_v<vtbackend::PageSize, ClampFn, vtbackend::PageSize>
[[nodiscard]] constexpr GridFit fitPageToPixels(vtbackend::ImageSize availableDevicePx,
                                                vtbackend::ImageSize cellSize,
                                                Margins marginsDevicePx,
                                                ClampFn&& clampTotalPageSize,
                                                GutterWidth gutterDevicePx = 0) noexcept
{
    auto const page = std::forward<ClampFn>(clampTotalPageSize)(
        pageSizeForPixels(availableDevicePx, cellSize, marginsDevicePx, gutterDevicePx));

    auto const usedHeight = unbox<int>(page.lines) * unbox<int>(cellSize.height);
    auto const top = marginsDevicePx.vertical;
    auto const leftover = unbox<int>(availableDevicePx.height) - usedHeight - top;
    auto const bottom = std::clamp(leftover, 0, marginsDevicePx.vertical);

    return { .pageSize = page, .pageMargin = pageMarginFor(marginsDevicePx, gutterDevicePx, bottom) };
}

/// The MAIN-PAGE row a device-pixel y coordinate falls on, if it falls on one at all.
///
/// Screen rows count from the top of everything the renderer draws, so row 0 is the status line when
/// one sits above the grid (@see vtbackend::Terminal::mainPageTopRow). Viewport's coordinate
/// translation, on the other hand, speaks main-page rows -- handing it a screen row shifts every
/// hit-test down by the height of that status line, which is exactly the bug this exists to prevent.
///
/// Returns nullopt rather than clamping when the coordinate is off the main page: a click on the
/// status line is not a click on the grid, and answering with the nearest grid row would act on a
/// cell the user did not point at.
///
/// Pure, so the mapping is testable without a window -- and stated once, so the gutter's
/// hit-test and the cell hit-test cannot drift apart.
///
/// @param yDevicePx       The y coordinate in device pixels, relative to the item's top edge, with
///                        any smooth-scroll offset already subtracted.
/// @param pageMarginTop   The renderer's grid origin (@c GridMetrics::pageMargin.top), device pixels.
/// @param cellHeightPx    The cell height in device pixels; must be positive.
/// @param mainPageTopRow  The screen row the main page starts on.
/// @param mainPageLines   How many lines the main page has.
/// @return The main-page-relative row, or nullopt when the coordinate is not over the main page.
[[nodiscard]] constexpr std::optional<int> mainPageRowAt(
    int yDevicePx, int pageMarginTop, int cellHeightPx, int mainPageTopRow, int mainPageLines) noexcept
{
    if (cellHeightPx <= 0 || mainPageLines <= 0)
        return std::nullopt;

    auto const row = detail::mainPageRowOf(yDevicePx, pageMarginTop, cellHeightPx, mainPageTopRow);
    if (row < 0 || row >= mainPageLines)
        return std::nullopt;

    return row;
}

/// The MAIN-PAGE row a device-pixel y coordinate falls on, or the nearest one when it falls off.
///
/// The clamping sibling of mainPageRowAt(), over the same arithmetic: a drag that leaves the grid must
/// keep naming its nearest row, because auto-scroll depends on that, where a click off the grid must not
/// be answered with a row at all. Only the out-of-range policy differs.
///
/// @param yDevicePx       The y coordinate in device pixels, relative to the item's top edge, with
///                        any smooth-scroll offset already subtracted.
/// @param pageMarginTop   The renderer's grid origin (@c GridMetrics::pageMargin.top), device pixels.
/// @param cellHeightPx    The cell height in device pixels; must be positive.
/// @param mainPageTopRow  The screen row the main page starts on.
/// @param mainPageLines   How many lines the main page has.
/// @return The main-page-relative row, clamped to [0, mainPageLines - 1].
[[nodiscard]] constexpr int mainPageRowNear(
    int yDevicePx, int pageMarginTop, int cellHeightPx, int mainPageTopRow, int mainPageLines) noexcept
{
    if (cellHeightPx <= 0 || mainPageLines <= 0)
        return 0;

    return std::clamp(
        detail::mainPageRowOf(yDevicePx, pageMarginTop, cellHeightPx, mainPageTopRow), 0, mainPageLines - 1);
}

/// The gutter's parts, in device pixels: the blank lead-in against the pane's left edge, the cell columns,
/// then the block rail's strip ending where the grid begins. Their sum is the GutterWidth a page is fitted
/// with.
struct GutterExtent
{
    GutterWidth leadIn = 0;  ///< The lead-in (@see gutterLeadIn); 0 when there is no gutter.
    GutterWidth columns = 0; ///< The cell columns themselves; 0 when the gutter has none.
    GutterWidth rail = 0;    ///< The block rail's strip after the cells; 0 when there is none.
};

/// Whether the device-pixel x coordinate @p xDevicePx falls inside the gutter, its lead-in included.
///
/// The gutter is the strip [pageMarginLeft - rail - columns - lead-in, pageMarginLeft): fitPageToPixels folds
/// its width INTO pageMargin.left, so the strip is exactly what that inset gained, and column 0 begins where
/// it ends. Pure, so the hit-test is testable without a window -- and so the one place that decides where
/// the gutter IS is the same header that decided how wide to make it.
///
/// @param xDevicePx       The x coordinate, in device pixels, relative to the item's left edge.
/// @param pageMarginLeft  The renderer's grid origin (@c GridMetrics::pageMargin.left), in device pixels.
/// @param extent          The reserved gutter; neither columns nor a rail means there is no gutter.
/// @return Whether the coordinate is over the gutter rather than over a cell or the margin left of it.
[[nodiscard]] constexpr bool isInGutter(int xDevicePx, int pageMarginLeft, GutterExtent extent) noexcept
{
    return extent.columns + extent.rail > 0
           && xDevicePx >= pageMarginLeft - extent.rail - extent.columns - extent.leadIn
           && xDevicePx < pageMarginLeft;
}

/// Which part of the gutter a point is over.
enum class GutterPart : uint8_t
{
    Cells = 0, ///< The cell columns: timestamps, line numbers, or the glyph block column.
    Rail,      ///< The block rail's strip.
};

/// A point over the gutter: the part, and for the cells the column counted from the gutter's left edge.
struct GutterPoint
{
    GutterPart part = GutterPart::Cells; ///< The part under the point.
    int column = 0;                      ///< The cell column; 0 over the rail.

    bool operator==(GutterPoint const&) const = default;
};

/// What of the gutter is under @p xDevicePx: the rail's strip, or a cell column counted from the gutter's
/// LEFT edge -- the column vtbackend::gutterSegmentAt() names a segment for.
///
/// The lead-in counts as the first column's -- the rail's, when there are no cells: a target that reaches
/// the pane's edge is the easier one to hit, and the strip holds nothing else a press there could have meant.
/// Built on isInGutter() so the two cannot disagree about where the gutter is.
/// @param xDevicePx       The x coordinate, in device pixels, relative to the item's left edge.
/// @param pageMarginLeft  The renderer's grid origin (@c GridMetrics::pageMargin.left), in device pixels.
/// @param extent          The reserved gutter.
/// @param cellWidthPx     The cell width in device pixels.
/// @return The point, or nullopt when the coordinate is not over the gutter.
[[nodiscard]] constexpr std::optional<GutterPoint> gutterPointAt(int xDevicePx,
                                                                 int pageMarginLeft,
                                                                 GutterExtent extent,
                                                                 int cellWidthPx) noexcept
{
    if (cellWidthPx <= 0 || !isInGutter(xDevicePx, pageMarginLeft, extent))
        return std::nullopt;
    auto const railStart = pageMarginLeft - extent.rail;
    if (xDevicePx >= railStart || extent.columns == 0)
        return GutterPoint { .part = GutterPart::Rail, .column = 0 };
    return GutterPoint { .part = GutterPart::Cells,
                         .column = std::max(0, xDevicePx - (railStart - extent.columns)) / cellWidthPx };
}

/// The gutter cell column under @p xDevicePx, counted from the gutter's LEFT edge (@see gutterPointAt).
/// @param xDevicePx       The x coordinate, in device pixels, relative to the item's left edge.
/// @param pageMarginLeft  The renderer's grid origin (@c GridMetrics::pageMargin.left), in device pixels.
/// @param extent          The reserved gutter.
/// @param cellWidthPx     The cell width in device pixels.
/// @return The column, or nullopt when the coordinate is not over the gutter's cells -- over the rail
///         included.
[[nodiscard]] constexpr std::optional<int> gutterColumnAt(int xDevicePx,
                                                          int pageMarginLeft,
                                                          GutterExtent extent,
                                                          int cellWidthPx) noexcept
{
    auto const point = gutterPointAt(xDevicePx, pageMarginLeft, extent, cellWidthPx);
    if (!point || point->part != GutterPart::Cells)
        return std::nullopt;
    return point->column;
}

/// The page column under @p xDevicePx, or nullopt when it is beside the page: over the gutter, or the margins
/// left and right of the columns.
///
/// The declining sibling of the cell hit-test's clamped column, for whatever spans the page's columns only --
/// the sticky header, which leaves the gutter and the margins to themselves.
/// @param xDevicePx      The x coordinate, in device pixels, relative to the item's left edge.
/// @param pageMarginLeft The renderer's grid origin (@c GridMetrics::pageMargin.left), in device pixels.
/// @param cellWidthPx    The cell width in device pixels.
/// @param pageColumns    How many columns the page has.
/// @return The column, or nullopt when the coordinate is not over the page's columns.
[[nodiscard]] constexpr std::optional<int> pageColumnAt(int xDevicePx,
                                                        int pageMarginLeft,
                                                        int cellWidthPx,
                                                        int pageColumns) noexcept
{
    if (cellWidthPx <= 0 || xDevicePx < pageMarginLeft)
        return std::nullopt;
    auto const column = (xDevicePx - pageMarginLeft) / cellWidthPx;
    if (column >= pageColumns)
        return std::nullopt;
    return column;
}

/// Logical device-pixel availability of an item: floor(logical * scale) per axis (FLOOR side of the law).
/// @param logicalWidth  Item width in logical pixels (Qt item coordinates; may be fractional).
/// @param logicalHeight Item height in logical pixels.
/// @param scale         Content scale (device pixels per logical pixel).
/// @return Available area in whole device pixels.
[[nodiscard]] constexpr vtbackend::ImageSize availableDevicePixels(double logicalWidth,
                                                                   double logicalHeight,
                                                                   double scale) noexcept
{
    return { .width = vtbackend::Width::cast_from(detail::floorScaled(logicalWidth, scale)),
             .height = vtbackend::Height::cast_from(detail::floorScaled(logicalHeight, scale)) };
}

/// Device-pixel extent -> logical size, CEILED per axis (requirement side of the law: a window granted
/// this logical size always covers the device-pixel requirement).
/// @param devicePx Extent in device pixels.
/// @param scale    Content scale (device pixels per logical pixel).
/// @return The covering extent in logical pixels.
[[nodiscard]] constexpr LogicalSize logicalSizeForDevicePixels(vtbackend::ImageSize devicePx,
                                                               double scale) noexcept
{
    return { .width = detail::ceilUnscaled(unbox<double>(devicePx.width), scale),
             .height = detail::ceilUnscaled(unbox<double>(devicePx.height), scale) };
}

/// Window size (logical pixels) needed to show the given total page: ceil(requiredDevicePx / scale) plus the
/// declared chrome (CEIL side of the law — the resulting window always fits the requested grid; see the
/// roundtrip invariant in the header comment).
/// @param totalPageSize   Total page size (main page + status line).
/// @param cellSize        Cell size in device pixels.
/// @param marginsDevicePx Symmetric margins in device pixels.
/// @param scale           Content scale (device pixels per logical pixel).
/// @param chrome          Declared window chrome in logical pixels.
/// @param gutterDevicePx  Gutter reserved left of the grid, in device pixels (@see GutterWidth).
/// @return Window size in logical pixels.
[[nodiscard]] constexpr LogicalSize windowSizeForPage(vtbackend::PageSize totalPageSize,
                                                      vtbackend::ImageSize cellSize,
                                                      Margins marginsDevicePx,
                                                      double scale,
                                                      Chrome chrome,
                                                      GutterWidth gutterDevicePx = 0) noexcept
{
    auto const required = requiredPixelsForPage(totalPageSize, cellSize, marginsDevicePx, gutterDevicePx);
    return { .width = detail::ceilUnscaled(unbox<double>(required.width), scale) + chrome.width,
             .height = detail::ceilUnscaled(unbox<double>(required.height), scale) + chrome.height };
}

/// WM size hints for the given cell geometry: minimum = window size for MinimumTotalPageSize plus chrome;
/// base = the fixed non-grid part (2*margins + chrome); increment = one logical cell (ceiled so the step
/// always covers a whole cell at fractional scale).
/// @param cellSizeDevicePx Cell size in device pixels.
/// @param marginsLogicalPx Symmetric margins in logical pixels (as configured).
/// @param scale            Content scale (device pixels per logical pixel).
/// @param chrome           Declared window chrome in logical pixels.
/// @param gutterDevicePx   Gutter reserved left of the grid, in DEVICE pixels -- the unit it is decided
///                         in, being a fraction of the cell width (@see GutterWidth). Taken in that
///                         unit and converted once, here: a caller that unscaled it first would hand
///                         over a truncated value this would then scale back up, and the base hint
///                         would be a pixel short of the gutter the renderer actually reserves.
/// @return The hints, in logical pixels.
[[nodiscard]] constexpr SizeHints sizeHintsFor(vtbackend::ImageSize cellSizeDevicePx,
                                               Margins marginsLogicalPx,
                                               double scale,
                                               Chrome chrome,
                                               GutterWidth gutterDevicePx = 0) noexcept
{
    auto const minimum = windowSizeForPage(MinimumTotalPageSize,
                                           cellSizeDevicePx,
                                           scaled(marginsLogicalPx, scale),
                                           scale,
                                           chrome,
                                           gutterDevicePx);

    // Ceiled, like the cell increment beside it: a base rounded DOWN leaves the grid area a device
    // pixel wider than the window can give it, which the resize snapping then reads as room for a
    // column that does not fit.
    auto const base = LogicalSize { .width = (2 * marginsLogicalPx.horizontal) + chrome.width
                                             + detail::ceilUnscaled(gutterDevicePx, scale),
                                    .height = (2 * marginsLogicalPx.vertical) + chrome.height };
    auto const increment =
        LogicalSize { .width = detail::ceilUnscaled(unbox<double>(cellSizeDevicePx.width), scale),
                      .height = detail::ceilUnscaled(unbox<double>(cellSizeDevicePx.height), scale) };
    return { .minimum = minimum, .base = base, .increment = increment };
}

/// THE content-scale decision (exactly one implementation, shared byte-for-byte by pre-show sizing and the
/// runtime renderer): a platform font-DPI override (KDE kcmfonts forceFontDPI, honored at >= 96) wins over
/// the window's device-pixel ratio, which wins over the target screen's (the pre-show guess); absent all
/// three the scale is 1.0. The result is clamped to >= 0.5 as a last-resort guard against nonsensical
/// platform reports.
///
/// NB: when the override disagrees with the real DPR, device-pixel conversion also uses the overridden
/// value — historic behavior, deliberately preserved. TODO(dpi-split): separate font DPI from the
/// logical<->device DPR; requires revisiting the render-target sizing contract.
/// @param forcedFontDpi Platform font-DPI override (e.g. KDE forceFontDPI), if configured.
/// @param windowDpr     The mapped window's devicePixelRatio, if a platform window exists.
/// @param screenDpr     The target screen's devicePixelRatio (pre-show best guess), if known.
/// @return The resolved content scale (device pixels per logical pixel), >= 0.5.
[[nodiscard]] constexpr double resolveContentScale(std::optional<double> forcedFontDpi,
                                                   std::optional<double> windowDpr,
                                                   std::optional<double> screenDpr) noexcept
{
    auto const scale = [&]() {
        if (forcedFontDpi && *forcedFontDpi >= 96.0)
            return *forcedFontDpi / 96.0;
        if (windowDpr && *windowDpr > 0.0)
            return *windowDpr;
        if (screenDpr && *screenDpr > 0.0)
            return *screenDpr;
        return 1.0;
    }();
    return std::max(0.5, scale);
}

/// Snaps a split pane's logical extent so its boundary lands on a whole DEVICE pixel.
///
/// A split child is sized `view.width * ratio`, which is a real number -- so the first pane routinely
/// gets a fractional width and the second a fractional ORIGIN. That origin used to reach the renderer by
/// two paths that rounded differently: the vertex transform composed it unrounded, while the scissor
/// rounded it to a whole pixel. Geometry and clip then disagreed by up to half a pixel, and since
/// the glyph atlas is sampled with QRhiSampler::Nearest the sample points landed on texel boundaries --
/// a stem loses its bright column for a scanline or two while its neighbours in the same row, same
/// frame, are untouched. That is #2040: measured as a stem's ink dropping 2656 -> 2043 (23%) with the
/// glyph's mask intact, on two of three identical `l` glyphs.
///
/// composeItemToClip() now rounds every item's origin to the pixel the scissor uses (snappedItemOrigin),
/// whatever placed it, so this snap is no longer what keeps the glyphs whole. It still keeps the pane
/// boundary itself, and the split handle between two panes, on the device grid.
///
/// Snapping in logical coordinates alone is not enough at a fractional device-pixel ratio: the
/// quantity both paths agree about is the HARDWARE pixel, so the rounding happens in device space and
/// converts back.
///
/// @note Applies to the SPLIT HANDLE too, not just the pane extent. The second pane's origin is
///       `firstExtent + handleThickness`, so a snapped extent alone still leaves that origin
///       fractional whenever the handle is: the default 6 logical pixels is 7.5 device pixels at the
///       common 125% scale and 10.5 at 175%. PaneNode.qml therefore snaps both through this rule.
///       WindowController's window-size solver keeps using the unsnapped thickness, which is
///       deliberate -- it sizes the WINDOW, where a sub-logical-pixel difference cannot move a pane
///       boundary off the device grid.
///
/// @note The binding in PaneNode.qml computes this inline rather than calling here: a QML property
///       binding cannot invoke a free C++ function, and exposing it would mean a QObject singleton
///       registered with the meta-object system purely for one arithmetic expression. The two must
///       therefore stay in step by hand -- this function is the specification and the tested one, so
///       any change to the rule belongs here first, and the QML binding follows.
///
/// @param logicalExtent The unsnapped extent (`view.width * ratio`) in logical pixels.
/// @param devicePixelRatio Logical-to-device scale; values <= 0 are treated as 1 (nothing to snap to).
/// @return The extent snapped so `extent * devicePixelRatio` is an integer.
[[nodiscard]] inline double snapPaneExtentToDevicePixels(double logicalExtent,
                                                         double devicePixelRatio) noexcept
{
    if (!(devicePixelRatio > 0.0))
        return logicalExtent;
    return std::round(logicalExtent * devicePixelRatio) / devicePixelRatio;
}

} // namespace contour::geometry
