// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/geometry/WindowGeometry.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/GutterSettings.hpp>

#include <vtrasterizer/FontDescriptions.hpp>

#include <text_shaper/Font.hpp>

namespace vtrasterizer
{
class Renderer;
}

namespace contour::session
{

class TerminalSession;

/// The font configuration @p renderer is currently rendering with, in the form the terminal reports
/// it to an application that asks (OSC 50).
[[nodiscard]] vtbackend::FontDef getFontDefinition(vtrasterizer::Renderer& renderer);

/// Clamps a font description onto what the given DPI can actually rasterize.
/// @param fonts The requested description.
/// @param screenDPI The DPI it will be rasterized at.
/// @return The description with impossible sizes brought into range.
[[nodiscard]] vtrasterizer::FontDescriptions sanitizeFontDescription(vtrasterizer::FontDescriptions fonts,
                                                                     text::DPI screenDPI);

/// Adapts configured window margins to the geometry module's margin type.
/// @param margins Configured margins (logical or device pixels — the unit passes through unchanged).
/// @return The same margins as geometry::Margins.
[[nodiscard]] constexpr geometry::Margins toGeometryMargins(config::WindowMargins margins) noexcept
{
    return { .horizontal = unbox<int>(margins.horizontal), .vertical = unbox<int>(margins.vertical) };
}

/// The gutter width, in device pixels, @p layout reserves.
///
/// THE gutter decision, in one place: every geometry call site reads it through configuredGutterWidth(),
/// whose layout is configuredGutterLayout() -- the very gutterSettingsFor() the terminal draws with --
/// because a page fitted with one gutter and a renderer drawing another put the grid in two places.
///
/// @param layout The gutter's segments.
/// @param cellSize The cell size in device pixels.
/// @return The width in device pixels; 0 when the layout has no columns.
/// @see vtbackend::gutterLayoutFor
[[nodiscard]] inline geometry::GutterWidth gutterWidthFor(vtbackend::GutterLayout const& layout,
                                                          vtbackend::ImageSize cellSize) noexcept
{
    return layout.totalColumns * unbox<int>(cellSize.width);
}

/// How a session's chrome row draws the gutter's block column, in points: the form, and the rail's metrics.
///
/// Fixed per session: the chrome row is resolved once, before the first session exists (`ui_style` takes
/// effect on the next start), so whatever reads it -- the page fit on the render thread among them -- reads
/// one value for the session's life.
struct GutterChrome
{
    vtbackend::BlockColumnForm form =
        vtbackend::BlockColumnForm::Glyphs; ///< Glyph cells, or the rail's strip.
    int stripPoints = 0;                    ///< The strip's width (@c space.blockGutter); 0 under Glyphs.
    int barPoints = 0;                      ///< The rail bar's width (@c size.rail).
    int barInsetPoints = 0;                 ///< The bar's left edge within the strip (@c space.railInset).
    int barRadiusPoints = 0;                ///< The bar's end radius (@c radius.rail).
    int markPoints = 0;                     ///< The rail mark's edge (@c size.railMark).

    bool operator==(GutterChrome const&) const = default;
};

/// The gutter chrome of chrome row @p style, from its tokens.
/// @param style The chrome row.
/// @return Its block column's form and the rail's metrics in points.
[[nodiscard]] GutterChrome gutterChromeFor(config::ChromeStyle style) noexcept;

/// The gutter layout @p config asks for under @p chrome: what the window reserves, what the hit-test reads,
/// and -- through the same config::gutterSettingsFor() -- what the terminal draws.
/// @param config The loaded configuration.
/// @param chrome The session's gutter chrome, whose form decides whether the block column is cells.
/// @return vtbackend::gutterLayoutFor(config::gutterSettingsFor(config, chrome.form)).
[[nodiscard]] vtbackend::GutterLayout configuredGutterLayout(config::Config const& config,
                                                             GutterChrome const& chrome);

/// The gutter @p layout reserves, in device pixels, in its three parts: the blank lead-in against the pane's
/// edge, the cell columns behind it, and the rail's strip against the grid.
/// @param layout The gutter's segments.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in and the strip are scaled by.
/// @param chrome The session's gutter chrome: the strip's width in points.
/// @return The parts; all 0 when the layout has neither columns nor a rail.
[[nodiscard]] geometry::GutterExtent gutterExtentFor(vtbackend::GutterLayout const& layout,
                                                     vtbackend::ImageSize cellSize,
                                                     double scale,
                                                     GutterChrome const& chrome);

/// The gutter @p config reserves under @p chrome, in device pixels, in its three parts. What the hit-test
/// maps a pointer onto, the lead-in included.
/// @param config The loaded configuration.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in and the strip are scaled by.
/// @param chrome The session's gutter chrome.
/// @return The parts; all 0 when there is no gutter.
[[nodiscard]] geometry::GutterExtent configuredGutterExtent(config::Config const& config,
                                                            vtbackend::ImageSize cellSize,
                                                            double scale,
                                                            GutterChrome const& chrome);

/// The gutter width, in device pixels, @p config reserves under @p chrome: what every page fit, window size
/// and size hint takes off the page. Its cells and the rail's strip behind the blank lead-in that keeps them
/// clear of the window's edge.
/// @param config The loaded configuration.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in and the strip are scaled by.
/// @param chrome The session's gutter chrome.
/// @return configuredGutterExtent()'s parts added up; 0 when there is no gutter.
[[nodiscard]] geometry::GutterWidth configuredGutterWidth(config::Config const& config,
                                                          vtbackend::ImageSize cellSize,
                                                          double scale,
                                                          GutterChrome const& chrome);

/// Resizes @p session's terminal to @p totalPageSize under its lock, and, when the page changed, asks for the
/// size indicator. The request is posted to the session's thread, so it reads the main page as it is when it
/// runs, outside the lock: this is reached from the render thread too (TerminalDisplay::createRenderer()).
/// @param session The session whose terminal to resize.
/// @param totalPageSize The new page, status line included.
/// @param viewSize What the child is told the pixel size is.
void resizeToFit(TerminalSession& session, vtbackend::PageSize totalPageSize, vtbackend::ImageSize viewSize);

/// Tells @p session's QML that its page changed size: lineCountChanged() and columnsCountChanged(), each only
/// where that dimension changed. Posted to the session's thread, as resizeToFit() posts the size indicator:
/// applyResize() runs on the render thread too, and the signals wake QML bindings.
/// @param session The session whose page changed.
/// @param oldPage The page before the resize.
/// @param newPage The page after it.
void announcePageSize(TerminalSession& session, vtbackend::PageSize oldPage, vtbackend::PageSize newPage);

/// Refits the grid to @p newPixelSize and resizes the child PTY accordingly.
void applyResize(vtbackend::ImageSize newPixelSize,
                 TerminalSession& session,
                 vtrasterizer::Renderer& renderer);

/// Applies @p fontDescriptions to @p renderer at @p dpi.
/// @return Whether anything actually changed.
bool applyFontDescription(text::DPI dpi,
                          vtrasterizer::Renderer& renderer,
                          vtrasterizer::FontDescriptions fontDescriptions);

} // namespace contour::session
