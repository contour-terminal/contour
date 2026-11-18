// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/PathCoverage.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    /// Accumulates the signed area each edge of a path sweeps over the pixels of a window.
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

        /// Adds the edges of a polygon, the one from its last point back to its first included.
        /// @param points The polygon.
        /// @param origin Where in the points' coordinates the accumulator's window starts.
        void addPolygon(std::span<PathPoint const> points, PathPoint origin) noexcept
        {
            if (points.size() < 2)
                return;
            auto const local = [&](PathPoint point) {
                return PathPoint { .x = point.x - origin.x, .y = point.y - origin.y };
            };
            for (auto const index: std::views::iota(size_t { 0 }, points.size()))
                addClipped(local(points[index]), local(points[(index + 1) % points.size()]));
        }

        /// @return The share of every pixel that is inside by @p rule, top row first.
        [[nodiscard]] std::vector<float> coverage(VectorFillRule rule) const
        {
            auto pixels = std::vector<float>(_width * _height);
            for (auto const row: std::views::iota(size_t { 0 }, _height))
            {
                auto winding = 0.0F;
                for (auto const column: std::views::iota(size_t { 0 }, _width))
                {
                    winding += _area[(row * _stride) + column];
                    pixels[(row * _width) + column] = insideShare(winding, rule);
                }
            }
            return pixels;
        }

      private:
        /// @return How much of a pixel is inside, given its area-weighted winding.
        [[nodiscard]] static float insideShare(float winding, VectorFillRule rule) noexcept
        {
            auto const turns = std::abs(winding);
            if (rule == VectorFillRule::NonZero)
                return std::min(turns, 1.0F);
            // Inside where the winding is odd: a triangle wave over it, 1 at every odd number.
            auto const phase = std::fmod(turns, 2.0F);
            return phase <= 1.0F ? phase : 2.0F - phase;
        }

        /// Adds the edge from @p from to @p to, cut down to the window. The part above or below it sweeps no
        /// pixel and is dropped; the part left or right of it is moved onto the border it is beyond, which
        /// keeps what it contributes to the pixels inside -- everything, from the left, and nothing, from
        /// the right.
        void addClipped(PathPoint from, PathPoint to) noexcept
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
            // last one skip most of the path here.
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
        void accumulate(PathPoint from, PathPoint to, float direction) noexcept
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
    };
} // namespace

std::vector<float> CoverageMask::expanded(ImageSize window) const
{
    auto const windowWidth = unbox<size_t>(window.width);
    auto pixels = std::vector<float>(windowWidth * unbox<size_t>(window.height), 0.0F);
    for (auto const row: std::views::iota(size_t { 0 }, height))
        std::ranges::copy(std::span { values }.subspan(row * width, width),
                          pixels.begin() + static_cast<std::ptrdiff_t>(((top + row) * windowWidth) + left));
    return pixels;
}

CoverageMask pathCoverage(std::span<Polyline const> polygons, VectorFillRule rule, ImageSize window)
{
    // Only the pixels the polygons' bounds reach can be covered: a glyph of many small layers, as an emoji
    // is, would otherwise pay for the whole window once a layer.
    auto const windowWidth = static_cast<float>(unbox<size_t>(window.width));
    auto const windowHeight = static_cast<float>(unbox<size_t>(window.height));
    auto minX = windowWidth;
    auto minY = windowHeight;
    auto maxX = 0.0F;
    auto maxY = 0.0F;
    for (auto const& polygon: polygons)
        for (auto const& point: polygon.points)
        {
            // A point that is not a number bounds nothing; its edges are dropped when they are added.
            if (!std::isfinite(point.x) || !std::isfinite(point.y))
                continue;
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    auto const left = std::floor(std::clamp(minX, 0.0F, windowWidth));
    auto const top = std::floor(std::clamp(minY, 0.0F, windowHeight));
    auto const right = std::ceil(std::clamp(maxX, 0.0F, windowWidth));
    auto const bottom = std::ceil(std::clamp(maxY, 0.0F, windowHeight));
    if (!(left < right && top < bottom))
        return {};

    auto mask = CoverageMask { .left = static_cast<size_t>(left),
                               .top = static_cast<size_t>(top),
                               .width = static_cast<size_t>(right - left),
                               .height = static_cast<size_t>(bottom - top),
                               .values = {} };
    auto accumulator = CoverageAccumulator { mask.width, mask.height };
    for (auto const& polygon: polygons)
        accumulator.addPolygon(polygon.points, PathPoint { .x = left, .y = top });
    mask.values = accumulator.coverage(rule);
    return mask;
}

} // namespace vtrasterizer
