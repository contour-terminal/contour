// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.h>
#include <contour/geometry/WindowGeometry.h>

#include <vtbackend/primitives.h>

#include <vtrasterizer/FontDescriptions.h>

#include <text_shaper/font.h>

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
