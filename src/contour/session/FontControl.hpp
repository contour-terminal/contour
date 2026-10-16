// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
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

/// The gutter layout @p config asks for: what the window reserves, what the hit-test reads, and -- through
/// the same config::gutterSettingsFor() -- what the terminal draws.
/// @param config The loaded configuration.
/// @return vtbackend::gutterLayoutFor(config::gutterSettingsFor(config)).
[[nodiscard]] vtbackend::GutterLayout configuredGutterLayout(config::Config const& config);

/// The gutter @p layout reserves, in device pixels, in its two parts: the blank lead-in against the pane's
/// edge, and the columns behind it.
/// @param layout The gutter's segments.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in is scaled by.
/// @return Both parts; both 0 when the layout has no columns.
[[nodiscard]] geometry::GutterExtent gutterExtentFor(vtbackend::GutterLayout const& layout,
                                                     vtbackend::ImageSize cellSize,
                                                     double scale);

/// The gutter @p config reserves, in device pixels, in its two parts: the blank lead-in against the pane's
/// edge, and the columns behind it. What the hit-test maps a pointer onto, the lead-in included.
/// @param config The loaded configuration.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in is scaled by.
/// @return Both parts; both 0 when the layout has no columns.
[[nodiscard]] geometry::GutterExtent configuredGutterExtent(config::Config const& config,
                                                            vtbackend::ImageSize cellSize,
                                                            double scale);

/// The gutter width, in device pixels, @p config reserves: what every page fit, window size and size hint
/// takes off the page. Its columns behind the blank lead-in that keeps them clear of the window's edge.
/// @param config The loaded configuration.
/// @param cellSize The cell size in device pixels.
/// @param scale The device-pixel ratio, which the lead-in is scaled by.
/// @return configuredGutterExtent()'s two parts added up; 0 when the layout has no columns.
[[nodiscard]] geometry::GutterWidth configuredGutterWidth(config::Config const& config,
                                                          vtbackend::ImageSize cellSize,
                                                          double scale);

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
