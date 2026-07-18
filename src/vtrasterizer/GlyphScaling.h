// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

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

/// A geometric adjustment to a rasterized tile: how much larger to draw it, and where.
///
/// Expressed as a multiplier rather than an absolute size so that a strategy does not need to know
/// the cell size, which is what keeps this header free of the renderer.
struct GlyphScaleAdjustment
{
    /// Multiplier applied to the tile's target width and to its x offset.
    unsigned widthFactor = 1;

    /// Multiplier applied to the tile's target height.
    unsigned heightFactor = 1;

    /// Whether the caller must rasterize the glyph afresh at `pointSize * widthFactor` rather than
    /// reuse the tile it already has.
    bool requiresRerasterization = false;
};

/// Decides how a glyph is enlarged. @see GlyphScalingMethod.
///
/// Injected rather than selected inline so that a second strategy is a new implementation and a new
/// row in the method table, not an edit to every place a glyph is drawn.
class GlyphScaler
{
  public:
    virtual ~GlyphScaler() = default;

    /// @param scale the block's scale in cells, 1..7. A scale of 1 must yield no adjustment.
    [[nodiscard]] virtual GlyphScaleAdjustment adjustmentFor(uint8_t scale) const noexcept = 0;

    [[nodiscard]] virtual GlyphScalingMethod method() const noexcept = 0;
};

/// Enlarges by drawing the ordinary-size tile into a larger rectangle. @see GlyphScalingMethod.
class StretchingGlyphScaler final: public GlyphScaler
{
  public:
    [[nodiscard]] GlyphScaleAdjustment adjustmentFor(uint8_t scale) const noexcept override
    {
        auto const factor = static_cast<unsigned>(scale ? scale : 1);
        return GlyphScaleAdjustment { .widthFactor = factor,
                                      .heightFactor = factor,
                                      .requiresRerasterization = false };
    }

    [[nodiscard]] GlyphScalingMethod method() const noexcept override { return GlyphScalingMethod::Stretch; }
};

/// Enlarges by asking the font for the glyph at a larger point size. @see GlyphScalingMethod.
class RerasterizingGlyphScaler final: public GlyphScaler
{
  public:
    [[nodiscard]] GlyphScaleAdjustment adjustmentFor(uint8_t scale) const noexcept override
    {
        auto const factor = static_cast<unsigned>(scale ? scale : 1);
        // The factors are still reported so a caller can size the glyph request, but the tile that
        // comes back is ALREADY at the final size -- stretching it again would double the scaling.
        return GlyphScaleAdjustment { .widthFactor = factor,
                                      .heightFactor = factor,
                                      .requiresRerasterization = factor > 1 };
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
