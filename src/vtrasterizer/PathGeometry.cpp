// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/PathGeometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <utility>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    constexpr auto Pi = std::numbers::pi_v<float>;

    /// The most lines one curve, or one round cap or join, is flattened into, however large it is drawn.
    constexpr auto MaxSteps = 128;

    /// Below this, two directions count as the same one.
    constexpr auto Parallel = 1e-6F;

    [[nodiscard]] constexpr PathPoint operator+(PathPoint a, PathPoint b) noexcept
    {
        return { .x = a.x + b.x, .y = a.y + b.y };
    }

    [[nodiscard]] constexpr PathPoint operator-(PathPoint a, PathPoint b) noexcept
    {
        return { .x = a.x - b.x, .y = a.y - b.y };
    }

    [[nodiscard]] constexpr PathPoint operator*(PathPoint a, float scale) noexcept
    {
        return { .x = a.x * scale, .y = a.y * scale };
    }

    [[nodiscard]] constexpr float dot(PathPoint a, PathPoint b) noexcept
    {
        return (a.x * b.x) + (a.y * b.y);
    }

    [[nodiscard]] constexpr float cross(PathPoint a, PathPoint b) noexcept
    {
        return (a.x * b.y) - (a.y * b.x);
    }

    [[nodiscard]] float length(PathPoint a) noexcept
    {
        return std::hypot(a.x, a.y);
    }

    /// @return @p direction turned a quarter of a turn: the side of a line a stroke's edge is built on.
    [[nodiscard]] constexpr PathPoint normalOf(PathPoint direction) noexcept
    {
        return { .x = -direction.y, .y = direction.x };
    }

    [[nodiscard]] constexpr PathPoint pointOf(VectorPoint point) noexcept
    {
        return { .x = point.x, .y = point.y };
    }

    /// @return How many lines stand in for a curve that strays @p deviation from one, to within @p tolerance.
    [[nodiscard]] int stepsFor(float deviation, float tolerance) noexcept
    {
        if (!(deviation > tolerance))
            return 1;
        return std::clamp(static_cast<int>(std::ceil(std::sqrt(deviation / tolerance))), 1, MaxSteps);
    }

    /// Collects the subpaths of a path as they are flattened.
    class Flattener
    {
      public:
        explicit Flattener(float tolerance) noexcept: _tolerance { tolerance } {}

        void moveTo(PathPoint to)
        {
            finish();
            _current.points.push_back(to);
        }

        void lineTo(PathPoint to)
        {
            _drawn = true;
            if (_current.points.empty() || _current.points.back() != to)
                _current.points.push_back(to);
        }

        void quadTo(PathPoint control, PathPoint to)
        {
            auto const from = pen();
            // A quadratic strays from its chord by a quarter of its second difference, at most.
            auto const steps = stepsFor(length(from - (control * 2) + to) / 4, _tolerance);
            for (auto const step: std::views::iota(1, steps))
            {
                auto const t = static_cast<float>(step) / static_cast<float>(steps);
                auto const u = 1 - t;
                lineTo((from * (u * u)) + (control * (2 * u * t)) + (to * (t * t)));
            }
            lineTo(to);
        }

        void cubicTo(PathPoint control1, PathPoint control2, PathPoint to)
        {
            auto const from = pen();
            // Wang's bound for a cubic: three quarters of its larger second difference.
            auto const deviation =
                std::max(length(from - (control1 * 2) + control2), length(control1 - (control2 * 2) + to));
            auto const steps = stepsFor(deviation * 0.75F, _tolerance);
            for (auto const step: std::views::iota(1, steps))
            {
                auto const t = static_cast<float>(step) / static_cast<float>(steps);
                auto const u = 1 - t;
                lineTo((from * (u * u * u)) + (control1 * (3 * u * u * t)) + (control2 * (3 * u * t * t))
                       + (to * (t * t * t)));
            }
            lineTo(to);
        }

        void close()
        {
            _drawn = true;
            _current.closure = PathClosure::Closed;
            finish();
        }

        [[nodiscard]] std::vector<Polyline> take()
        {
            finish();
            return std::move(_polylines);
        }

      private:
        [[nodiscard]] PathPoint pen() const noexcept
        {
            return _current.points.empty() ? PathPoint {} : _current.points.back();
        }

        /// Ends the subpath being collected. One that only moved draws nothing and is dropped.
        void finish()
        {
            if (_drawn && !_current.points.empty())
            {
                // The closing line is implied: a last point equal to the first says it twice.
                if (_current.closure == PathClosure::Closed && _current.points.size() > 1
                    && _current.points.back() == _current.points.front())
                    _current.points.pop_back();
                _polylines.push_back(std::move(_current));
            }
            _current = Polyline {};
            _drawn = false;
        }

        float _tolerance;
        std::vector<Polyline> _polylines;
        Polyline _current;
        bool _drawn = false; ///< Whether the subpath has a segment besides its moveto.
    };

    /// Builds the outlines of what a stroke covers.
    class Outliner
    {
      public:
        Outliner(VectorStrokeStyle const& style, float tolerance) noexcept:
            _style { style },
            _half { style.width / 2 },
            // The angle a line may span on a circle of the stroke's half width and stay within tolerance.
            _arcStep { tolerance < _half ? std::clamp(2 * std::acos(1 - (tolerance / _half)), 0.05F, Pi / 2)
                                         : Pi / 2 }
        {
        }

        void add(Polyline const& polyline)
        {
            auto const& points = polyline.points;
            if (points.empty())
                return;
            if (points.size() == 1)
                addDot(points.front());
            else if (polyline.closure == PathClosure::Closed)
            {
                // Its two edges, each a loop of its own: one along the path, one against it.
                addLoop(points);
                auto reversed = points;
                std::ranges::reverse(reversed);
                addLoop(reversed);
            }
            else
            {
                // One outline around it: along one side, around the end, back along the other, around the
                // start. The way back is the way there of the same line walked backwards.
                auto outline = Polyline { .points = {}, .closure = PathClosure::Closed };
                auto reversed = points;
                std::ranges::reverse(reversed);
                addSide(points, outline.points);
                addCap(points.back(), directionOf(points[points.size() - 2], points.back()), outline.points);
                addSide(reversed, outline.points);
                addCap(reversed.back(),
                       directionOf(reversed[reversed.size() - 2], reversed.back()),
                       outline.points);
                _outlines.push_back(std::move(outline));
            }
        }

        [[nodiscard]] std::vector<Polyline> take() noexcept { return std::move(_outlines); }

      private:
        [[nodiscard]] static PathPoint directionOf(PathPoint from, PathPoint to) noexcept
        {
            auto const delta = to - from;
            auto const distance = length(delta);
            return distance > 0 ? delta * (1 / distance) : PathPoint { .x = 1, .y = 0 };
        }

        /// One line of a polyline: where it ends, which way it runs and how long it is.
        struct Line
        {
            Line(PathPoint from, PathPoint end) noexcept:
                to { end }, direction { directionOf(from, end) }, length { vtrasterizer::length(end - from) }
            {
            }

            PathPoint to;
            PathPoint direction;
            float length;
        };

        /// Adds the points of an arc around @p center, from the angle @p start over @p sweep, leaving out
        /// the point it starts at.
        void addArc(PathPoint center, float start, float sweep, std::vector<PathPoint>& out) const
        {
            auto const steps =
                std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / _arcStep)), 1, MaxSteps);
            for (auto const step: std::views::iota(1, steps + 1))
            {
                auto const angle = start + (sweep * static_cast<float>(step) / static_cast<float>(steps));
                out.push_back(center + PathPoint { .x = std::cos(angle), .y = std::sin(angle) } * _half);
            }
        }

        /// A subpath of no length shows what its caps draw: a dot, a square, or nothing.
        void addDot(PathPoint at)
        {
            auto outline = Polyline { .points = {}, .closure = PathClosure::Closed };
            switch (_style.cap)
            {
                case VectorLineCap::Butt: return;
                case VectorLineCap::Round:
                    // Wound as every other outline is, which is against the angle.
                    addArc(at, 0, -2 * Pi, outline.points);
                    break;
                case VectorLineCap::Square:
                    outline.points = { at + PathPoint { .x = -_half, .y = _half },
                                       at + PathPoint { .x = _half, .y = _half },
                                       at + PathPoint { .x = _half, .y = -_half },
                                       at + PathPoint { .x = -_half, .y = -_half } };
                    break;
            }
            _outlines.push_back(std::move(outline));
        }

        /// Adds the edge of the stroke on the normal's side of an open line, from its first point's offset
        /// to its last point's.
        void addSide(std::span<PathPoint const> points, std::vector<PathPoint>& out) const
        {
            out.push_back(points[0] + (normalOf(directionOf(points[0], points[1])) * _half));
            for (auto const index: std::views::iota(size_t { 1 }, points.size()))
            {
                auto const incoming = Line { points[index - 1], points[index] };
                out.push_back(points[index] + (normalOf(incoming.direction) * _half));
                if (index + 1 < points.size())
                    addJoin(points[index], incoming, Line { points[index], points[index + 1] }, out);
            }
        }

        /// Adds one edge of a closed line's stroke: the one on the normal's side, all the way around.
        void addLoop(std::span<PathPoint const> points)
        {
            auto outline = Polyline { .points = {}, .closure = PathClosure::Closed };
            auto const count = points.size();
            for (auto const index: std::views::iota(size_t { 0 }, count))
            {
                // Each line's offset ends at the corner after it, and the join there starts the next
                // one's: around a loop, that is every point of it.
                auto const incoming = Line { points[index], points[(index + 1) % count] };
                auto const outgoing = Line { points[(index + 1) % count], points[(index + 2) % count] };
                outline.points.push_back(incoming.to + (normalOf(incoming.direction) * _half));
                addJoin(incoming.to, incoming, outgoing, outline.points);
            }
            _outlines.push_back(std::move(outline));
        }

        /// Turns the corner at @p at on the normal's side, from the offset of the line arriving to the
        /// offset of the line leaving. The first of those is already in @p out.
        void addJoin(PathPoint at,
                     Line const& arriving,
                     Line const& departing,
                     std::vector<PathPoint>& out) const
        {
            auto const incoming = arriving.direction;
            auto const outgoing = departing.direction;
            auto const turn = cross(incoming, outgoing);
            auto const along = dot(incoming, outgoing);
            auto const leaving = at + (normalOf(outgoing) * _half);
            if (turn > Parallel)
            {
                // The inside of the corner, where the two offsets cross. How far back from the corner
                // they do is what each line must have to spare, for the corner at its other end too.
                auto const reach = _half * turn / (1 + along);
                if (along > -1 + Parallel && reach <= std::min(arriving.length, departing.length) / 2)
                {
                    // Where they cross: the edge turns there, exactly. A curve's inner edge is made of
                    // these.
                    out.back() = at + ((normalOf(incoming) + normalOf(outgoing)) * (_half / (1 + along)));
                    return;
                }
                // Lines too short for that: through the corner itself, so that the two offsets overlap
                // inside the stroke and nowhere else.
                out.push_back(at);
                out.push_back(leaving);
                return;
            }
            if (turn > -Parallel && along > 0)
            {
                // Straight on.
                out.push_back(leaving);
                return;
            }
            // The outside of the corner, a line doubling back included.
            switch (_style.join)
            {
                case VectorLineJoin::Bevel: break;
                case VectorLineJoin::Round: {
                    auto const normal = normalOf(incoming);
                    auto const sweep = std::acos(std::clamp(along, -1.0F, 1.0F));
                    addArc(at, std::atan2(normal.y, normal.x), -sweep, out);
                    return;
                }
                case VectorLineJoin::Miter: {
                    // The corner's tip, when it is no longer than the limit allows: as SVG has it, the
                    // ratio of its length to the stroke's width.
                    auto const spread = 1 + along;
                    if (spread > Parallel && std::sqrt(2 / spread) <= _style.miterLimit)
                        out.push_back(at + ((normalOf(incoming) + normalOf(outgoing)) * (_half / spread)));
                    break;
                }
            }
            out.push_back(leaving);
        }

        /// Goes around the end of an open line, from the normal's side to the other. The offset on the
        /// normal's side is already in @p out.
        void addCap(PathPoint at, PathPoint direction, std::vector<PathPoint>& out) const
        {
            auto const normal = normalOf(direction);
            switch (_style.cap)
            {
                case VectorLineCap::Butt: break;
                case VectorLineCap::Square:
                    out.push_back(at + ((normal + direction) * _half));
                    out.push_back(at + ((direction - normal) * _half));
                    break;
                case VectorLineCap::Round:
                    // Half a turn, by way of the line's own direction.
                    addArc(at, std::atan2(normal.y, normal.x), -Pi, out);
                    return;
            }
            out.push_back(at - (normal * _half));
        }

        VectorStrokeStyle const& _style;
        float _half;
        float _arcStep;
        std::vector<Polyline> _outlines;
    };
} // namespace

std::optional<Affine> inverse(Affine const& map) noexcept
{
    auto const determinant = (map.a * map.d) - (map.b * map.c);
    if (!std::isfinite(determinant) || determinant == 0)
        return std::nullopt;
    auto const a = map.d / determinant;
    auto const b = -map.b / determinant;
    auto const c = -map.c / determinant;
    auto const d = map.a / determinant;
    return Affine {
        .a = a, .b = b, .c = c, .d = d, .e = -((a * map.e) + (c * map.f)), .f = -((b * map.e) + (d * map.f))
    };
}

float largestStretch(Affine const& map) noexcept
{
    // The larger of the two singular values of its linear part.
    auto const sum = (map.a * map.a) + (map.b * map.b) + (map.c * map.c) + (map.d * map.d);
    auto const determinant = (map.a * map.d) - (map.b * map.c);
    return std::sqrt((sum + std::sqrt(std::max(0.0F, (sum * sum) - (4 * determinant * determinant)))) / 2);
}

std::vector<Polyline> flattenPath(std::span<VectorSegment const> segments, float tolerance)
{
    auto flattener = Flattener { tolerance };
    for (auto const& segment: segments)
    {
        auto const& points = segment.points;
        switch (segment.kind)
        {
            case VectorSegmentKind::MoveTo: flattener.moveTo(pointOf(points[0])); break;
            case VectorSegmentKind::LineTo: flattener.lineTo(pointOf(points[0])); break;
            case VectorSegmentKind::QuadTo: flattener.quadTo(pointOf(points[0]), pointOf(points[1])); break;
            case VectorSegmentKind::CubicTo:
                flattener.cubicTo(pointOf(points[0]), pointOf(points[1]), pointOf(points[2]));
                break;
            case VectorSegmentKind::Close: flattener.close(); break;
        }
    }
    return flattener.take();
}

std::vector<Polyline> strokePolylines(std::span<Polyline const> polylines,
                                      VectorStrokeStyle const& style,
                                      float tolerance)
{
    auto outliner = Outliner { style, tolerance };
    for (auto const& polyline: polylines)
        outliner.add(polyline);
    return outliner.take();
}

void transformPolylines(std::span<Polyline> polylines, Affine const& map) noexcept
{
    for (auto& polyline: polylines)
        for (auto& point: polyline.points)
            point = map.apply(point);
}

} // namespace vtrasterizer
