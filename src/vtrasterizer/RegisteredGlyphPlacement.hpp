// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <algorithm>
#include <optional>

/// Where a glyph registered through the Glyph Protocol lands in its cell: its viewBox placed in the
/// render span the way SVG's `preserveAspectRatio` places a viewBox in a viewport (specification section
/// 8.9.3), free of any render target so it can be tested by itself.
namespace vtrasterizer
{

/// Maps a point of a glyph's viewBox onto the pixels of its render span, both with Y down and the span's
/// top-left corner at the origin: `(x, y)` lands on `(tx + x * sx, ty + y * sy)`.
struct GlyphSpanTransform
{
    float sx = 1;
    float sy = 1;
    float tx = 0;
    float ty = 0;

    bool operator==(GlyphSpanTransform const&) const = default;
};

/// The pixels a glyph is scaled into: one or two cells wide, one cell tall.
struct GlyphSpanGeometry
{
    float width = 0;
    float height = 0;
};

/// Places a glyph's viewBox in its render span: scaled by `fit`, then positioned by `align`.
/// @param layout The registration's placement.
/// @param viewBox The glyph's viewBox, which is its extent.
/// @param span The pixels to place it in.
/// @return The transform, or nothing when there is nothing to draw into: an empty span.
[[nodiscard]] constexpr std::optional<GlyphSpanTransform> placeRegisteredGlyph(
    vtbackend::GlyphLayout const& layout,
    vtbackend::VectorViewBox const& viewBox,
    GlyphSpanGeometry const& span) noexcept
{
    using vtbackend::GlyphAlignment;
    using vtbackend::GlyphFit;

    if (!(viewBox.width > 0 && viewBox.height > 0 && span.width > 0 && span.height > 0))
        return std::nullopt;

    auto const fitWidth = span.width / viewBox.width;
    auto const fitHeight = span.height / viewBox.height;
    auto transform = GlyphSpanTransform {};
    switch (layout.fit)
    {
        case GlyphFit::Contain: transform.sx = transform.sy = std::min(fitWidth, fitHeight); break;
        case GlyphFit::Cover: transform.sx = transform.sy = std::max(fitWidth, fitHeight); break;
        case GlyphFit::Stretch:
            transform.sx = fitWidth;
            transform.sy = fitHeight;
            break;
    }

    // What the span has left over on an axis, or lacks, once the scaled viewBox is in it.
    auto const offsetFor = [](GlyphAlignment alignment, float slack) {
        switch (alignment)
        {
            case GlyphAlignment::Start: return 0.0F;
            case GlyphAlignment::Center: return slack / 2;
            case GlyphAlignment::End: return slack;
        }
        return 0.0F;
    };
    transform.tx = offsetFor(layout.horizontal, span.width - (viewBox.width * transform.sx))
                   - (viewBox.x * transform.sx);
    transform.ty = offsetFor(layout.vertical, span.height - (viewBox.height * transform.sy))
                   - (viewBox.y * transform.sy);
    return transform;
}

/// Decides how many cells a registered glyph is drawn across. A glyph registered two cells wide overflows
/// into the cell after its own, which the application keeps blank; where there is no such cell -- the
/// glyph is in the last column, or something is printed after it -- it is drawn in one.
/// @param glyph The glyph.
/// @param cell The cell showing it.
/// @param next The cell rendered after @p cell; null for the last one.
/// @return The span to draw the glyph across.
[[nodiscard]] inline vtbackend::GlyphRenderSpan registeredGlyphSpan(
    vtbackend::RegisteredGlyph const& glyph,
    vtbackend::RenderCell const& cell,
    vtbackend::RenderCell const* next) noexcept
{
    using vtbackend::GlyphRenderSpan;
    if (glyph.definition.layout.span == GlyphRenderSpan::OneCell || next == nullptr)
        return GlyphRenderSpan::OneCell;

    auto const columnsPerCell = cell.attributes.lineFlags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;
    auto const follows =
        next->position.line == cell.position.line
        && next->position.column == cell.position.column + vtbackend::ColumnOffset(columnsPerCell);
    auto const blank = !next->image && std::ranges::all_of(next->codepoints, [](char32_t codepoint) {
        return codepoint == U' ' || codepoint == 0;
    });
    return follows && blank ? GlyphRenderSpan::TwoCells : GlyphRenderSpan::OneCell;
}

} // namespace vtrasterizer
