// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>

#include <algorithm>
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

/// Window chrome OUTSIDE the terminal content area (e.g. the QML title/tab bar), in logical pixels,
/// DECLARED by the UI layer (main.qml knows its layout). Never measured from live window-minus-item
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
/// character-cell resize grid on X11 (WM_NORMAL_HINTS) and its Windows analogue, is silently ignored on
/// Wayland (xdg-shell has no increment concept), and is actively harmful on macOS — see @ref
/// SizeHintPolicy and @ref sizeHintPolicyFor.
struct SizeHints
{
    LogicalSize minimum;   ///< Smallest window the WM should allow.
    LogicalSize base;      ///< Fixed part not participating in the increment grid (margins + chrome).
    LogicalSize increment; ///< Interactive-resize step: one logical cell.

    [[nodiscard]] constexpr bool operator==(SizeHints const&) const noexcept = default;
};

/// Windowing platform, as far as WM size-hint applicability is concerned. Data (not a scattered
/// `#if defined`) so @ref sizeHintPolicyFor is a pure, per-OS-testable function on any host.
enum class SizeHintPlatform : std::uint8_t
{
    Windows, ///< Win32: base/increment honored for interactive resize.
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
/// terminal grid to a degenerate size. macOS therefore gets @c minimum only; there is no character-grid
/// resize-snapping protocol on Cocoa to preserve.
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
#if defined(_WIN32)
    return SizeHintPlatform::Windows;
#elif defined(__APPLE__)
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

    /// ceil(value / scale) for non-negative value and positive scale, in constexpr-friendly form.
    [[nodiscard]] constexpr int ceilUnscaled(double value, double scale) noexcept
    {
        auto const unscaled = std::max(0.0, value / scale) - RoundingSlack;
        auto const truncated = static_cast<int>(unscaled);
        return unscaled > static_cast<double>(truncated) ? truncated + 1 : truncated;
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

/// Computes how many whole cells fit into the given device-pixel area: floor((available - 2*margins) / cell).
///
/// FLOOR is the load-bearing contract: the grid never claims pixels the surface does not have; the sub-cell
/// remainder is rendered as padding. Degenerate inputs (margins exceeding the area, zero cell size) clamp to
/// a 1x1 page instead of wrapping.
/// @param availableDevicePx Total available area in device pixels.
/// @param cellSize          Cell size in device pixels.
/// @param marginsDevicePx   Symmetric minimum margins in device pixels.
/// @return The floored page size, at least 1x1.
[[nodiscard]] constexpr vtbackend::PageSize pageSizeForPixels(vtbackend::ImageSize availableDevicePx,
                                                              vtbackend::ImageSize cellSize,
                                                              Margins marginsDevicePx) noexcept
{
    auto const usableWidth =
        std::max(0, unbox<int>(availableDevicePx.width) - 2 * marginsDevicePx.horizontal);
    auto const usableHeight =
        std::max(0, unbox<int>(availableDevicePx.height) - 2 * marginsDevicePx.vertical);
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
/// @return Required area in device pixels.
[[nodiscard]] constexpr vtbackend::ImageSize requiredPixelsForPage(vtbackend::PageSize totalPageSize,
                                                                   vtbackend::ImageSize cellSize,
                                                                   Margins marginsDevicePx) noexcept
{
    auto const width =
        (unbox<int>(cellSize.width) * unbox<int>(totalPageSize.columns)) + (2 * marginsDevicePx.horizontal);
    auto const height =
        (unbox<int>(cellSize.height) * unbox<int>(totalPageSize.lines)) + (2 * marginsDevicePx.vertical);
    return { .width = vtbackend::Width::cast_from(std::max(0, width)),
             .height = vtbackend::Height::cast_from(std::max(0, height)) };
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
/// @return The fitted page and the renderer margin placement.
template <typename ClampFn>
    requires std::is_invocable_r_v<vtbackend::PageSize, ClampFn, vtbackend::PageSize>
[[nodiscard]] constexpr GridFit fitPageToPixels(vtbackend::ImageSize availableDevicePx,
                                                vtbackend::ImageSize cellSize,
                                                Margins marginsDevicePx,
                                                ClampFn&& clampTotalPageSize) noexcept
{
    auto const page = std::forward<ClampFn>(clampTotalPageSize)(
        pageSizeForPixels(availableDevicePx, cellSize, marginsDevicePx));

    auto const usedHeight = unbox<int>(page.lines) * unbox<int>(cellSize.height);
    auto const top = marginsDevicePx.vertical;
    auto const leftover = unbox<int>(availableDevicePx.height) - usedHeight - top;
    auto const bottom = std::clamp(leftover, 0, marginsDevicePx.vertical);

    return { .pageSize = page,
             .pageMargin = { .left = marginsDevicePx.horizontal, .top = top, .bottom = bottom } };
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
/// @return Window size in logical pixels.
[[nodiscard]] constexpr LogicalSize windowSizeForPage(vtbackend::PageSize totalPageSize,
                                                      vtbackend::ImageSize cellSize,
                                                      Margins marginsDevicePx,
                                                      double scale,
                                                      Chrome chrome) noexcept
{
    auto const required = requiredPixelsForPage(totalPageSize, cellSize, marginsDevicePx);
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
/// @return The hints, in logical pixels.
[[nodiscard]] constexpr SizeHints sizeHintsFor(vtbackend::ImageSize cellSizeDevicePx,
                                               Margins marginsLogicalPx,
                                               double scale,
                                               Chrome chrome) noexcept
{
    auto const minimum = windowSizeForPage(
        MinimumTotalPageSize, cellSizeDevicePx, scaled(marginsLogicalPx, scale), scale, chrome);
    auto const base = LogicalSize { .width = (2 * marginsLogicalPx.horizontal) + chrome.width,
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

} // namespace contour::geometry
