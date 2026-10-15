// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/render/RenderBuffer.hpp>

#include <string>

namespace vtrasterizer
{

namespace detail
{
    /// A one-codepoint cell outside RenderBuffer::cells, as its own shaping group: placed at its own pen
    /// position rather than run together with whatever the text renderer saw last. Spelled here for the
    /// gutter's single cells and in vtbackend::layoutRenderText() for every overlay that is a run of
    /// text, so a field added to RenderCell is filled in those two places and no third.
    ///
    /// @param codepoint The codepoint to draw.
    /// @param position Where to draw it; the column may be negative, left of the grid.
    /// @param attributes How to draw it.
    /// @return The cell.
    [[nodiscard]] inline vtbackend::RenderCell standaloneCell(char32_t codepoint,
                                                              vtbackend::CellLocation position,
                                                              vtbackend::RenderAttributes const& attributes)
    {
        return vtbackend::RenderCell {
            .codepoints = std::u32string(1, codepoint),
            .image = {},
            .position = position,
            .attributes = attributes,
            .width = 1,
            .sizing = {},
            .groupStart = true,
            .groupEnd = true,
        };
    }
} // namespace detail

/// The cell a gutter cell is drawn as: at its own column left of the grid, as a shaping group of its own.
///
/// Split out of Renderer::renderGutter so the placement is testable without fonts or a render target. A
/// gutter cell is mapped through the very GridMetrics every grid cell is, at a NEGATIVE column, which is
/// the whole of what lets a gutter several columns wide exist without a second coordinate system.
///
/// @param marker The gutter cell the terminal emitted.
/// @return The cell to hand the background and text renderers.
[[nodiscard]] inline vtbackend::RenderCell gutterRenderCell(vtbackend::RenderGutterCell const& marker)
{
    return detail::standaloneCell(
        marker.codepoint,
        vtbackend::CellLocation { .line = marker.lineOffset, .column = marker.column },
        marker.attributes);
}

} // namespace vtrasterizer
