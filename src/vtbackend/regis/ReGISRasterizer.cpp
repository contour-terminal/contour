// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISRasterizer.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using crispy::point;

namespace vtbackend::regis
{

namespace
{
    constexpr int stampHalfLow(int width) noexcept
    {
        return width / 2;
    }

    /// Upper bound on the number of one-pixel steps (and on a circle radius) any single primitive
    /// will walk. Arc sweep angles, curve control points and circle radii all derive from unbounded
    /// wire input; without this cap a hostile value would spin the rasterizer for many seconds and, in
    /// the arc case, overflow the double->int step-count cast (undefined behaviour). The bound is far
    /// larger than a whole-canvas primitive ever needs, so it never affects legitimate drawing.
    constexpr int MaxPlotSteps = 1 << 16;
} // namespace

ReGISRasterizer::ReGISRasterizer(ImageSize size): _size { size }
{
    _buffer.resize(size.area() * 4, 0);
}

void ReGISRasterizer::resize(ImageSize size)
{
    _size = size;
    _buffer.assign(size.area() * 4, 0);
    _patternPhase = 0;
}

void ReGISRasterizer::eraseTo(RGBAColor color) noexcept
{
    for (auto i = size_t { 0 }; i < _buffer.size(); i += 4)
    {
        _buffer[i] = color.red();
        _buffer[i + 1] = color.green();
        _buffer[i + 2] = color.blue();
        _buffer[i + 3] = color.alpha();
    }
}

RGBAColor ReGISRasterizer::at(int x, int y) const noexcept
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    if (x < 0 || y < 0 || x >= w || y >= h)
        return RGBAColor { 0 };
    auto const i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return RGBAColor { _buffer[i], _buffer[i + 1], _buffer[i + 2], _buffer[i + 3] };
}

void ReGISRasterizer::blend(int x, int y, RGBColor color, WritingMode mode) noexcept
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    auto const i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    switch (mode)
    {
        case WritingMode::Replace:
        case WritingMode::Overlay:
            _buffer[i] = color.red;
            _buffer[i + 1] = color.green;
            _buffer[i + 2] = color.blue;
            _buffer[i + 3] = 0xFF;
            break;
        case WritingMode::Erase: _buffer[i] = _buffer[i + 1] = _buffer[i + 2] = _buffer[i + 3] = 0; break;
        case WritingMode::Complement:
            _buffer[i] = static_cast<uint8_t>(255 - _buffer[i]);
            _buffer[i + 1] = static_cast<uint8_t>(255 - _buffer[i + 1]);
            _buffer[i + 2] = static_cast<uint8_t>(255 - _buffer[i + 2]);
            _buffer[i + 3] = 0xFF;
            break;
    }
}

void ReGISRasterizer::stamp(Pen const& pen, int x, int y) noexcept
{
    auto const width = std::max(1, pen.lineWidth);
    if (width == 1)
        blend(x, y, pen.color, pen.mode);
    else
    {
        auto const half = stampHalfLow(width);
        for (auto dy = -half; dy < width - half; ++dy)
            for (auto dx = -half; dx < width - half; ++dx)
                blend(x + dx, y + dy, pen.color, pen.mode);
    }
    if (pen.shade)
    {
        // Fill the span from this pixel to the reference line, so the swept area under the primitive
        // becomes solid. The reference is a wire-driven pixel coordinate, so clamp the span to the
        // canvas: off-canvas pixels are no-ops, and an unclamped span would let a far-off reference
        // spin here for every stamped pixel.
        auto const w = unbox<int>(_size.width);
        auto const h = unbox<int>(_size.height);
        if (pen.shadeVertical)
            for (auto sx = std::max(0, std::min(x, pen.shadeReference));
                 sx <= std::min(w - 1, std::max(x, pen.shadeReference));
                 ++sx)
                blend(sx, y, pen.color, pen.mode);
        else
            for (auto sy = std::max(0, std::min(y, pen.shadeReference));
                 sy <= std::min(h - 1, std::max(y, pen.shadeReference));
                 ++sy)
                blend(x, sy, pen.color, pen.mode);
    }
}

bool ReGISRasterizer::patternPaintsNext(Pen const& pen) noexcept
{
    auto const multiplier = std::max(1u, pen.patternMultiplier);
    auto const bitIndex = (_patternPhase / multiplier) % 8;
    auto const paints = (pen.pattern >> (7 - bitIndex)) & 1u;
    ++_patternPhase;
    return paints != 0;
}

void ReGISRasterizer::plotDot(Pen const& pen, point p) noexcept
{
    // A dot always paints regardless of line pattern.
    stamp(pen, p.x, p.y);
}

void ReGISRasterizer::plotLine(Pen const& pen, point from, point to) noexcept
{
    // Zingl's integer Bresenham line, consuming one pattern step per pixel.
    auto x0 = from.x;
    auto y0 = from.y;
    auto const x1 = to.x;
    auto const y1 = to.y;
    auto const dx = std::abs(x1 - x0);
    auto const dy = -std::abs(y1 - y0);
    auto const sx = x0 < x1 ? 1 : -1;
    auto const sy = y0 < y1 ? 1 : -1;
    auto err = dx + dy;
    // Cap the walk: endpoints derive from unbounded wire coordinates, so a line to a far-off point
    // would spin here. MaxPlotSteps far exceeds the on-canvas span, so the visible line is unaffected.
    for (auto stepsLeft = MaxPlotSteps; stepsLeft-- > 0;)
    {
        if (patternPaintsNext(pen))
            stamp(pen, x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        auto const e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void ReGISRasterizer::plotCircle(Pen const& pen, point center, int radius) noexcept
{
    if (radius <= 0)
    {
        stamp(pen, center.x, center.y);
        return;
    }
    // Cap the radius: it derives from unbounded wire coordinates and drives the loop length, so a huge
    // value would spin here. A radius this large is already far off the canvas.
    radius = std::min(radius, MaxPlotSteps);
    // Zingl's midpoint circle: one accumulator, plotting the four symmetric points per step.
    auto x = -radius;
    auto y = 0;
    auto err = 2 - (2 * radius);
    do
    {
        auto const emit = [&](int px, int py) {
            if (patternPaintsNext(pen))
                stamp(pen, px, py);
        };
        emit(center.x - x, center.y + y);
        emit(center.x - y, center.y - x);
        emit(center.x + x, center.y - y);
        emit(center.x + y, center.y + x);
        radius = err;
        if (radius <= y)
            err += (++y * 2) + 1;
        if (radius > x || err > y)
            err += (++x * 2) + 1;
    } while (x < 0);
}

void ReGISRasterizer::plotArc(
    Pen const& pen, point center, int radius, double startDegrees, double sweepDegrees) noexcept
{
    if (radius <= 0)
    {
        stamp(pen, center.x, center.y);
        return;
    }
    // Walk the arc in roughly one-pixel steps; y grows downward so a positive (counter-clockwise)
    // sweep subtracts from the pixel y.
    auto const start = startDegrees * std::numbers::pi / 180.0;
    auto const sweep = sweepDegrees * std::numbers::pi / 180.0;
    auto const arcLength = std::abs(sweep) * static_cast<double>(radius);
    // Cap the step count before the double->int cast: the sweep angle is unbounded wire input, so an
    // unclamped arcLength both overflows the cast (UB) and stalls the terminal in a huge cos/sin loop.
    auto const steps =
        std::max(1, static_cast<int>(std::ceil(std::min(arcLength, static_cast<double>(MaxPlotSteps)))));
    for (auto i = 0; i <= steps; ++i)
    {
        auto const a = start + (sweep * (static_cast<double>(i) / static_cast<double>(steps)));
        auto const px = center.x + static_cast<int>(std::lround(std::cos(a) * radius));
        auto const py = center.y - static_cast<int>(std::lround(std::sin(a) * radius));
        if (patternPaintsNext(pen))
            stamp(pen, px, py);
    }
}

void ReGISRasterizer::plotCurve(Pen const& pen, std::span<point const> points, bool closed) noexcept
{
    auto const n = static_cast<int>(points.size());
    if (n < 2)
    {
        if (n == 1)
            plotDot(pen, points[0]);
        return;
    }
    // Catmull-Rom interpolating spline: each segment between p1 and p2 is shaped by neighbours p0/p3.
    auto const at = [&](int i) -> point {
        if (closed)
            return points[static_cast<size_t>(((i % n) + n) % n)];
        return points[static_cast<size_t>(std::clamp(i, 0, n - 1))];
    };
    auto const segments = closed ? n : n - 1;
    for (auto i = 0; i < segments; ++i)
    {
        auto const p0 = at(i - 1);
        auto const p1 = at(i);
        auto const p2 = at(i + 1);
        auto const p3 = at(i + 2);
        auto const dx = p2.x - p1.x;
        auto const dy = p2.y - p1.y;
        auto const steps = std::clamp(static_cast<int>(std::hypot(dx, dy)), 2, MaxPlotSteps);
        auto previous = p1;
        for (auto s = 1; s <= steps; ++s)
        {
            auto const t = static_cast<double>(s) / steps;
            auto const t2 = t * t;
            auto const t3 = t2 * t;
            auto const cx = 0.5
                            * ((2.0 * p1.x) + ((-p0.x + p2.x) * t)
                               + (((2.0 * p0.x) - (5.0 * p1.x) + (4.0 * p2.x) - p3.x) * t2)
                               + ((-p0.x + (3.0 * p1.x) - (3.0 * p2.x) + p3.x) * t3));
            auto const cy = 0.5
                            * ((2.0 * p1.y) + ((-p0.y + p2.y) * t)
                               + (((2.0 * p0.y) - (5.0 * p1.y) + (4.0 * p2.y) - p3.y) * t2)
                               + ((-p0.y + (3.0 * p1.y) - (3.0 * p2.y) + p3.y) * t3));
            auto const current =
                point { .x = static_cast<int>(std::lround(cx)), .y = static_cast<int>(std::lround(cy)) };
            plotLine(pen, previous, current);
            previous = current;
        }
    }
}

void ReGISRasterizer::compositePixel(
    int x, int y, RGBColor color, WritingMode mode, uint8_t coverage) noexcept
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    if (x < 0 || y < 0 || x >= w || y >= h || coverage == 0)
        return;
    auto const i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    auto const srcAlpha = static_cast<double>(coverage) / 255.0;

    if (mode == WritingMode::Erase)
    {
        // Erase fades the pixel toward transparent by the coverage.
        _buffer[i + 3] = static_cast<uint8_t>(_buffer[i + 3] * (1.0 - srcAlpha));
        return;
    }

    auto sourceColor = color;
    if (mode == WritingMode::Complement)
        sourceColor = RGBColor { static_cast<uint8_t>(255 - _buffer[i]),
                                 static_cast<uint8_t>(255 - _buffer[i + 1]),
                                 static_cast<uint8_t>(255 - _buffer[i + 2]) };

    // Straight-alpha "over": out = src*srcA + dst*dstA*(1-srcA), normalised by the resulting alpha.
    auto const dstAlpha = static_cast<double>(_buffer[i + 3]) / 255.0;
    auto const outAlpha = srcAlpha + (dstAlpha * (1.0 - srcAlpha));
    auto const channel = [&](uint8_t src, uint8_t dst) -> uint8_t {
        if (outAlpha <= 0.0)
            return 0;
        auto const value = ((src * srcAlpha) + (dst * dstAlpha * (1.0 - srcAlpha))) / outAlpha;
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    };
    _buffer[i] = channel(sourceColor.red, _buffer[i]);
    _buffer[i + 1] = channel(sourceColor.green, _buffer[i + 1]);
    _buffer[i + 2] = channel(sourceColor.blue, _buffer[i + 2]);
    _buffer[i + 3] = static_cast<uint8_t>(std::lround(std::clamp(outAlpha * 255.0, 0.0, 255.0)));
}

void ReGISRasterizer::blendCoverage(Pen const& pen,
                                    point origin,
                                    ImageSize size,
                                    std::span<uint8_t const> coverage) noexcept
{
    auto const w = unbox<int>(size.width);
    auto const h = unbox<int>(size.height);
    for (auto ty = 0; ty < h; ++ty)
        for (auto tx = 0; tx < w; ++tx)
        {
            auto const cov = coverage[static_cast<size_t>((ty * w) + tx)];
            if (cov != 0)
                compositePixel(origin.x + tx, origin.y + ty, pen.color, pen.mode, cov);
        }
}

void ReGISRasterizer::fillPolygon(Pen const& pen, std::span<point const> points) noexcept
{
    if (points.size() < 3)
        return;
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    auto minY = points[0].y;
    auto maxY = points[0].y;
    for (auto const& p: points)
    {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    minY = std::max(minY, 0);
    maxY = std::min(maxY, h - 1);

    auto intersections = std::vector<int> {};
    for (auto y = minY; y <= maxY; ++y)
    {
        auto const scanY = static_cast<double>(y) + 0.5;
        intersections.clear();
        for (auto i = size_t { 0 }; i < points.size(); ++i)
        {
            auto const& a = points[i];
            auto const& b = points[(i + 1) % points.size()];
            auto const ay = static_cast<double>(a.y);
            auto const by = static_cast<double>(b.y);
            if ((ay <= scanY && by > scanY) || (by <= scanY && ay > scanY))
            {
                auto const t = (scanY - ay) / (by - ay);
                intersections.push_back(
                    static_cast<int>(std::lround(static_cast<double>(a.x) + (t * (b.x - a.x)))));
            }
        }
        std::ranges::sort(intersections);
        for (auto k = size_t { 0 }; k + 1 < intersections.size(); k += 2)
            // Clamp the span to the canvas: the intersection X derives from unbounded wire vertices,
            // and off-canvas pixels are no-ops, so an unclamped span would iterate billions of times.
            for (auto x = std::max(0, intersections[k]); x <= std::min(w - 1, intersections[k + 1]); ++x)
                blend(x, y, pen.color, pen.mode);
    }
}

} // namespace vtbackend::regis
