// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/VectorGlyph.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// The geometry a vector glyph (the Glyph Protocol's `vg` format) is drawn from: curves turned into lines,
/// and strokes turned into the outlines of what they cover. Free of any render target, so it can be tested
/// by itself.
namespace vtrasterizer
{

/// A point of a path.
struct PathPoint
{
    float x = 0;
    float y = 0;

    bool operator==(PathPoint const&) const = default;
};

/// An affine map of the plane, as SVG's `matrix(a b c d e f)`: (x, y) goes to (a x + c y + e, b x + d y + f).
struct Affine
{
    float a = 1;
    float b = 0;
    float c = 0;
    float d = 1;
    float e = 0;
    float f = 0;

    /// @param point A point.
    /// @return Where the map puts it.
    [[nodiscard]] constexpr PathPoint apply(PathPoint point) const noexcept
    {
        return { .x = (a * point.x) + (c * point.y) + e, .y = (b * point.x) + (d * point.y) + f };
    }

    bool operator==(Affine const&) const = default;
};

/// @param matrix A matrix of a vector glyph.
/// @return The same map.
[[nodiscard]] constexpr Affine toAffine(vtbackend::VectorMatrix const& matrix) noexcept
{
    return { .a = matrix.a, .b = matrix.b, .c = matrix.c, .d = matrix.d, .e = matrix.e, .f = matrix.f };
}

/// @param outer The map applied second.
/// @param inner The map applied first.
/// @return The map that applies @p inner and then @p outer.
[[nodiscard]] constexpr Affine compose(Affine const& outer, Affine const& inner) noexcept
{
    return { .a = (outer.a * inner.a) + (outer.c * inner.b),
             .b = (outer.b * inner.a) + (outer.d * inner.b),
             .c = (outer.a * inner.c) + (outer.c * inner.d),
             .d = (outer.b * inner.c) + (outer.d * inner.d),
             .e = (outer.a * inner.e) + (outer.c * inner.f) + outer.e,
             .f = (outer.b * inner.e) + (outer.d * inner.f) + outer.f };
}

/// @param map A map.
/// @return The map that undoes it; nothing for one that flattens the plane onto a line or a point.
[[nodiscard]] std::optional<Affine> inverse(Affine const& map) noexcept;

/// @param map A map.
/// @return The most it stretches a length by.
[[nodiscard]] float largestStretch(Affine const& map) noexcept;

/// Whether a run of lines ends where it began.
enum class PathClosure : uint8_t
{
    Open,
    Closed,
};

/// A run of straight lines: one subpath, its curves flattened.
struct Polyline
{
    std::vector<PathPoint> points;
    PathClosure closure = PathClosure::Open; ///< Closed: a line joins the last point to the first.

    bool operator==(Polyline const&) const = default;
};

/// Turns a path's segments into lines.
/// @param segments The path, in its own coordinates.
/// @param tolerance How far a line may stray from the curve it stands for, in those coordinates.
/// @return One polyline for each subpath that draws something, with no two equal points in a row.
[[nodiscard]] std::vector<Polyline> flattenPath(std::span<vtbackend::VectorSegment const> segments,
                                                float tolerance);

/// Outlines what stroking polylines covers.
///
/// Every polygon returned is closed and they are all wound so that filling them together by the non-zero
/// rule covers the stroke: an open polyline gives one, around it, and a closed one two, its outer and its
/// inner edge.
/// @param polylines The lines to stroke.
/// @param style The stroke's width, caps and joins.
/// @param tolerance How far a line may stray from a round cap or join, in the polylines' coordinates.
/// @return The outlines, as closed polylines.
[[nodiscard]] std::vector<Polyline> strokePolylines(std::span<Polyline const> polylines,
                                                    vtbackend::VectorStrokeStyle const& style,
                                                    float tolerance);

/// Moves every point of @p polylines through @p map.
/// @param polylines The lines.
/// @param map Where their points go.
void transformPolylines(std::span<Polyline> polylines, Affine const& map) noexcept;

} // namespace vtrasterizer
