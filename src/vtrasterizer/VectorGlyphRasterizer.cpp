// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/VectorGlyphRasterizer.hpp>

#include <vtrasterizer/PathCoverage.hpp>
#include <vtrasterizer/PathGeometry.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    /// How far a line may stray from the curve it stands for, in pixels.
    constexpr auto FlatteningTolerance = 0.1F;

    /// A colour as it is composited: the channels multiplied by the alpha, each 0 to 1.
    struct Premultiplied
    {
        float red = 0;
        float green = 0;
        float blue = 0;
        float alpha = 0;
    };

    /// A colour as a payload states it: the channels as they are, each 0 to 1.
    struct Straight
    {
        float red = 0;
        float green = 0;
        float blue = 0;
        float alpha = 0;

        [[nodiscard]] constexpr Premultiplied premultiplied() const noexcept
        {
            return { .red = red * alpha, .green = green * alpha, .blue = blue * alpha, .alpha = alpha };
        }
    };

    /// What `fg` is, for the drawing at hand.
    struct ForegroundInk
    {
        float red = 1;
        float green = 1;
        float blue = 1;
    };

    [[nodiscard]] constexpr float unit(uint8_t value) noexcept
    {
        return static_cast<float>(value) / 255.0F;
    }

    [[nodiscard]] Straight resolve(VectorSolid const& solid, ForegroundInk const& foreground) noexcept
    {
        if (auto const* const fixed = std::get_if<VectorFixedColor>(&solid))
            return { .red = unit(fixed->red),
                     .green = unit(fixed->green),
                     .blue = unit(fixed->blue),
                     .alpha = unit(fixed->alpha) };
        return { .red = foreground.red,
                 .green = foreground.green,
                 .blue = foreground.blue,
                 .alpha = unit(std::get<VectorForeground>(solid).alpha) };
    }

    /// Paints the pixels of a layer: one colour for all of them, or a gradient's colour at each.
    class Paint
    {
      public:
        /// @param color The layer's colour.
        /// @param toPixels The map from the layer's coordinates to the window's pixels.
        /// @param foreground What `fg` is.
        Paint(VectorColor const& color, Affine const& toPixels, ForegroundInk const& foreground)
        {
            if (auto const* const solid = std::get_if<VectorSolid>(&color))
            {
                _solid = resolve(*solid, foreground).premultiplied();
                return;
            }
            auto const& gradient = std::get<VectorGradient>(color);
            _gradient = &gradient;
            for (auto const& stop: gradient.stops)
                _stops.push_back(resolve(stop.color, foreground));
            // Gradient space to the layer's coordinates, then on to pixels; a pixel is looked up backwards.
            _fromPixels = inverse(compose(toPixels, toAffine(gradient.transform)));
            // One with no extent, or that cannot be looked up, paints its last stop, as SVG has it.
            if (!_fromPixels || !hasExtent(gradient))
                _solid = _stops.back().premultiplied();
        }

        /// @return The colour of the pixel at column @p x of row @p y; nothing where a gradient paints none.
        [[nodiscard]] std::optional<Premultiplied> at(size_t x, size_t y) const noexcept
        {
            if (_solid)
                return _solid;
            auto const point =
                _fromPixels->apply({ .x = static_cast<float>(x) + 0.5F, .y = static_cast<float>(y) + 0.5F });
            auto const position =
                std::visit([&](auto const& shape) { return positionOn(shape, point); }, _gradient->shape);
            return position.transform([&](float t) { return colorAt(spread(t)).premultiplied(); });
        }

      private:
        [[nodiscard]] static bool hasExtent(VectorGradient const& gradient) noexcept
        {
            if (auto const* const linear = std::get_if<VectorLinearGradient>(&gradient.shape))
                return linear->x1 != linear->x2 || linear->y1 != linear->y2;
            auto const& radial = std::get<VectorRadialGradient>(gradient.shape);
            return radial.focalX != radial.centerX || radial.focalY != radial.centerY
                   || radial.focalRadius != radial.radius;
        }

        /// @return How far along the line from its first point to its second @p point lies.
        [[nodiscard]] static std::optional<float> positionOn(VectorLinearGradient const& linear,
                                                             PathPoint point) noexcept
        {
            auto const dx = linear.x2 - linear.x1;
            auto const dy = linear.y2 - linear.y1;
            return (((point.x - linear.x1) * dx) + ((point.y - linear.y1) * dy)) / ((dx * dx) + (dy * dy));
        }

        /// @return The circle, of those between the focal one (0) and the outer one (1) and beyond, that
        ///         @p point lies on: the largest such with a radius that is not negative, as a two-point
        ///         conical gradient is drawn. Nothing where no circle passes.
        [[nodiscard]] static std::optional<float> positionOn(VectorRadialGradient const& radial,
                                                             PathPoint point) noexcept
        {
            auto const centerX = radial.centerX - radial.focalX;
            auto const centerY = radial.centerY - radial.focalY;
            auto const pointX = point.x - radial.focalX;
            auto const pointY = point.y - radial.focalY;
            auto const growth = radial.radius - radial.focalRadius;
            // |point - t center| = focalRadius + t growth, squared: a t^2 - 2 b t + c = 0.
            auto const a = (centerX * centerX) + (centerY * centerY) - (growth * growth);
            auto const b = (pointX * centerX) + (pointY * centerY) + (radial.focalRadius * growth);
            auto const c = (pointX * pointX) + (pointY * pointY) - (radial.focalRadius * radial.focalRadius);
            auto const radiusAt = [&](float t) {
                return radial.focalRadius + (t * growth);
            };
            if (std::abs(a) < 1e-9F)
            {
                // The circles grow exactly as fast as they move: one answer.
                if (b == 0)
                    return std::nullopt;
                auto const t = c / (2 * b);
                return radiusAt(t) >= 0 ? std::optional { t } : std::nullopt;
            }
            auto const discriminant = (b * b) - (a * c);
            if (discriminant < 0)
                return std::nullopt;
            auto const root = std::sqrt(discriminant);
            auto const first = (b + root) / a;
            auto const second = (b - root) / a;
            auto const smaller = std::min(first, second);
            auto const larger = std::max(first, second);
            if (radiusAt(larger) >= 0)
                return larger;
            return radiusAt(smaller) >= 0 ? std::optional { smaller } : std::nullopt;
        }

        /// @return @p t brought into 0 to 1, as the gradient's spread says.
        [[nodiscard]] float spread(float t) const noexcept
        {
            switch (_gradient->spread)
            {
                case VectorSpread::Pad: return std::clamp(t, 0.0F, 1.0F);
                case VectorSpread::Repeat: return t - std::floor(t);
                case VectorSpread::Reflect: {
                    auto const phase = t - (2 * std::floor(t / 2));
                    return phase <= 1 ? phase : 2 - phase;
                }
            }
            return t;
        }

        /// @return The gradient's colour at @p t, between the stops either side of it.
        [[nodiscard]] Straight colorAt(float t) const noexcept
        {
            auto const& stops = _gradient->stops;
            // The first stop past t; of stops sharing an offset, the later one is the colour from there on.
            auto const after = std::ranges::upper_bound(stops, t, {}, &VectorGradientStop::offset);
            if (after == stops.begin())
                return _stops.front();
            if (after == stops.end())
                return _stops.back();
            auto const index = static_cast<size_t>(after - stops.begin());
            auto const& from = _stops[index - 1];
            auto const& to = _stops[index];
            auto const share = (t - stops[index - 1].offset) / (after->offset - stops[index - 1].offset);
            auto const mix = [&](float a, float b) {
                return a + ((b - a) * share);
            };
            return { .red = mix(from.red, to.red),
                     .green = mix(from.green, to.green),
                     .blue = mix(from.blue, to.blue),
                     .alpha = mix(from.alpha, to.alpha) };
        }

        std::optional<Premultiplied> _solid;       ///< Set when every pixel has the same colour.
        VectorGradient const* _gradient = nullptr; ///< The gradient, otherwise.
        std::vector<Straight> _stops;              ///< Its stops' colours, the foreground resolved.
        std::optional<Affine> _fromPixels;         ///< Pixels to gradient space.
    };

    /// The picture being painted: the window's pixels, premultiplied.
    class Canvas
    {
      public:
        Canvas(size_t width, size_t height): _width { width }, _pixels(width * height) {}

        /// Paints @p paint over the pixels @p coverage covers, where @p clip lets it.
        void paintOver(std::span<float const> coverage,
                       std::span<float const> clip,
                       Paint const& paint) noexcept
        {
            for (auto const index: std::views::iota(size_t { 0 }, _pixels.size()))
            {
                auto const share = coverage[index] * (clip.empty() ? 1.0F : clip[index]);
                if (share <= 0)
                    continue;
                auto const color = paint.at(index % _width, index / _width);
                if (!color)
                    continue;
                auto& pixel = _pixels[index];
                auto const kept = 1 - (color->alpha * share);
                pixel = { .red = (color->red * share) + (pixel.red * kept),
                          .green = (color->green * share) + (pixel.green * kept),
                          .blue = (color->blue * share) + (pixel.blue * kept),
                          .alpha = (color->alpha * share) + (pixel.alpha * kept) };
            }
        }

        [[nodiscard]] RasterizedGlyph take(GlyphPixels format) const
        {
            auto const byteOf = [](float value) {
                return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
            };
            auto result = RasterizedGlyph { .format = format, .pixels = {} };
            result.pixels.reserve(_pixels.size() * (format == GlyphPixels::Color ? 4 : 1));
            for (auto const& pixel: _pixels)
            {
                if (format == GlyphPixels::Color)
                    result.pixels.insert(result.pixels.end(),
                                         { byteOf(pixel.red), byteOf(pixel.green), byteOf(pixel.blue) });
                result.pixels.push_back(byteOf(pixel.alpha));
            }
            return result;
        }

      private:
        size_t _width;
        std::vector<Premultiplied> _pixels;
    };

    /// @return What a layer covers of the window: its path filled, or the outline of its stroke.
    [[nodiscard]] std::vector<float> coverageOf(VectorLayer const& layer,
                                                Affine const& toPixels,
                                                ImageSize window)
    {
        // Flattened, and stroked, in the layer's own coordinates: a matrix that scales its axes unequally
        // then draws the elliptical pen SVG draws. The tolerance is a pixel's, carried back there.
        auto const stretch = largestStretch(toPixels);
        auto const tolerance = stretch > 0 ? FlatteningTolerance / stretch : FlatteningTolerance;
        auto polygons = flattenPath(layer.segments, tolerance);
        auto rule = VectorFillRule::NonZero;
        if (auto const* const stroke = std::get_if<VectorStroke>(&layer.operation))
            polygons = strokePolylines(polygons, stroke->style, tolerance);
        else if (auto const* const fill = std::get_if<VectorFill>(&layer.operation))
            rule = fill->rule;
        else
            rule = std::get<VectorClip>(layer.operation).rule;
        transformPolylines(polygons, toPixels);
        return pathCoverage(polygons, rule, window);
    }
} // namespace

RasterizedGlyph rasterizeVectorGlyph(VectorGlyph const& glyph,
                                     GlyphSpanTransform const& placement,
                                     ImageSize window,
                                     GlyphWindowOrigin origin,
                                     RGBColor foreground)
{
    // As coverage, the foreground is drawn white and its alphas are what is kept; in colour, it is the
    // colour given.
    auto const format =
        glyph.palette() == VectorPalette::ForegroundOnly ? GlyphPixels::Coverage : GlyphPixels::Color;
    auto const ink = format == GlyphPixels::Coverage ? ForegroundInk {}
                                                     : ForegroundInk { .red = unit(foreground.red),
                                                                       .green = unit(foreground.green),
                                                                       .blue = unit(foreground.blue) };
    auto const viewBoxToPixels = Affine { .a = placement.sx,
                                          .b = 0,
                                          .c = 0,
                                          .d = placement.sy,
                                          .e = placement.tx - static_cast<float>(origin.x),
                                          .f = placement.ty - static_cast<float>(origin.y) };

    auto canvas = Canvas { unbox<size_t>(window.width), unbox<size_t>(window.height) };
    // The clip in force, and how many more layers it covers.
    auto clip = std::vector<float> {};
    auto clipped = size_t { 0 };
    for (auto const& layer: glyph.layers)
    {
        auto const toPixels =
            layer.transform ? compose(viewBoxToPixels, toAffine(*layer.transform)) : viewBoxToPixels;
        auto coverage = coverageOf(layer, toPixels, window);
        if (auto const* const clipLayer = std::get_if<VectorClip>(&layer.operation))
        {
            // A clip path that encloses no area covers nothing, and so hides what it clips.
            clip = std::move(coverage);
            clipped = clipLayer->count;
            continue;
        }

        auto const& color = std::holds_alternative<VectorFill>(layer.operation)
                                ? std::get<VectorFill>(layer.operation).color
                                : std::get<VectorStroke>(layer.operation).color;
        canvas.paintOver(coverage,
                         clipped > 0 ? std::span<float const> { clip } : std::span<float const> {},
                         Paint { color, toPixels, ink });
        if (clipped > 0)
            --clipped;
    }
    return canvas.take(format);
}

} // namespace vtrasterizer
