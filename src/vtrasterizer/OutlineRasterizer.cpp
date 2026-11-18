// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/OutlineRasterizer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    struct Point
    {
        float x = 0;
        float y = 0;
    };

    [[nodiscard]] constexpr Point midpoint(Point a, Point b) noexcept
    {
        return { .x = (a.x + b.x) / 2, .y = (a.y + b.y) / 2 };
    }

    /// The most line segments one curve is flattened into, however large it is drawn.
    constexpr auto MaxCurveSegments = 64;

    /// Accumulates the signed area each edge of a path sweeps over the pixels of a window. Summing a row
    /// left to right then gives each pixel's winding-weighted coverage, which is how font rasterizers get
    /// exact antialiasing out of one pass over the edges.
    class CoverageAccumulator
    {
      public:
        CoverageAccumulator(size_t width, size_t height):
            _width { width },
            _height { height },
            // Two columns more than the window: an edge clamped to the right border deposits there, where
            // it is never read.
            _stride { width + 2 },
            _area(_stride * height, 0.0F)
        {
        }

        void moveTo(Point to) noexcept { _pen = to; }

        void lineTo(Point to) noexcept
        {
            addClipped(_pen, to);
            _pen = to;
        }

        void quadTo(Point control, Point to) noexcept
        {
            // How far the curve strays from its chord decides how many lines stand in for it.
            auto const dx = _pen.x - (2 * control.x) + to.x;
            auto const dy = _pen.y - (2 * control.y) + to.y;
            auto const deviation = (dx * dx) + (dy * dy);
            auto const from = _pen;
            if (deviation >= 0.333F)
            {
                auto const segments =
                    std::min(MaxCurveSegments, 1 + static_cast<int>(std::sqrt(std::sqrt(3 * deviation))));
                for (auto const step: std::views::iota(1, segments))
                {
                    auto const t = static_cast<float>(step) / static_cast<float>(segments);
                    auto const u = 1 - t;
                    lineTo({ .x = (u * u * from.x) + (2 * u * t * control.x) + (t * t * to.x),
                             .y = (u * u * from.y) + (2 * u * t * control.y) + (t * t * to.y) });
                }
            }
            lineTo(to);
        }

        /// @return The coverage of every pixel of the window, top row first.
        [[nodiscard]] std::vector<uint8_t> coverage() const
        {
            auto pixels = std::vector<uint8_t>(_width * _height);
            for (auto const row: std::views::iota(size_t { 0 }, _height))
            {
                auto winding = 0.0F;
                for (auto const column: std::views::iota(size_t { 0 }, _width))
                {
                    winding += _area[(row * _stride) + column];
                    pixels[(row * _width) + column] =
                        static_cast<uint8_t>(std::lround(std::min(std::abs(winding), 1.0F) * 255.0F));
                }
            }
            return pixels;
        }

      private:
        /// Adds the edge from @p from to @p to, cut down to the window. The part above or below it sweeps no
        /// pixel and is dropped; the part left or right of it is moved onto the border it is beyond, which
        /// keeps what it contributes to the pixels inside -- everything, from the left, and nothing, from
        /// the right.
        void addClipped(Point from, Point to) noexcept
        {
            if (!(std::isfinite(from.x) && std::isfinite(from.y) && std::isfinite(to.x)
                  && std::isfinite(to.y))
                || from.y == to.y)
                return;

            auto const direction = from.y < to.y ? 1.0F : -1.0F;
            if (from.y > to.y)
                std::swap(from, to);
            auto const top = 0.0F;
            auto const bottom = static_cast<float>(_height);
            if (to.y <= top || from.y >= bottom)
                return;

            // An edge wholly right of the window sweeps none of its pixels: the tiles left of a wide glyph's
            // last one skip most of the outline here.
            auto const right = static_cast<float>(_width);
            if (from.x >= right && to.x >= right)
                return;

            auto const slope = (to.x - from.x) / (to.y - from.y);
            if (from.y < top)
                from = { .x = from.x + (slope * (top - from.y)), .y = top };
            if (to.y > bottom)
                to = { .x = from.x + (slope * (bottom - from.y)), .y = bottom };

            // Cut where the edge crosses the left and the right border, so each piece lies wholly on one
            // side of each and clamping it moves no part of it across a pixel.
            auto cuts = std::array { from.y, from.y, from.y, to.y };
            if (slope != 0)
            {
                auto const crossing = [&](float border) {
                    return std::clamp(from.y + ((border - from.x) / slope), from.y, to.y);
                };
                cuts[1] = crossing(0.0F);
                cuts[2] = crossing(right);
                std::ranges::sort(cuts);
            }
            for (auto const index: std::views::iota(size_t { 0 }, cuts.size() - 1))
            {
                auto const y0 = cuts[index];
                auto const y1 = cuts[index + 1];
                if (y0 >= y1)
                    continue;
                // Judged at the piece's middle, which is on the side all of it is on.
                auto const middle = from.x + (slope * (((y0 + y1) / 2) - from.y));
                auto const xAt = [&](float y) {
                    if (middle <= 0)
                        return 0.0F;
                    if (middle >= right)
                        return right;
                    return std::clamp(from.x + (slope * (y - from.y)), 0.0F, right);
                };
                accumulate({ .x = xAt(y0), .y = y0 }, { .x = xAt(y1), .y = y1 }, direction);
            }
        }

        /// Adds an edge that lies inside the window, running downwards from @p from to @p to.
        void accumulate(Point from, Point to, float direction) noexcept
        {
            auto const slope = (to.x - from.x) / (to.y - from.y);
            auto x = from.x;
            auto const firstRow = static_cast<size_t>(from.y);
            auto const lastRow = std::min(_height, static_cast<size_t>(std::ceil(to.y)));
            for (auto const row: std::views::iota(firstRow, lastRow))
            {
                auto const rowTop = static_cast<float>(row);
                auto const dy = std::min(rowTop + 1, to.y) - std::max(rowTop, from.y);
                // Kept inside the window: the endpoints are, but stepping from one to the other in floats
                // can land a hair outside, and a pixel index must not.
                auto const xNext = std::clamp(x + (slope * dy), 0.0F, static_cast<float>(_width));
                auto const d = dy * direction;
                auto const [x0, x1] = std::minmax(x, xNext);
                auto const x0Floor = std::floor(x0);
                auto const x0Index = static_cast<size_t>(x0Floor);
                auto const x1Ceil = std::ceil(x1);
                auto const x1Index = static_cast<size_t>(x1Ceil);
                auto const cells = std::span { _area }.subspan(row * _stride, _stride);

                if (x1Index <= x0Index + 1)
                {
                    // The edge stays within one pixel on this row.
                    auto const xMiddle = (0.5F * (x + xNext)) - x0Floor;
                    cells[x0Index] += d - (d * xMiddle);
                    cells[x0Index + 1] += d * xMiddle;
                }
                else
                {
                    // It crosses several: each takes the share of the row's area the edge leaves it.
                    auto const s = 1 / (x1 - x0);
                    auto const x0Fraction = x0 - x0Floor;
                    auto const a0 = 0.5F * s * (1 - x0Fraction) * (1 - x0Fraction);
                    auto const x1Fraction = x1 - x1Ceil + 1;
                    auto const am = 0.5F * s * x1Fraction * x1Fraction;
                    cells[x0Index] += d * a0;
                    if (x1Index == x0Index + 2)
                        cells[x0Index + 1] += d * (1 - a0 - am);
                    else
                    {
                        auto const a1 = s * (1.5F - x0Fraction);
                        cells[x0Index + 1] += d * (a1 - a0);
                        for (auto const column: std::views::iota(x0Index + 2, x1Index - 1))
                            cells[column] += d * s;
                        auto const a2 = a1 + (static_cast<float>(x1Index - x0Index - 3) * s);
                        cells[x1Index - 1] += d * (1 - a2 - am);
                    }
                    cells[x1Index] += d * am;
                }
                x = xNext;
            }
        }

        size_t _width;
        size_t _height;
        size_t _stride;
        std::vector<float> _area;
        Point _pen {};
    };

    /// Walks one closed contour by the TrueType rules.
    void walkContour(std::span<GlyphOutlinePoint const> points,
                     GlyphSpanTransform const& transform,
                     OutlineWindowOrigin origin,
                     CoverageAccumulator& path)
    {
        if (points.empty())
            return;

        auto const place = [&](GlyphOutlinePoint const& point) {
            return Point { .x = transform.tx + (static_cast<float>(point.x) * transform.sx)
                                - static_cast<float>(origin.x),
                           .y = transform.ty - (static_cast<float>(point.y) * transform.sy)
                                - static_cast<float>(origin.y) };
        };
        auto const onCurve = [](GlyphOutlinePoint const& point) {
            return point.kind == GlyphPointKind::OnCurve;
        };

        // A contour may open with an off-curve point. It then starts at its last point, when that one is
        // on the curve, and halfway between its first and last otherwise.
        auto const& first = points.front();
        auto const& last = points.back();
        auto const start = onCurve(first)  ? place(first)
                           : onCurve(last) ? place(last)
                                           : midpoint(place(first), place(last));
        // The point the contour starts at is not walked again: the first, or the last when that is it.
        auto const walked = onCurve(first)  ? points.subspan(1)
                            : onCurve(last) ? points.first(points.size() - 1)
                                            : points;

        path.moveTo(start);
        auto control = std::optional<Point> {};
        for (auto const& point: walked)
        {
            auto const position = place(point);
            if (onCurve(point))
            {
                if (control)
                    path.quadTo(*control, position);
                else
                    path.lineTo(position);
                control.reset();
            }
            else
            {
                // Two off-curve points in a row imply an on-curve one halfway between them.
                if (control)
                    path.quadTo(*control, midpoint(*control, position));
                control = position;
            }
        }
        // Close the contour: with the curve still pending, or with a line.
        if (control)
            path.quadTo(*control, start);
        else
            path.lineTo(start);
    }
} // namespace

std::vector<uint8_t> rasterizeOutline(GlyphOutline const& outline,
                                      GlyphSpanTransform const& transform,
                                      ImageSize window,
                                      OutlineWindowOrigin origin)
{
    auto path = CoverageAccumulator { unbox<size_t>(window.width), unbox<size_t>(window.height) };
    for (auto const index: std::views::iota(size_t { 0 }, outline.contourEnds.size()))
        walkContour(outline.contour(index), transform, origin, path);
    return path.coverage();
}

} // namespace vtrasterizer
