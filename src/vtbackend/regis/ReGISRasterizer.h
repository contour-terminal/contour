// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Image.h>
#include <vtbackend/regis/ReGISTables.h>

#include <crispy/point.h>

#include <cstdint>
#include <span>
#include <vector>

namespace vtbackend::regis
{

/// The drawing "pen" a ReGIS primitive uses: colour, writing mode, line pattern and width.
///
/// The pen is passed to each primitive rather than held by the rasterizer, so the canvas stays the
/// only mutable state and every geometry routine is testable in isolation. The line pattern phase
/// (which bit of @ref pattern comes next) is the rasterizer's, reset via @ref ReGISRasterizer::resetPattern.
struct Pen
{
    RGBColor color { 255, 255, 255 };          ///< Foreground colour.
    WritingMode mode { WritingMode::Replace }; ///< How a plotted pixel combines with the canvas.
    uint8_t pattern { DefaultPattern };        ///< 8-bit repeating line pattern (a set bit paints).
    unsigned patternMultiplier { 1 };          ///< Each pattern bit repeats this many pixels.
    int lineWidth { 1 };                       ///< Line thickness in pixels (>=1).

    // Shading (W(S...)): when enabled, every plotted pixel is extended with a solid span to a
    // reference line, filling the swept area between a primitive and that line.
    bool shade { false };         ///< Whether shading is active.
    bool shadeVertical { false }; ///< Reference is a vertical line (span in x) vs. horizontal (span in y).
    int shadeReference { 0 };     ///< The reference coordinate (x if @ref shadeVertical, else y).
};

/// A pure RGBA drawing canvas with the ReGIS geometry primitives.
///
/// Owns a tightly-packed RGBA byte buffer (@ref Image::Data). It carries no terminal dependency, so
/// every primitive can be unit-tested with direct pixel assertions. The parser maps ReGIS user
/// coordinates to canvas pixels before calling in, so the rasterizer works purely in pixel space.
class ReGISRasterizer
{
  public:
    /// Constructs a canvas of @p size pixels, cleared to fully transparent.
    explicit ReGISRasterizer(ImageSize size);

    /// @return the canvas pixel dimensions.
    [[nodiscard]] ImageSize size() const noexcept { return _size; }

    /// @return the raw RGBA pixel buffer (4 bytes per pixel, row-major, top-down).
    [[nodiscard]] Image::Data const& data() const noexcept { return _buffer; }

    /// @return the raw RGBA pixel buffer, for moving out on commit.
    [[nodiscard]] Image::Data& data() noexcept { return _buffer; }

    /// Resizes the canvas to @p size and clears it to fully transparent.
    void resize(ImageSize size);

    /// Fills the whole canvas with @p color.
    void eraseTo(RGBAColor color) noexcept;

    /// @return the RGBA colour at pixel (@p x, @p y), or transparent if out of bounds.
    [[nodiscard]] RGBAColor at(int x, int y) const noexcept;

    /// Restarts the line-pattern phase, so the next patterned primitive begins on the pattern's
    /// first bit. Called at the start of a new drawing path.
    void resetPattern() noexcept { _patternPhase = 0; }

    /// Plots a single point (the @c V[] dot), honouring the pen's colour, mode and line width.
    void plotDot(Pen const& pen, crispy::point p) noexcept;

    /// Draws a straight line from @p from to @p to (patterned Bresenham).
    void plotLine(Pen const& pen, crispy::point from, crispy::point to) noexcept;

    /// Draws a full circle of @p radius pixels centred at @p center (midpoint circle).
    void plotCircle(Pen const& pen, crispy::point center, int radius) noexcept;

    /// Draws a circular arc centred at @p center through @p radius, from @p startDegrees sweeping
    /// @p sweepDegrees (a negative sweep goes clockwise).
    void plotArc(
        Pen const& pen, crispy::point center, int radius, double startDegrees, double sweepDegrees) noexcept;

    /// Fills the polygon with vertices @p points using the even-odd (scanline) rule.
    void fillPolygon(Pen const& pen, std::span<crispy::point const> points) noexcept;

    /// Draws a smooth curve interpolating @p points (a Catmull-Rom spline flattened to segments).
    /// @param closed Whether the curve wraps from the last point back to the first.
    void plotCurve(Pen const& pen, std::span<crispy::point const> points, bool closed) noexcept;

    /// Composites an alpha-coverage glyph mask at @p origin using the pen colour.
    ///
    /// @param size The mask dimensions in pixels.
    /// @param coverage One byte per pixel (0 = transparent, 255 = opaque), row-major. Straight-alpha
    ///                 "over" compositing gives anti-aliased text against the (possibly transparent)
    ///                 canvas.
    void blendCoverage(Pen const& pen,
                       crispy::point origin,
                       ImageSize size,
                       std::span<uint8_t const> coverage) noexcept;

  private:
    /// Writes @p color at (@p x, @p y) combining it per @p mode; ignores out-of-bounds pixels.
    void blend(int x, int y, RGBColor color, WritingMode mode) noexcept;

    /// Composites @p color at (@p x, @p y) with fractional @p coverage (0..255) using straight-alpha
    /// "over" blending; ignores out-of-bounds pixels.
    void compositePixel(int x, int y, RGBColor color, WritingMode mode, uint8_t coverage) noexcept;

    /// Stamps the pen (a @c lineWidth x @c lineWidth brush) centred at (@p x, @p y).
    void stamp(Pen const& pen, int x, int y) noexcept;

    /// Advances the pattern phase and returns whether the pen paints at this step.
    [[nodiscard]] bool patternPaintsNext(Pen const& pen) noexcept;

    ImageSize _size;
    Image::Data _buffer;        ///< RGBA, 4 bytes per pixel, row-major top-down.
    unsigned _patternPhase = 0; ///< Index of the next pattern step (in pen pixels).
};

} // namespace vtbackend::regis
