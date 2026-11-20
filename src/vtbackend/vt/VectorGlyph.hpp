// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

/// The Glyph Protocol's `vg` payload format (specification section 8.9): SVG's path data and paint model
/// sent as text. Not an SVG document; there is no XML.
///
/// A payload is one or more layers separated by `|`, drawn in order. Each is a header, `:`, and SVG 2 path
/// data:
///
///     fill=nonzero,fg:M4 4h16v16H4Z|stroke=2/butt/miter,#e5534bff:M6 12h12
///     clip=1,nonzero:M0 0h12v24H0Z|fill=nonzero,lin(0 0 24 0 / 0 fg / 1 #e5534b):M2 2h20v20H2Z
///
/// Parsing happens once, at registration, and is strict so that every terminal draws the same thing: any
/// error rejects the whole payload instead of drawing up to it. The limits bound how much path crosses the
/// cell, which is what makes a glyph slow to draw, and are counted the way the reference implementation
/// (Rio) counts them, so a payload one terminal takes the other takes too.
namespace vtbackend
{

/// The most segments one registration may hold, counted after arcs are expanded into cubics, clip paths
/// included. Movetos and closes count too.
inline constexpr size_t MaxVectorSegments = 2048;

/// The most layers one registration may hold, clip layers included.
inline constexpr size_t MaxVectorLayers = 128;

/// The most stops one gradient may have.
inline constexpr size_t MaxVectorGradientStops = 16;

/// How far outside the viewBox a point may lie, in viewBox sizes (the larger of its width and height).
inline constexpr float MaxVectorExtentMultiple = 1.0F;

/// The most path one registration may draw, in viewBox sizes: every line's length and every curve's control
/// polygon, across all layers, plus a stroke's width for each of its segments.
inline constexpr double MaxVectorInkMultiple = 64.0;

/// How much a matrix may stretch or shrink a length: its singular values lie within 1/this and this.
inline constexpr double MaxVectorMatrixStretch = 4096.0;

/// The largest magnitude a viewBox value may have.
inline constexpr float MaxVectorViewBoxValue = 1'000'000.0F;

/// The registration's coordinate system: the rectangle that maps onto the glyph's render span. Y points
/// down, as in SVG.
struct VectorViewBox
{
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    bool operator==(VectorViewBox const&) const = default;
};

/// An affine matrix as SVG's `matrix(a b c d e f)`: (x, y) maps to (a x + c y + e, b x + d y + f).
struct VectorMatrix
{
    float a = 1;
    float b = 0;
    float c = 0;
    float d = 1;
    float e = 0;
    float f = 0;

    bool operator==(VectorMatrix const&) const = default;
};

/// Which points of a path are inside it, as SVG's `fill-rule`.
enum class VectorFillRule : uint8_t
{
    NonZero,
    EvenOdd,
};

/// How a stroke ends, as SVG's `stroke-linecap`.
enum class VectorLineCap : uint8_t
{
    Round,
    Butt,
    Square,
};

/// How a stroke turns a corner, as SVG's `stroke-linejoin`.
enum class VectorLineJoin : uint8_t
{
    Round,
    Bevel,
    Miter,
};

/// How a stroke is drawn. The width is in the layer's units.
struct VectorStrokeStyle
{
    float width = 1;
    VectorLineCap cap = VectorLineCap::Round;
    VectorLineJoin join = VectorLineJoin::Round;
    float miterLimit = 4; ///< Used when join is Miter; at least 1.

    bool operator==(VectorStrokeStyle const&) const = default;
};

/// The cell's foreground colour when the glyph is drawn, with this alpha on top of its own.
struct VectorForeground
{
    uint8_t alpha = 0xFF;

    bool operator==(VectorForeground const&) const = default;
};

/// A fixed sRGB colour, alpha not premultiplied.
struct VectorFixedColor
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0xFF;

    bool operator==(VectorFixedColor const&) const = default;
};

/// A colour that is not a gradient.
using VectorSolid = std::variant<VectorForeground, VectorFixedColor>;

/// What a gradient does outside its 0..1 range, as SVG's `spreadMethod`.
enum class VectorSpread : uint8_t
{
    Pad,
    Reflect,
    Repeat,
};

/// A gradient running from one point to another.
struct VectorLinearGradient
{
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;

    bool operator==(VectorLinearGradient const&) const = default;
};

/// A two-point conical gradient: from the focal circle to the outer one.
struct VectorRadialGradient
{
    float focalX = 0;
    float focalY = 0;
    float focalRadius = 0;
    float centerX = 0;
    float centerY = 0;
    float radius = 0;

    bool operator==(VectorRadialGradient const&) const = default;
};

/// One stop of a gradient.
struct VectorGradientStop
{
    float offset = 0; ///< 0 to 1, never less than the stop before.
    VectorSolid color {};

    bool operator==(VectorGradientStop const&) const = default;
};

/// A gradient: 2 to MaxVectorGradientStops stops along a line or between two circles.
struct VectorGradient
{
    std::variant<VectorLinearGradient, VectorRadialGradient> shape {};
    VectorSpread spread = VectorSpread::Pad;
    VectorMatrix transform {}; ///< Gradient space to the layer's coordinates.
    std::vector<VectorGradientStop> stops;

    bool operator==(VectorGradient const&) const = default;
};

/// What a fill or a stroke is painted with.
using VectorColor = std::variant<VectorSolid, VectorGradient>;

/// A layer that fills its path.
struct VectorFill
{
    VectorFillRule rule = VectorFillRule::NonZero;
    VectorColor color {};

    bool operator==(VectorFill const&) const = default;
};

/// A layer that strokes its path.
struct VectorStroke
{
    VectorStrokeStyle style {};
    VectorColor color {};

    bool operator==(VectorStroke const&) const = default;
};

/// A layer whose path clips the layers after it.
struct VectorClip
{
    VectorFillRule rule = VectorFillRule::NonZero;
    size_t count = 1; ///< How many following layers draw only inside it.

    bool operator==(VectorClip const&) const = default;
};

/// What a layer does with its path.
using VectorLayerOperation = std::variant<VectorFill, VectorStroke, VectorClip>;

/// What one drawing command of a path does.
enum class VectorSegmentKind : uint8_t
{
    MoveTo,  ///< Starts a subpath at points[0].
    LineTo,  ///< A line to points[0].
    QuadTo,  ///< A quadratic Bezier: control point points[0], to points[1].
    CubicTo, ///< A cubic Bezier: control points points[0] and points[1], to points[2].
    Close,   ///< Closes the subpath.
};

/// A point of a path.
struct VectorPoint
{
    float x = 0;
    float y = 0;

    bool operator==(VectorPoint const&) const = default;
};

/// One drawing command, in its layer's coordinates. Arcs do not appear: they are expanded into cubics.
struct VectorSegment
{
    VectorSegmentKind kind = VectorSegmentKind::Close;
    std::array<VectorPoint, 3> points {}; ///< As many as the kind uses; the rest are zero.

    bool operator==(VectorSegment const&) const = default;
};

/// One layer of a glyph.
struct VectorLayer
{
    VectorLayerOperation operation {};
    std::vector<VectorSegment> segments;   ///< In the layer's own coordinates.
    std::optional<VectorMatrix> transform; ///< Layer coordinates to the viewBox; absent, they are the same.

    bool operator==(VectorLayer const&) const = default;
};

/// What a glyph's colours draw from, which decides how its pixels can be kept.
enum class VectorPalette : uint8_t
{
    ForegroundOnly, ///< Every colour is the foreground: an alpha mask, tinted when drawn.
    Mixed,          ///< Fixed colours beside the foreground: pixels that depend on the foreground.
    FixedOnly,      ///< No colour is the foreground: pixels that depend on nothing.
};

/// A validated `vg` registration.
struct VectorGlyph
{
    VectorViewBox viewBox {};
    std::vector<VectorLayer> layers;

    /// @return What the glyph's colours, gradient stops included, draw from.
    [[nodiscard]] VectorPalette palette() const noexcept;

    bool operator==(VectorGlyph const&) const = default;
};

/// Why a payload is refused.
enum class VectorGlyphError : uint8_t
{
    Malformed, ///< Not what the format's grammar allows (`reason=malformed_payload`).
    TooLarge,  ///< Past one of the format's limits (`reason=outline_too_large`).
};

/// @param viewBox A viewBox.
/// @return Whether it is one a registration may have: a positive width and height, and no value beyond
///         MaxVectorViewBoxValue (which a NaN is).
[[nodiscard]] bool isValidVectorViewBox(VectorViewBox const& viewBox) noexcept;

/// Reads a `vb=x,y,w,h` value. Separators are commas or spaces, as in SVG's `viewBox`.
/// @param text The value.
/// @return The viewBox, when it has four numbers, a positive width and height, and no value beyond
///         MaxVectorViewBoxValue.
[[nodiscard]] std::optional<VectorViewBox> parseVectorViewBox(std::string_view text);

/// Reads and judges a `vg` payload against its viewBox.
/// @param payload The payload: printable ASCII, layers separated by `|`.
/// @param viewBox The registration's viewBox.
/// @return The glyph, or why the payload is refused.
[[nodiscard]] std::expected<VectorGlyph, VectorGlyphError> parseVectorGlyph(std::string_view payload,
                                                                            VectorViewBox viewBox);

} // namespace vtbackend
