// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/geometry/WindowGeometry.hpp>

#include <vtbackend/core/Primitives.hpp>

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

/// The gutter width, in device pixels, a profile asks for.
///
/// THE gutter decision, in one place: every geometry call site reads it from here, because a page fitted
/// with a gutter and a renderer drawing without one (or vice versa) put the grid in two different places.
/// One cell wide when fold markers are shown, and nothing at all otherwise -- a gutter costs a column of
/// terminal width, so a user who does not want markers must not pay for it.
///
/// @param folding The profile's folding configuration.
/// @param cellSizeDevicePx The cell size in device pixels.
/// @return The gutter width in device pixels; 0 when no gutter is wanted.
[[nodiscard]] constexpr geometry::GutterWidth gutterWidthFor(config::FoldingConfig const& folding,
                                                             vtbackend::ImageSize cellSizeDevicePx) noexcept
{
    if (!folding.markersVisible())
        return 0;
    return unbox<int>(cellSizeDevicePx.width);
}

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
