// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/TextScale.h>
#include <vtbackend/primitives.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vtrasterizer
{

/// How a glyph is enlarged when the kitty text sizing protocol (`OSC 66` `s=`) asks for a scaled
/// cell block.
///
/// The two answers trade runtime cost against fidelity, which is why this is a choice rather than a
/// constant:
///
/// - @c Stretch reuses the glyph already rasterized at the ordinary cell size and draws it into a
///   larger rectangle. It costs nothing at rasterization time and nothing extra in the atlas -- the
///   same tile serves every scale -- but the result is a magnified bitmap, so it softens as the
///   scale grows. This is exactly what Contour already does for DECDHL double-height lines.
///
/// - @c Rerasterize asks the font for the glyph at `scale x` the point size, so the outline is
///   re-hinted and stays crisp at any scale. It costs a rasterization and an atlas tile per
///   (glyph, scale) pair.
///
/// A terminal that scrolls scaled text past the viewport pays @c Rerasterize's cost repeatedly,
/// which is the case that makes @c Stretch worth keeping rather than treating as a stepping stone.
enum class GlyphScalingMethod : uint8_t
{
    Stretch,
    Rerasterize,
};

/// A geometric adjustment to a rasterized tile: how much larger to draw it.
///
/// Expressed as a multiplier rather than an absolute size so that a strategy does not need to know
/// the cell size, which is what keeps this header free of the renderer. It is fractional because
/// `OSC 66`'s `n`/`d` ask for sizes that are not whole multiples of a cell.
struct GlyphScaleAdjustment
{
    /// Multiplier applied to the tile's target size and to its x offset, on both axes.
    double factor = 1.0;

    /// Whether the caller must rasterize the glyph afresh at `pointSize * factor` rather than reuse
    /// the tile it already has.
    bool requiresRerasterization = false;
};

/// Where a glyph's tile is drawn relative to the position ordinary text would take.
struct GlyphPenOffset
{
    int dx = 0;
    int dy = 0;
};

/// How the protocol's `v`/`h` distribute the slack a fraction leaves: 0 = top/left, 1 =
/// bottom/right, 2 = centered, indexed by the protocol's own value.
///
/// A table rather than a switch so that a third alignment is a row, and so that both the vertical
/// and horizontal axes read from one definition.
[[nodiscard]] constexpr double alignmentShare(uint8_t alignment) noexcept
{
    constexpr auto Share = std::array { 0.0, 1.0, 0.5 };
    return Share[alignment < Share.size() ? alignment : 0];
}

/// Where a glyph's raster sits inside the canvas of its text-sizing block, and how big that canvas
/// is.
///
/// A block is rasterized once into a canvas `scale` cells tall and then cut into cell-sized tiles,
/// because the atlas stores nothing larger than one cell. Deciding the placement here -- rather than
/// by shifting the pen when the tile is drawn -- is what lets every row of the block draw its own
/// tile at its own cell with no further arithmetic, and therefore what lets a block whose head has
/// scrolled off the viewport be clipped instead of lost.
///
/// kitty computes the same placement in calculate_regions_for_line() and apply_horizontal_alignment().
struct BlockPlacement
{
    /// Whole cells on both axes: `scale * numCells` wide by `scale` tall.
    vtbackend::ImageSize canvasSize {};

    /// Top-left of the glyph's raster within the canvas. May be negative, or beyond the canvas:
    /// a glyph on its baseline routinely overhangs, and the overhang is clipped away.
    int originX = 0;
    int originY = 0;
};

/// @param cellScale     the block's sizing.
/// @param cellWidth     one ordinary cell's width in pixels.
/// @param cellHeight    one ordinary cell's height in pixels.
/// @param baseline      the baseline's offset above one ordinary cell's bottom.
/// @param numCells      how many cells wide the block's content is at 1x (2 for a wide glyph).
/// @param glyphBearingX the raster's left bearing, at the size it was ACTUALLY rasterized.
/// @param glyphBearingY the raster's ink top above its baseline, at the size it was ACTUALLY
///                      rasterized. Both bearings are the final-size ones: a caller that magnifies
///                      an ordinary raster must scale them to match before asking.
[[nodiscard]] inline BlockPlacement blockPlacementFor(vtbackend::CellScale const& cellScale,
                                                      int cellWidth,
                                                      int cellHeight,
                                                      int baseline,
                                                      int numCells,
                                                      int glyphBearingX,
                                                      int glyphBearingY) noexcept
{
    auto const scale = static_cast<double>(cellScale.scale != 0 ? cellScale.scale : 1);
    auto const factor = cellScale.drawFactor();
    auto const cells = std::max(1, numCells);

    auto const canvasSize = vtbackend::ImageSize {
        vtbackend::Width::cast_from(static_cast<int>(std::lround(scale)) * cells * cellWidth),
        vtbackend::Height::cast_from(static_cast<int>(std::lround(scale)) * cellHeight),
    };

    // The drawn box is `factor` cells on each axis; a proper fraction leaves the rest as slack, and
    // v/h say where in it the box sits.
    auto const slack = scale - factor;
    auto const boxTop = slack * static_cast<double>(cellHeight) * alignmentShare(cellScale.verticalAlignment);
    auto const boxLeft = slack * static_cast<double>(cells) * static_cast<double>(cellWidth)
                         * alignmentShare(cellScale.horizontalAlignment);

    // The baseline sits `factor * baseline` above the drawn box's bottom, and the ink's top is its
    // bearing above the baseline.
    auto const boxBottom = boxTop + (factor * static_cast<double>(cellHeight));
    auto const baselineY = boxBottom - (factor * static_cast<double>(baseline));

    return BlockPlacement {
        .canvasSize = canvasSize,
        .originX = static_cast<int>(std::lround(boxLeft)) + glyphBearingX,
        .originY = static_cast<int>(std::lround(baselineY)) - glyphBearingY,
    };
}

/// Decides how a glyph is enlarged and where it sits. @see GlyphScalingMethod.
///
/// Injected rather than selected inline so that a second strategy is a new implementation and a new
/// row in the method table, not an edit to every place a glyph is drawn.
class GlyphScaler
{
  public:
    virtual ~GlyphScaler() = default;

    /// @param cellScale the block's sizing. Ordinary text must yield an identity adjustment.
    [[nodiscard]] virtual GlyphScaleAdjustment adjustmentFor(
        vtbackend::CellScale const& cellScale) const noexcept = 0;

    /// Where the tile goes, relative to where ordinary text of the same glyph would be drawn.
    ///
    /// The renderer's pen is the bottom of the block's FIRST row, and ordinary placement subtracts
    /// the baseline and the glyph's top bearing at 1x. A block `scale` cells tall wants its glyph's
    /// baseline `f * B` above the block's BOTTOM, where `f` is the drawn factor:
    ///
    ///     drawn  = pen.y - B - t + f*t
    ///     wanted = pen.y + (s-1)*H - f*B
    ///     dy     = (s-1)*H - (f-1)*(B + t)
    ///
    /// A fraction leaves slack -- `(s - f)` cells on each axis -- and `v`/`h` say where in that slack
    /// the glyph sits. The formula above lands it at the bottom, so top and centre shift back up.
    ///
    /// @param cellScale     the block's sizing; ordinary text must yield {0, 0}.
    /// @param cellWidth     one ordinary cell's width in pixels.
    /// @param cellHeight    one ordinary cell's height in pixels (`H`).
    /// @param baseline      the baseline's offset above the cell bottom (`B`).
    /// @param glyphBearingY the glyph's ink top above its baseline (`t`), as the tile reports it.
    [[nodiscard]] virtual GlyphPenOffset penOffsetFor(vtbackend::CellScale const& cellScale,
                                                      int cellWidth,
                                                      int cellHeight,
                                                      int baseline,
                                                      int glyphBearingY) const noexcept = 0;

    [[nodiscard]] virtual GlyphScalingMethod method() const noexcept = 0;

  protected:
    /// The part of the offset both strategies share: the slack a fraction leaves, distributed by
    /// `v` and `h`.
    ///
    /// Data-driven so a third alignment is a row, not a third branch on each axis. The block is
    /// measured as `scale` cells on both axes, which is exact for the one-cell-wide glyphs a
    /// fractional block is written with.
    [[nodiscard]] static constexpr GlyphPenOffset alignmentOffset(vtbackend::CellScale const& cellScale,
                                                                  int cellWidth,
                                                                  int cellHeight) noexcept
    {
        if (!cellScale.hasFraction())
            return {};

        auto const scale = static_cast<double>(cellScale.scale != 0 ? cellScale.scale : 1);
        auto const slack = scale - cellScale.drawFactor();

        auto const vertical = alignmentShare(cellScale.verticalAlignment);
        auto const horizontal = alignmentShare(cellScale.horizontalAlignment);

        // dy is measured from the BOTTOM-aligned placement the formulas above produce, so the
        // vertical share counts backwards: share 1.0 (bottom) moves nothing.
        return GlyphPenOffset {
            .dx = static_cast<int>(std::lround(slack * horizontal * static_cast<double>(cellWidth))),
            .dy = static_cast<int>(std::lround(-slack * (1.0 - vertical) * static_cast<double>(cellHeight))),
        };
    }
};

/// Enlarges by drawing the ordinary-size tile into a larger rectangle. @see GlyphScalingMethod.
class StretchingGlyphScaler final: public GlyphScaler
{
  public:
    [[nodiscard]] GlyphScaleAdjustment adjustmentFor(
        vtbackend::CellScale const& cellScale) const noexcept override
    {
        return GlyphScaleAdjustment { .factor = cellScale.drawFactor(), .requiresRerasterization = false };
    }

    /// The tile is the ORDINARY-size raster magnified, so its bearing is still the 1x one and grows
    /// with the tile -- which is why `t` appears here. @see GlyphScaler::penOffsetFor.
    [[nodiscard]] GlyphPenOffset penOffsetFor(vtbackend::CellScale const& cellScale,
                                              int cellWidth,
                                              int cellHeight,
                                              int baseline,
                                              int glyphBearingY) const noexcept override
    {
        if (cellScale.isOrdinary())
            return {};

        auto const scale = static_cast<double>(cellScale.scale != 0 ? cellScale.scale : 1);
        auto const factor = cellScale.drawFactor();
        auto const dy = ((scale - 1.0) * static_cast<double>(cellHeight))
                        - ((factor - 1.0) * static_cast<double>(baseline + glyphBearingY));

        auto const alignment = alignmentOffset(cellScale, cellWidth, cellHeight);
        return GlyphPenOffset { .dx = alignment.dx, .dy = static_cast<int>(std::lround(dy)) + alignment.dy };
    }

    [[nodiscard]] GlyphScalingMethod method() const noexcept override { return GlyphScalingMethod::Stretch; }
};

/// Enlarges by asking the font for the glyph at a larger point size. @see GlyphScalingMethod.
class RerasterizingGlyphScaler final: public GlyphScaler
{
  public:
    [[nodiscard]] GlyphScaleAdjustment adjustmentFor(
        vtbackend::CellScale const& cellScale) const noexcept override
    {
        auto const factor = cellScale.drawFactor();
        // The factor is still reported so a caller can size the glyph request, but the tile that
        // comes back is ALREADY at the final size -- stretching it again would double the scaling.
        return GlyphScaleAdjustment { .factor = factor, .requiresRerasterization = factor != 1.0 };
    }

    /// The tile came back from the font at `factor x` the point size, so its bearing is ALREADY
    /// scaled and must not be counted again -- which drops the `t` term.
    /// @see GlyphScaler::penOffsetFor.
    [[nodiscard]] GlyphPenOffset penOffsetFor(vtbackend::CellScale const& cellScale,
                                              int cellWidth,
                                              int cellHeight,
                                              int baseline,
                                              int /*glyphBearingY*/) const noexcept override
    {
        if (cellScale.isOrdinary())
            return {};

        auto const scale = static_cast<double>(cellScale.scale != 0 ? cellScale.scale : 1);
        auto const factor = cellScale.drawFactor();
        auto const dy = ((scale - 1.0) * static_cast<double>(cellHeight))
                        - ((factor - 1.0) * static_cast<double>(baseline));

        auto const alignment = alignmentOffset(cellScale, cellWidth, cellHeight);
        return GlyphPenOffset { .dx = alignment.dx, .dy = static_cast<int>(std::lround(dy)) + alignment.dy };
    }

    [[nodiscard]] GlyphScalingMethod method() const noexcept override
    {
        return GlyphScalingMethod::Rerasterize;
    }
};

/// The wire/config names of each method, and the strategy each selects.
///
/// Data-driven so that a third method is a row here plus its implementation, rather than an edit to
/// every place a method is parsed, formatted or chosen.
[[nodiscard]] inline std::string_view nameOf(GlyphScalingMethod method) noexcept
{
    switch (method)
    {
        case GlyphScalingMethod::Stretch: return "stretch";
        case GlyphScalingMethod::Rerasterize: return "rerasterize";
    }
    return "stretch";
}

/// @return the method @p name selects, or nullopt when it names none of them.
[[nodiscard]] inline std::optional<GlyphScalingMethod> methodFromName(std::string_view name) noexcept
{
    if (name == "stretch")
        return GlyphScalingMethod::Stretch;
    if (name == "rerasterize")
        return GlyphScalingMethod::Rerasterize;
    return std::nullopt;
}

/// @return a shared, stateless scaler implementing @p method.
[[nodiscard]] inline GlyphScaler const& glyphScalerFor(GlyphScalingMethod method) noexcept
{
    static auto const stretching = StretchingGlyphScaler {};
    static auto const rerasterizing = RerasterizingGlyphScaler {};
    switch (method)
    {
        case GlyphScalingMethod::Stretch: return stretching;
        case GlyphScalingMethod::Rerasterize: return rerasterizing;
    }
    return stretching;
}

} // namespace vtrasterizer
