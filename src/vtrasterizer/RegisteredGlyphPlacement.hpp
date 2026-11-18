// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <algorithm>
#include <optional>

/// Where a glyph registered through the Glyph Protocol lands in its cell: the protocol's sizing and
/// placement model (specification section 8.5), free of any render target so it can be tested by itself.
namespace vtrasterizer
{

/// Maps a point of an outline, in authored units with Y up, onto the pixels of its render span, Y down
/// from the span's top-left corner: `(x, y)` lands on `(tx + x * sx, ty - y * sy)`.
struct GlyphSpanTransform
{
    float sx = 1;
    float sy = 1;
    float tx = 0;
    float ty = 0;

    bool operator==(GlyphSpanTransform const&) const = default;
};

/// The pixels a glyph is scaled into.
struct GlyphSpanGeometry
{
    float width = 0;    ///< The span's width: one or two cells.
    float height = 0;   ///< The span's height: one cell.
    float baseline = 0; ///< The text baseline, down from the span's top.
};

/// Places an outline in its render span: pad, then size, then align, all on the authored extent.
/// @param layout The registration's sizing and placement.
/// @param bounds The outline's own bounds, which are the extent of a registration that declares none.
/// @param span The pixels to place it in.
/// @return The transform, or nothing when there is nothing to draw: an empty span, or an extent without
///         width or height.
[[nodiscard]] constexpr std::optional<GlyphSpanTransform> placeRegisteredGlyph(
    vtbackend::GlyphLayout const& layout,
    vtbackend::GlyphOutlineBounds const& bounds,
    GlyphSpanGeometry const& span) noexcept
{
    using vtbackend::GlyphHorizontalAlignment;
    using vtbackend::GlyphSizeMode;
    using vtbackend::GlyphVerticalAlignment;

    // The authored extent: declared, or the outline's bounds.
    auto const declared = layout.declaresExtent();
    auto const unitsPerEm = static_cast<float>(layout.unitsPerEm);
    auto const left = declared ? 0.0F : static_cast<float>(bounds.xMin);
    auto const bottom =
        declared ? static_cast<float>(layout.extentBottom.value_or(0)) : static_cast<float>(bounds.yMin);
    auto const width = declared ? static_cast<float>(layout.advanceWidth.value_or(layout.unitsPerEm))
                                : static_cast<float>(bounds.xMax - bounds.xMin);
    auto const height = declared ? static_cast<float>(layout.lineHeight.value_or(layout.unitsPerEm))
                                 : static_cast<float>(bounds.yMax - bounds.yMin);
    if (width <= 0 || height <= 0 || span.width <= 0 || span.height <= 0 || unitsPerEm <= 0)
        return std::nullopt;

    // The effective span, inside the padding.
    auto const& pad = layout.padding;
    auto const insetX = span.width * pad.left;
    auto const insetY = span.height * pad.top;
    auto const innerWidth = span.width * (1 - pad.left - pad.right);
    auto const innerHeight = span.height * (1 - pad.top - pad.bottom);

    auto const fitWidth = innerWidth / width;
    auto const fitHeight = innerHeight / height;
    // Without a declared extent the em keeps a set in proportion: a period stays smaller than an M.
    auto const contain =
        declared ? std::min(fitWidth, fitHeight)
                 : std::min({ fitWidth, fitHeight, std::min(innerWidth, innerHeight) / unitsPerEm });
    auto const cover = std::max(fitWidth, fitHeight);

    auto transform = GlyphSpanTransform {};
    switch (layout.size)
    {
        case GlyphSizeMode::Contain: transform.sx = transform.sy = contain; break;
        case GlyphSizeMode::Height: transform.sx = transform.sy = fitHeight; break;
        case GlyphSizeMode::Advance: transform.sx = transform.sy = fitWidth; break;
        case GlyphSizeMode::Cover: transform.sx = transform.sy = cover; break;
        case GlyphSizeMode::Stretch:
            transform.sx = fitWidth;
            transform.sy = fitHeight;
            break;
    }

    auto const slackX = innerWidth - (width * transform.sx);
    transform.tx = insetX - (left * transform.sx);
    switch (layout.horizontal)
    {
        case GlyphHorizontalAlignment::Start: break;
        case GlyphHorizontalAlignment::Center: transform.tx += slackX / 2; break;
        case GlyphHorizontalAlignment::End: transform.tx += slackX; break;
    }

    // Where the outline's y = 0 lands when the extent's bottom sits on the span's.
    auto const onBottom = insetY + innerHeight + (bottom * transform.sy);
    auto const slackY = innerHeight - (height * transform.sy);
    // A stretched extent is the span, so there is nothing for an alignment to choose.
    auto const vertical =
        layout.size == GlyphSizeMode::Stretch ? GlyphVerticalAlignment::Start : layout.vertical;
    switch (vertical)
    {
        case GlyphVerticalAlignment::Start: transform.ty = onBottom; break;
        case GlyphVerticalAlignment::Center: transform.ty = onBottom - (slackY / 2); break;
        case GlyphVerticalAlignment::End: transform.ty = onBottom - slackY; break;
        case GlyphVerticalAlignment::Baseline: transform.ty = span.baseline; break;
    }
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
    if (glyph.layout.span == GlyphRenderSpan::OneCell || next == nullptr)
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
